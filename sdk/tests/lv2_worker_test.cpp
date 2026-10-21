// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the LV2 worker, driven the way a real host drives it.
//
// LV2 gives a plugin two threads besides the audio one: the host's, which
// calls save() and restore(), and the worker, which calls work(). The DSP's
// loadFile(), saveState() and loadState() are [main-thread] by the wrapper's
// contract -- one thread, never two at once -- and in CLAP and VST3 they get
// exactly that. In LV2 the worker loaded a file while the host saved the
// session, so a DSP's file path was read and written at once.
//
// And a real host runs work() for the NEXT message while the response to the
// previous one is still queued for the audio thread. Both work() calls wrote
// one shared string; the responses swapped it in turn, and the second swap
// put back the state from before either load.
//
// lv2_host_test drives the worker inline, one message at a time, and could
// see neither. This compiles the wrapper in, so the plugin's own interface
// state can be read after the host's calls.
#define SONORE_NUM_PARAMS 1

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include <sonore/dsp.h>
#include <sonore/state_bag.h>

// Heap blocks still live, counted while g_countHeap is set: what an
// instance's life leaves behind (see the dropped-answer check at the end).
static std::atomic<long> g_heapLive{0};
static std::atomic<bool> g_countHeap{false};
void* operator new(size_t size) {
  void* p = std::malloc(size ? size : 1);
  if (!p) throw std::bad_alloc();
  if (g_countHeap.load(std::memory_order_relaxed)) ++g_heapLive;
  return p;
}
void operator delete(void* p) noexcept {
  if (p && g_countHeap.load(std::memory_order_relaxed)) --g_heapLive;
  std::free(p);
}
void operator delete(void* p, size_t) noexcept { operator delete(p); }

namespace {
std::atomic<int> g_inside{0};
std::atomic<int> g_overlaps{0};

/** Held for a moment, so that two threads let in together are seen to be.
 *  A spin, not a sleep: a Windows sleep is a 15 ms timer tick. */
struct Inside {
  Inside() {
    if (g_inside.fetch_add(1) != 0) g_overlaps.fetch_add(1);
    const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(200);
    while (std::chrono::steady_clock::now() < until) {
    }
  }
  ~Inside() { g_inside.fetch_sub(1); }
};
} // namespace

struct SonoreDsp {
  std::string file;
  void prepare(const sonore::ProcessSpec&) { Inside in; }
  void process(sonore::AudioBlock<float>&, const float*) {}
  void loadFile(const char*, const char* path) {
    Inside in;
    file = path;
  }
  void saveState(sonore::StateBag& bag) const {
    Inside in;
    bag.setString("file", file);
  }
  void loadState(const sonore::StateBag& bag) {
    Inside in;
    file = bag.getString("file", "");
  }
};

#include <sonore/plugin.h>

static const sonore::ParamInfo kParamTable[SONORE_NUM_PARAMS] = {
    {"unused", "Unused", "", 0.0f, 1.0f, 0.0f, 0},
};

static const sonore::PluginDescriptor kDesc = {
    "com.sonorie.test.lv2worker",
    "Sonore LV2 Worker Fixture",
    "Sonorie",
    "1.0.0",
    "Loads a file on the LV2 worker.",
    "https://sonorie.com",
    false,
    kParamTable,
    SONORE_NUM_PARAMS,
    nullptr,
    0,
    "utility",
};

#include <sonore/clap_wrapper.h>
#include <sonore/lv2_wrapper.h>

using namespace sonore::lv2;

namespace {
int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

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

/** What respond() was handed, kept for delivery later -- as a host's ring
 *  between the worker and the audio thread keeps it. */
std::mutex g_responsesLock;
std::vector<std::vector<uint8_t>> g_responses;

Lv2WorkerStatus respondCb(Lv2WorkerRespondHandle, uint32_t size, const void* data) {
  std::lock_guard<std::mutex> hold(g_responsesLock);
  const auto* p = (const uint8_t*) data;
  g_responses.emplace_back(p, p + size);
  return kLv2WorkerSuccess;
}

Lv2WorkerStatus scheduleCb(Lv2WorkerScheduleHandle, uint32_t, const void*) {
  return kLv2WorkerSuccess;
}

void deliverResponses(const Lv2WorkerInterface* worker, LV2_Handle handle) {
  std::lock_guard<std::mutex> hold(g_responsesLock);
  for (const auto& r : g_responses)
    worker->work_response(handle, (uint32_t) r.size(), r.empty() ? nullptr : r.data());
  g_responses.clear();
}

/** "file\0<path>", the payload the interface's loadFile atom carries. */
std::vector<uint8_t> loadPayload(const std::string& path) {
  std::vector<uint8_t> out;
  const char purpose[] = "file";
  out.insert(out.end(), purpose, purpose + sizeof(purpose)); // with its terminator
  out.insert(out.end(), path.begin(), path.end());           // without one, as sent
  return out;
}

std::vector<uint8_t> g_blob;
uint32_t g_blobKey = 0, g_blobType = 0;

LV2_State_Status storeCb(LV2_State_Handle, uint32_t key, const void* value, size_t size,
                         uint32_t type, uint32_t) {
  const auto* p = (const uint8_t*) value;
  g_blob.assign(p, p + size);
  g_blobKey = key;
  g_blobType = type;
  return LV2_STATE_SUCCESS;
}

const void* retrieveCb(LV2_State_Handle, uint32_t key, size_t* size, uint32_t* type,
                       uint32_t* flags) {
  if (key != g_blobKey || g_blob.empty()) return nullptr;
  *size = g_blob.size();
  *type = g_blobType;
  *flags = LV2_STATE_IS_POD;
  return g_blob.data();
}
} // namespace

