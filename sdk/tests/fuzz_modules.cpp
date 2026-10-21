// SPDX-License-Identifier: Apache-2.0
// Mutation fuzzing for what the modules read that they did not write.
//
//   analytics  ThreadedAnalyticsDestination's unsent-events file. It sits on
//              the user's disk between sessions, where a crash can truncate
//              it and anything can edit it, and the next session reads it
//              before it sends anything. Seeds are files the SDK's own writer
//              produced; every mutation is one step from valid. The verdict
//              is stronger than "came back": every event the parser accepts
//              must satisfy the limits it promises (a name, a finite time,
//              bounded strings and parameter counts), and writing the
//              accepted events out and parsing them again must give exactly
//              the same events -- a parser that accepts something its own
//              writer cannot reproduce has accepted something it did not
//              understand. Every 64th case also goes through a real file and
//              a destination's start(), the path a user's machine takes.
//   video      (Windows) a 1-second H.264 + AAC file from Media Foundation's
//              encoder, with bytes flipped and the file cut short, opened by
//              VideoReader and decoded to the end, picture and sound. The
//              container and codec parsing is Microsoft's; what is under
//              test is this SDK's side -- strides, crops and sizes taken
//              from whatever the decoder reports about a damaged stream,
//              and copying out of buffers that may be shorter than claimed.
//
//   fuzz_modules                          both, a quick pass (ctest)
//   fuzz_modules --iterations 20000       the deep pass, under ASan
//   fuzz_modules --target analytics       one of them
//
// A watchdog turns a hang into a named failure, and a crash writes the input
// beside the process before the sanitizer reports it.
#include <sonore/analytics.h>
#include <sonore/gfx/video.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include "video_test_file.h"
#endif

using namespace sonore;

namespace {

std::atomic<long long> g_deadlineMs{0};
std::atomic<bool> g_done{false};
const char* g_target = "";
const char* g_phase = "";
size_t g_case = 0;
std::string g_current;

long long nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void saveCurrent(const char* name) {
  if (std::FILE* f = std::fopen(name, "wb")) {
    if (!g_current.empty()) std::fwrite(g_current.data(), 1, g_current.size(), f);
    std::fclose(f);
  }
}

using SignalHandler = void (*)(int);
SignalHandler g_previous[64] = {};

void onCrash(int sig) {
  saveCurrent("fuzz_modules_crash.bin");
  std::fprintf(stderr, "  CRASH signal %d in %s phase=%s case=%zu size=%zu -- the input is in fuzz_modules_crash.bin\n",
               sig, g_target, g_phase, g_case, g_current.size());
  std::fflush(stderr);
  const SignalHandler previous = (sig >= 0 && sig < 64) ? g_previous[sig] : nullptr;
  if (previous && previous != SIG_DFL && previous != SIG_IGN) {
    std::signal(sig, previous);
    std::raise(sig);
  }
  std::_Exit(98);
}

void watchCrashes() {
  const int signals[] = {SIGSEGV, SIGABRT, SIGILL, SIGFPE};
  for (int sig : signals) {
    const SignalHandler previous = std::signal(sig, onCrash);
    if (sig >= 0 && sig < 64) g_previous[sig] = previous == SIG_ERR ? nullptr : previous;
  }
}

void watchdog() {
  while (!g_done.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const long long deadline = g_deadlineMs.load();
    if (deadline != 0 && nowMs() > deadline) {
      std::fprintf(stderr, "  HANG %s phase=%s case=%zu size=%zu -- the input is in fuzz_modules_hang.bin\n",
                   g_target, g_phase, g_case, g_current.size());
      saveCurrent("fuzz_modules_hang.bin");
      std::fflush(stderr);
      std::_Exit(99);
    }
  }
}

int g_failures = 0;

void fail(const char* what) {
  if (g_failures < 20)
    std::printf("  FAIL %s phase=%s case=%zu: %s\n", g_target, g_phase, g_case, what);
  ++g_failures;
}

std::string tempPath(const std::string& leaf) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  return (std::filesystem::temp_directory_path() / ("sonore-fuzz-modules-" + std::to_string(stamp) + "-" + leaf))
      .u8string();
}

// ═════════════════════════════════════════════════════════════════════════════
// Mutations
// ═════════════════════════════════════════════════════════════════════════════

std::string mutate(const std::string& seed, const std::vector<std::string>& seeds, std::mt19937& rng) {
  std::string b = seed;
  const int kind = (int) (rng() % 7);
  auto pos = [&](size_t n) { return n ? (size_t) (rng() % n) : (size_t) 0; };
  switch (kind) {
    case 0: { // flips
      const int n = 1 + (int) (rng() % 8);
      for (int k = 0; k < n && !b.empty(); ++k) b[pos(b.size())] = (char) (rng() & 0xFF);
      break;
    }
    case 1: { // the bytes JSON's structure and escapes are made of
      static const char structural[] = "{}[]\",:\\0123456789-+.eEtrufalsn \x00\x7f\xc3\xa9\xff";
      const int n = 1 + (int) (rng() % 6);
      for (int k = 0; k < n; ++k) {
        const char c = structural[rng() % (sizeof(structural) - 1)];
        if (rng() % 2 && !b.empty()) b[pos(b.size())] = c;
        else b.insert(b.begin() + (std::ptrdiff_t) pos(b.size() + 1), c);
      }
      break;
    }
    case 2: { // delete a range
      if (b.empty()) break;
      const size_t at = pos(b.size());
      b.erase(at, 1 + pos((std::min)((size_t) 64, b.size() - at)));
      break;
    }
    case 3: { // duplicate a range in place
      if (b.empty()) break;
      const size_t at = pos(b.size());
      const std::string piece = b.substr(at, 1 + pos(128));
      b.insert(pos(b.size() + 1), piece);
      break;
    }
    case 4: { // splice the head of this seed onto the tail of another
      const std::string& other = seeds[rng() % seeds.size()];
      b = b.substr(0, pos(b.size() + 1)) + other.substr(pos(other.size() + 1));
      break;
    }
    case 5: { // numbers at the edges of what a double or an int holds
      static const char* numbers[] = {"1e309", "-1e309", "2147483648", "-2147483649", "1e-400", "0.5",
                                      "NaN", "-0", "99999999999999999999", "1.7976931348623157e308"};
      const char* n = numbers[rng() % (sizeof(numbers) / sizeof(numbers[0]))];
      const size_t at = b.find_first_of("0123456789", pos(b.size()));
      if (at != std::string::npos) {
        size_t end = at;
        while (end < b.size() && std::strchr("0123456789.eE+-", b[end])) ++end;
        b.replace(at, end - at, n);
      }
      break;
    }
    default: { // truncate, then flip
      b.resize(pos(b.size() + 1));
      if (!b.empty()) b[pos(b.size())] = (char) (rng() & 0xFF);
      break;
    }
  }
  return b;
}

// ═════════════════════════════════════════════════════════════════════════════
// analytics
// ═════════════════════════════════════════════════════════════════════════════

std::vector<std::string> analyticsSeeds() {
  using D = ThreadedAnalyticsDestination;
  std::vector<std::string> seeds;
  seeds.push_back(D::serialiseEvents({}));
  std::deque<AnalyticsEvent> one(1);
  one[0].name = "preset_saved";
  one[0].timestamp = 1700000000.5;
  seeds.push_back(D::serialiseEvents(one));
  std::deque<AnalyticsEvent> many;
  for (int i = 0; i < 12; ++i) {
    AnalyticsEvent e;
    e.name = "event " + std::to_string(i) + (i % 3 ? "" : " \xc3\xa9\xe4\xb8\xad\xf0\x9f\x8e\xb5");
    e.eventType = i * 1000 - 3;
    e.timestamp = 1700000000.0 + i * 0.001;
    e.userId = i % 2 ? "user-\"quoted\"" : "";
    for (int k = 0; k < i % 5; ++k) e.parameters["p" + std::to_string(k)] = std::string((size_t) k * 3, (char) ('a' + k));
    e.parameters["escapes"] = "\\ \n \t \x01 \x7f /";
    if (i % 4 == 0) e.userProperties["tier"] = "pro";
    many.push_back(e);
  }
  seeds.push_back(D::serialiseEvents(many));
  // Hand-written, the way an older or a foreign writer might have it: fields
  // missing, extra fields, whitespace, nulls.
  seeds.push_back(
      "{ \"version\" : 1, \"format\" : \"sonore-analytics-events\", \"extra\": [1,{\"x\":null}],\n"
      "  \"events\" : [ { \"name\" : \"a\", \"time\" : 0 }, { \"name\" : \"b\", \"time\" : -1.5e3,"
      " \"type\": null, \"user\": null, \"parameters\": null, \"userProperties\": {} } ] }");
  return seeds;
}