int main() {
  std::printf("lv2_worker_test: the worker against the host's thread, and queued responses\n");

  UridMap urids;
  LV2_URID_Map map{&urids, UridMap::mapCb};
  LV2_Feature mapFeature{LV2_URID__map, &map};
  Lv2WorkerSchedule schedule{nullptr, scheduleCb};
  LV2_Feature workerFeature{SONORE_LV2_WORKER__schedule, &schedule};
  const LV2_Feature* features[] = {&mapFeature, &workerFeature, nullptr};

  const LV2_Descriptor* d = descriptor();
  LV2_Handle handle = d->instantiate(d, 48000.0, ".", features);
  check(handle != nullptr, "the plugin instantiates with a worker");
  if (!handle) return 1;
  auto* plugin = (Plugin*) handle;
  d->activate(handle);

  const auto* worker = (const Lv2WorkerInterface*) d->extension_data(SONORE_LV2_WORKER__interface);
  const auto* state = (const LV2_State_Interface*) d->extension_data(LV2_STATE__interface);
  check(worker && worker->work && worker->work_response, "it offers the worker interface");
  check(state && state->save && state->restore, "and the state interface");
  if (!worker || !state) return 1;

  // ── Two files, the first response still queued when the second is worked
  {
    const std::vector<uint8_t> first = loadPayload("first.wav");
    const std::vector<uint8_t> second = loadPayload("second.wav");
    worker->work(handle, respondCb, nullptr, (uint32_t) first.size(), first.data());
    worker->work(handle, respondCb, nullptr, (uint32_t) second.size(), second.data());
    deliverResponses(worker, handle);
    const bool showsSecond = plugin->uiStateJson.find("second.wav") != std::string::npos;
    std::printf("  ---- after both responses the interface holds %s ----\n",
                plugin->uiStateJson.c_str());
    check(showsSecond,
          "two loads with the first response still queued: the interface shows the second file");
  }

  // ── A response still queued when the host restores a state ──────────────
  //
  // The worker loads a file and queues its answer for the audio thread; the
  // host then restores a session -- between two runs, which is where LV2
  // puts restore() -- and only after that delivers the answer. The DSP holds
  // the restored file: restore() came second. The interface must show the
  // same, not the file from before the restore.
  {
    state->save(handle, storeCb, nullptr, 0, nullptr); // holds second.wav
    const std::vector<uint8_t> late = loadPayload("late.wav");
    worker->work(handle, respondCb, nullptr, (uint32_t) late.size(), late.data());
    state->restore(handle, retrieveCb, nullptr, 0, nullptr);
    deliverResponses(worker, handle);
    const bool dspRestored = plugin->shared.dsp.file == "second.wav";
    const bool shownRestored = plugin->uiStateJson.find("second.wav") != std::string::npos;
    std::printf("  ---- the DSP holds %s, the interface %s ----\n",
                plugin->shared.dsp.file.c_str(), plugin->uiStateJson.c_str());
    check(dspRestored && shownRestored,
          "a load answered after a restore does not put the old file back on the interface");
    // And a load AFTER the restore is still shown when it is answered.
    const std::vector<uint8_t> after = loadPayload("after.wav");
    worker->work(handle, respondCb, nullptr, (uint32_t) after.size(), after.data());
    deliverResponses(worker, handle);
    check(plugin->uiStateJson.find("after.wav") != std::string::npos,
          "...while a load made after it is shown when it is answered");
  }

  // ── The worker loading while the host saves and restores ───────────────
  state->save(handle, storeCb, nullptr, 0, nullptr);
  check(!g_blob.empty(), "the state saves");
  {
    g_overlaps.store(0);
    std::atomic<bool> done{false};
    constexpr int kLoads = 150;
    std::thread host([&] {
      while (!done.load()) {
        state->save(handle, storeCb, nullptr, 0, nullptr);
        state->restore(handle, retrieveCb, nullptr, 0, nullptr);
      }
    });
    for (int i = 0; i < kLoads; ++i) {
      const std::vector<uint8_t> payload = loadPayload("loop" + std::to_string(i) + ".wav");
      worker->work(handle, respondCb, nullptr, (uint32_t) payload.size(), payload.data());
    }
    done.store(true);
    host.join();
    deliverResponses(worker, handle);
    std::printf("  ---- %d DSP calls overlapped across %d loads ----\n", g_overlaps.load(),
                kLoads);
    check(g_overlaps.load() == 0,
          "the worker's loadFile never runs at the same time as the host's save or restore");
    // On the file the DSP holds -- which is the last one loaded unless a
    // restore of a state saved before that load came after it, and then the
    // restored one. It was "the last file loaded" whatever the DSP held,
    // and that passed only because a queued response put a file back on the
    // interface over the restore that had replaced it.
    std::printf("  ---- the DSP holds %s, the interface %s ----\n",
                plugin->shared.dsp.file.c_str(), plugin->uiStateJson.c_str());
    check(plugin->uiStateJson.find("'" + plugin->shared.dsp.file + "'") != std::string::npos,
          "and the interface ends on the file the DSP holds");
  }

  // ── The worker loading while run() re-prepares for a freewheel flip ─────
  //
  // A flip of lv2:freeWheeling re-runs prepare() from run() -- the only way
  // a DSP with the simple process() signature hears of it -- and run() never
  // took its turn: prepare() ran beside the worker's loadFile() and the
  // host's save, which take theirs. The host flips the port every run here,
  // so the rare meeting of a bounce starting and a file arriving is every
  // block rather than once a session.
  {
    constexpr uint32_t kFrames = 64;
    std::vector<float> in((size_t) kFrames * 2, 0.0f), out((size_t) kFrames * 2, 0.0f);
    float freeWheel = 0.0f;
    for (int c = 0; c < lv2Width(); ++c) {
      d->connect_port(handle, (uint32_t) (portAudioInL() + c), &in[(size_t) c * kFrames]);
      d->connect_port(handle, (uint32_t) (portAudioOutL() + c), &out[(size_t) c * kFrames]);
    }
    d->connect_port(handle, (uint32_t) portFreeWheel(), &freeWheel);
    g_overlaps.store(0);
    std::atomic<bool> done{false};
    std::atomic<int> flips{0};
    std::thread audio([&] {
      while (!done.load()) {
        freeWheel = freeWheel >= 0.5f ? 0.0f : 1.0f;
        d->run(handle, kFrames);
        ++flips;
      }
    });
    constexpr int kLoads = 150;
    for (int i = 0; i < kLoads; ++i) {
      const std::vector<uint8_t> payload = loadPayload("wheel" + std::to_string(i) + ".wav");
      worker->work(handle, respondCb, nullptr, (uint32_t) payload.size(), payload.data());
    }
    done.store(true);
    audio.join();
    deliverResponses(worker, handle);
    std::printf("  ---- %d DSP calls overlapped across %d loads and %d flipped runs ----\n",
                g_overlaps.load(), kLoads, flips.load());
    check(g_overlaps.load() == 0,
          "the worker's loadFile never runs at the same time as a freewheel re-prepare");
    // And the flip is not lost by taking turns: the port's last value is
    // what the plugin ends on once a run finds the worker gone.
    freeWheel = 1.0f;
    d->run(handle, kFrames);
    d->run(handle, kFrames);
    check(plugin->shared.offline, "a flip that waited for its turn is made at the next run");
  }

  d->deactivate(handle);
  d->cleanup(handle);

  // ── An answer the host never delivers ──────────────────────────────────
  //
  // A host drops what its worker queue still holds when an instance goes:
  // host.h on every prepare(), which makes a new instance, and jalv at exit.
  // The text work() built for that answer was reachable only through it,
  // and went with the queue -- two heap blocks a load. One instance's life
  // with its answer delivered, and one with it dropped, after a first pair
  // that fills every cache and static: neither may leave anything behind.
  {
    auto life = [&](bool deliver) {
      LV2_Handle h = d->instantiate(d, 48000.0, ".", features);
      d->activate(h);
      const std::vector<uint8_t> payload = loadPayload("dropped.wav");
      worker->work(h, respondCb, nullptr, (uint32_t) payload.size(), payload.data());
      if (deliver) deliverResponses(worker, h);
      d->deactivate(h);
      d->cleanup(h);
      std::lock_guard<std::mutex> hold(g_responsesLock);
      g_responses.clear(); // the host's queue goes with the instance
      g_responses.shrink_to_fit();
    };
    life(true);
    life(false);
    g_countHeap = true;
    life(true);
    const long delivered = g_heapLive.exchange(0);
    life(false);
    const long dropped = g_heapLive.exchange(0);
    g_countHeap = false;
    char line[200];
    std::snprintf(line, sizeof(line),
                  "an instance cleaned up with its worker's answer undelivered frees what it built "
                  "for it (%ld block(s) left; %ld when delivered)",
                  dropped, delivered);
    check(delivered == 0 && dropped == 0, line);
  }

  std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