bool eventIsWithinLimits(const AnalyticsEvent& e) {
  using D = ThreadedAnalyticsDestination;
  if (e.name.empty() || e.name.size() > D::kMaxStringBytes) return false;
  if (!std::isfinite(e.timestamp)) return false;
  if (e.userId.size() > D::kMaxStringBytes) return false;
  if (e.parameters.size() > D::kMaxParametersPerEvent || e.userProperties.size() > D::kMaxParametersPerEvent) return false;
  for (const auto* p : {&e.parameters, &e.userProperties})
    for (const auto& kv : *p)
      if (kv.first.size() > D::kMaxStringBytes || kv.second.size() > D::kMaxStringBytes) return false;
  return true;
}

/** A destination that sends nothing, for the file path through start(). */
struct Silent : ThreadedAnalyticsDestination {
  explicit Silent(Settings s) : ThreadedAnalyticsDestination(std::move(s)) {}
  ~Silent() override { stopAnalyticsThread(); }

protected:
  bool logBatchedEvents(const std::vector<AnalyticsEvent>&) override { return false; }
};

void checkAnalyticsCase(const std::string& text, const std::string& filePath, bool throughFile) {
  using D = ThreadedAnalyticsDestination;
  g_current = text;
  g_deadlineMs = nowMs() + 10000;
  std::deque<AnalyticsEvent> events;
  size_t skipped = 0;
  const bool recognised = D::parseEvents(text, events, 2000, &skipped);
  if (!recognised && !events.empty()) fail("a refused file still produced events");
  for (const AnalyticsEvent& e : events)
    if (!eventIsWithinLimits(e)) {
      fail("an accepted event breaks the parser's own limits");
      break;
    }
  // Write what was accepted and read it back: the same events, exactly.
  const std::string again = D::serialiseEvents(events);
  std::deque<AnalyticsEvent> reread;
  size_t skippedAgain = 0;
  if (!D::parseEvents(again, reread, 2000, &skippedAgain)) fail("the writer's own output was refused");
  else if (reread.size() != events.size() || skippedAgain != 0) fail("a round trip changed the event count");
  else
    for (size_t i = 0; i < events.size(); ++i)
      if (events[i] != reread[i]) {
        fail("a round trip changed an event");
        break;
      }

  if (throughFile) {
    if (std::FILE* f = pathio::open(filePath.c_str(), "wb")) {
      std::fwrite(text.data(), 1, text.size(), f);
      std::fclose(f);
    }
    D::Settings s;
    s.unsentEventsFile = filePath;
    s.batchPeriodMs = 1000000; // never sends during the case
    Silent d(s);
    d.startAnalyticsThread();
    if (d.counters().restored != events.size()) fail("start() restored a different number than parseEvents read");
    d.stopAnalyticsThread();
    pathio::remove(filePath.c_str());
    pathio::remove((filePath + ".tmp").c_str());
  }
  g_deadlineMs = 0;
}

void fuzzAnalytics(size_t iterations, uint32_t seed) {
  g_target = "analytics";
  const std::vector<std::string> seeds = analyticsSeeds();
  const std::string filePath = tempPath("events.json");
  std::mt19937 rng(seed);
  size_t cases = 0;

  g_phase = "seeds";
  for (const std::string& s : seeds) {
    std::deque<AnalyticsEvent> events;
    if (!ThreadedAnalyticsDestination::parseEvents(s, events, 2000) && s.size() > 2) fail("a seed was refused");
    checkAnalyticsCase(s, filePath, true);
    ++cases;
  }

  g_phase = "truncate";
  const std::string& big = seeds[2];
  const size_t step = (std::max)((size_t) 1, big.size() / 1500);
  for (size_t n = 0; n <= big.size(); n += step, ++g_case, ++cases) checkAnalyticsCase(big.substr(0, n), filePath, false);

  g_phase = "mutate";
  for (size_t i = 0; i < iterations; ++i, ++g_case, ++cases) {
    std::string text = seeds[rng() % seeds.size()];
    // Stacked: one mutation, sometimes a few on top of each other.
    const int depth = 1 + (int) (rng() % 3);
    for (int d = 0; d < depth; ++d) text = mutate(text, seeds, rng);
    checkAnalyticsCase(text, filePath, i % 64 == 0);
  }
  std::printf("  analytics  %zu cases\n", cases);
}

// ═════════════════════════════════════════════════════════════════════════════
// video
// ═════════════════════════════════════════════════════════════════════════════

#if defined(_WIN32)
void decodeEverything(const std::string& path) {
  gfx::VideoReader reader;
  std::string error;
  if (!reader.open(path, &error)) return;
  if (reader.width() <= 0 || reader.height() <= 0 || reader.width() > 16384 || reader.height() > 16384)
    fail("an opened file reports an impossible size");
  gfx::Bitmap frame;
  double t = 0.0;
  for (int i = 0; i < 90 && reader.readFrame(frame, &t); ++i)
    if (frame.width() != reader.width() || frame.height() != reader.height()) {
      fail("a frame's size disagrees with the reader's");
      break;
    }
  if (reader.hasAudio() && reader.setAudioFormat(48000, 2)) {
    std::vector<float> buffer(4096 * 2);
    size_t total = 0, got = 0;
    while (total < 48000 * 3 && (got = reader.readAudio(buffer.data(), 4096)) > 0) total += got;
  }
  reader.seek(0.5);
  reader.readFrame(frame, &t);
}

void fuzzVideo(size_t iterations, uint32_t seed) {
  g_target = "video";
  gfx::detail::mf::ComScope com(COINIT_APARTMENTTHREADED);
  gfx::detail::mf::Startup startup;
  const std::string original = tempPath("seed.mp4"), mutated = tempPath("case.mp4");
  std::string why;
  if (!startup.ok() || !video_fixture::write(original, why)) {
    std::printf("  SKIPPED video: the seed file could not be written (%s)\n", why.c_str());
    return;
  }
  std::string bytes;
  if (std::FILE* f = pathio::open(original.c_str(), "rb")) {
    char chunk[65536];
    size_t n = 0;
    while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) bytes.append(chunk, n);
    std::fclose(f);
  }
  std::mt19937 rng(seed);
  size_t cases = 0;
  for (size_t i = 0; i < iterations; ++i, ++g_case, ++cases) {
    std::string b = bytes;
    g_phase = i % 4 == 3 ? "truncate" : "flip";
    if (i % 4 == 3) {
      b.resize((size_t) (rng() % (b.size() + 1)));
    } else {
      // Few flips: a container with a hundred broken bytes is refused at the
      // door, and the interesting cases are the ones that get past it.
      const int n = 1 + (int) (rng() % 4);
      for (int k = 0; k < n; ++k) b[(size_t) (rng() % b.size())] = (char) (rng() & 0xFF);
    }
    g_current = b;
    if (std::FILE* f = pathio::open(mutated.c_str(), "wb")) {
      std::fwrite(b.data(), 1, b.size(), f);
      std::fclose(f);
    }
    g_deadlineMs = nowMs() + 20000;
    decodeEverything(mutated);
    g_deadlineMs = 0;
  }
  pathio::remove(original.c_str());
  pathio::remove(mutated.c_str());
  std::printf("  video      %zu cases\n", cases);
}
#endif

} // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  size_t iterations = 2000, videoIterations = 40;
  uint32_t seed = 0x5EED5u;
  std::string only;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--iterations" && i + 1 < argc) iterations = (size_t) std::strtoull(argv[++i], nullptr, 10);
    else if (a == "--video-iterations" && i + 1 < argc) videoIterations = (size_t) std::strtoull(argv[++i], nullptr, 10);
    else if (a == "--seed" && i + 1 < argc) seed = (uint32_t) std::strtoul(argv[++i], nullptr, 10);
    else if (a == "--target" && i + 1 < argc) only = argv[++i];
    else {
      std::fprintf(stderr, "usage: fuzz_modules [--iterations N] [--video-iterations N] [--seed S] [--target analytics|video]\n");
      return 2;
    }
  }
  std::thread dog(watchdog);
  dog.detach();
  watchCrashes();

  if (only.empty() || only == "analytics") fuzzAnalytics(iterations, seed);
#if defined(_WIN32)
  if (only.empty() || only == "video") fuzzVideo(videoIterations, seed);
#else
  if (only.empty() || only == "video")
    std::printf("  SKIPPED video (%zu cases asked for): Windows only\n", videoIterations);
#endif
  g_done = true;
  std::printf("%s\n", g_failures ? "MODULES FUZZ FAILED" : "MODULES FUZZ PASSED");
  return g_failures ? 1 : 0;
}
