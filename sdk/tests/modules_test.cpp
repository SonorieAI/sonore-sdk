// SPDX-License-Identifier: Apache-2.0
// The three modules the reference ships that an audio plugin rarely reaches
// for -- video, analytics, and (in box2d_renderer_test.cpp) Box2D -- tested
// with real numbers rather than "it compiled".
//
//   ANALYTICS   nothing is sent by default; batching, backoff, rate limiting
//               and the queue bound, driven by a FAKE clock so no test
//               sleeps; the unsent-events file written, restored (and
//               deleted), including the .tmp a crash leaves; a real thread
//               delivering through a real send; ButtonTracker on Button and
//               IconButton, toggles included.
//   VIDEO       (Windows) Media Foundation's own encoder writes a 1-second
//               160x120 H.264 file with an AAC soundtrack: 30 synthetic
//               frames, red then green then blue, with a white/black
//               corner pattern that a flip in either axis would move. It is
//               decoded back and checked: frame count, size, the colour of
//               known frames, timestamps, exact seeks, the soundtrack's
//               length and pitch. Then VideoComponent plays it at 4x on its
//               clock and the frames it shows are read back as colours.
//   CAMERA      enumeration and every refusal path. This machine has no
//               camera, which is said in the output rather than implied by
//               a pass: a camera that IS present is opened and must deliver
//               a frame.
//
// Everything Windows-only says SKIPPED elsewhere; the Apple branch of
// gfx/video.h is compiled by mac_syntax_modules.cpp, never run here.
#include <sonore/analytics.h>
#include <sonore/gfx/button_tracker.h>
#include <sonore/gfx/video.h>
#include <sonore/gfx/widgets.h>
#include <sonore/gfx/widgets2.h>

#include "video_test_file.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace sonore;
using namespace sonore::gfx;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
  std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++failures;
}

std::string tempPath(const std::string& leaf) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path p =
      std::filesystem::temp_directory_path() / std::filesystem::u8path("sonore-modules-" + std::to_string(stamp) + "-" + leaf);
  return p.u8string();
}

bool fileExists(const std::string& path) {
  std::FILE* f = pathio::open(path.c_str(), "rb");
  if (!f) return false;
  std::fclose(f);
  return true;
}

// ═════════════════════════════════════════════════════════════════════════════
// Analytics
// ═════════════════════════════════════════════════════════════════════════════

struct RecordingDestination : AnalyticsDestination {
  std::vector<AnalyticsEvent> events;
  void logEvent(const AnalyticsEvent& e) override { events.push_back(e); }
};

struct FakeSender : ThreadedAnalyticsDestination {
  explicit FakeSender(Settings s) : ThreadedAnalyticsDestination(std::move(s)) {}
  ~FakeSender() override { stopAnalyticsThread(); }

  std::mutex m;
  std::vector<std::vector<AnalyticsEvent>> batches;
  bool fail = false;
  bool throwIt = false;
  std::atomic<int> attempts{0};
  std::atomic<int> stopCalls{0};

  size_t deliveredCount() {
    std::lock_guard<std::mutex> l(m);
    size_t n = 0;
    for (auto& b : batches) n += b.size();
    return n;
  }

protected:
  bool logBatchedEvents(const std::vector<AnalyticsEvent>& batch) override {
    ++attempts;
    if (throwIt) throw std::runtime_error("network on fire");
    std::lock_guard<std::mutex> l(m);
    if (fail) return false;
    batches.push_back(batch);
    return true;
  }
  void stopLoggingEvents() override { ++stopCalls; }
};

AnalyticsEvent makeEvent(const std::string& name) {
  AnalyticsEvent e;
  e.name = name;
  e.timestamp = 1700000000.25;
  return e;
}

void testAnalyticsCore() {
  std::printf("\nAnalytics\n");
  // The privacy promise, checked: the shared instance starts with nowhere to
  // send anything, so logging is a no-op until a plugin adds a destination.
  check(Analytics::instance().numDestinations() == 0, "the shared instance has NO destinations by default");
  Analytics::instance().logEvent("ignored", {{"k", "v"}});
  check(&Analytics::instance() == &Analytics::instance(), "instance() is one object");

  Analytics a;
  a.logEvent("dropped before any destination");
  auto* rec = static_cast<RecordingDestination*>(a.addDestination(std::unique_ptr<AnalyticsDestination>(new RecordingDestination())));
  check(a.numDestinations() == 1 && rec != nullptr, "a destination is added and handed back");
  check(a.addDestination(nullptr) == nullptr && a.numDestinations() == 1, "a null destination is ignored");
  a.setClock([]() { return 1234.5; });
  a.setUserId("user-7");
  a.setUserProperties({{"plan", "pro"}, {"os", "win"}});
  a.logEvent("preset_saved", {{"bank", "A"}, {"slot", "3"}}, 2);
  check(rec->events.size() == 1, "an event reaches the destination");
  if (!rec->events.empty()) {
    const AnalyticsEvent& e = rec->events[0];
    check(e.name == "preset_saved" && e.eventType == 2, "with its name and type");
    check(e.parameters.size() == 2 && e.parameters.at("slot") == "3", "and its parameters");
    check(e.userId == "user-7" && e.userProperties.at("plan") == "pro", "and the user it was logged for");
    check(e.timestamp == 1234.5, "and the clock's timestamp");
  }
  a.setUserId("user-8");
  check(rec->events[0].userId == "user-7", "a later user id does not rewrite an event already logged");
  a.setSuspended(true);
  a.logEvent("while suspended");
  check(rec->events.size() == 1 && a.isSuspended(), "a suspended Analytics drops events");
  a.setSuspended(false);
  a.logEvent("after");
  check(rec->events.size() == 2, "and resumes when unsuspended");
  check(a.removeDestination(rec) && a.numDestinations() == 0, "a destination can be removed");
  check(!a.removeDestination(rec), "and removing it twice says so");
}

void testThreadedBatching() {
  std::printf("\nThreadedAnalyticsDestination: batching, on a fake clock\n");
  double now = 100.0;
  ThreadedAnalyticsDestination::Settings s;
  s.batchPeriodMs = 1000;
  s.maxBatchSize = 3;
  s.maxEventsPerMinute = 0.0;
  FakeSender d(s);
  d.setClock([&]() { return now; });
  for (int i = 0; i < 7; ++i) d.logEvent(makeEvent("e" + std::to_string(i)));
  check(d.numPendingEvents() == 7, "seven events queue");
  now = 100.5;
  check(d.processPendingEvents() == 0, "nothing is sent before one batch period has passed");
  now = 101.0;
  check(d.processPendingEvents() == 3, "at one period, a batch of maxBatchSize goes");
  check(d.processPendingEvents() == 0, "and the next waits another period");
  now = 102.0;
  check(d.processPendingEvents() == 3, "then the next three");
  now = 103.0;
  check(d.processPendingEvents() == 1, "then the last one");
  now = 104.0;
  check(d.processPendingEvents() == 0, "an empty queue sends nothing");
  bool ordered = d.batches.size() == 3;
  int expect = 0;
  for (auto& b : d.batches)
    for (auto& e : b) ordered = ordered && e.name == "e" + std::to_string(expect++);
  check(ordered && expect == 7, "in order, oldest first");
  check(d.counters().delivered == 7, "delivered is counted");
}

void testThreadedRetry() {
  std::printf("\nThreadedAnalyticsDestination: retry with backoff\n");
  double now = 0.0;
  ThreadedAnalyticsDestination::Settings s;
  s.batchPeriodMs = 1000;
  s.maxBatchSize = 10;
  s.maxRetryDelayMs = 8000;
  s.maxEventsPerMinute = 0.0;
  FakeSender d(s);
  d.setClock([&]() { return now; });
  d.fail = true;
  d.logEvent(makeEvent("a"));
  d.logEvent(makeEvent("b"));
  now = 1.0;
  check(d.processPendingEvents() == -1, "a failed send reports -1");
  check(d.numPendingEvents() == 2, "and keeps the batch");
  check(std::fabs(d.currentRetryDelaySeconds() - 2.0) < 1e-9 && std::fabs(d.nextAttemptTime() - 3.0) < 1e-9,
        "the first retry waits two periods");
  now = 2.9;
  check(d.processPendingEvents() == 0, "not before then");
  now = 3.0;
  check(d.processPendingEvents() == -1 && std::fabs(d.nextAttemptTime() - 7.0) < 1e-9, "then four");
  now = 7.0;
  check(d.processPendingEvents() == -1 && std::fabs(d.nextAttemptTime() - 15.0) < 1e-9, "then eight (the cap)");
  now = 15.0;
  check(d.processPendingEvents() == -1 && std::fabs(d.nextAttemptTime() - 23.0) < 1e-9, "and stays at the cap");
  d.logEvent(makeEvent("c"));
  d.fail = false;
  now = 23.0;
  check(d.processPendingEvents() == 3, "once the send works, everything goes");
  check(d.batches.size() == 1 && d.batches[0].size() == 3 && d.batches[0][0].name == "a" &&
            d.batches[0][1].name == "b" && d.batches[0][2].name == "c",
        "retried events keep their place ahead of newer ones");
  check(std::fabs(d.currentRetryDelaySeconds() - 1.0) < 1e-9 && std::fabs(d.nextAttemptTime() - 24.0) < 1e-9,
        "and success resets the wait to one period");
  check(d.counters().failedAttempts == 4, "four failed attempts were counted");

  d.throwIt = true;
  d.logEvent(makeEvent("d"));
  now = 24.0;
  check(d.processPendingEvents() == -1 && d.numPendingEvents() == 1, "a send that THROWS is a failure, not a crash");
  d.throwIt = false;
}

void testRateAndBound() {
  std::printf("\nThreadedAnalyticsDestination: rate limit and queue bound\n");
  double now = 0.0;
  ThreadedAnalyticsDestination::Settings s;
  s.maxEventsPerMinute = 60.0;
  s.maxQueuedEvents = 1000;
  FakeSender d(s);
  d.setClock([&]() { return now; });
  for (int i = 0; i < 100; ++i) d.logEvent(makeEvent("burst"));
  check(d.numPendingEvents() == 60, "a burst is cut off at a minute's worth (60)");
  check(d.counters().droppedRateLimited == 40, "and the 40 dropped are counted");
  now = 30.0;
  for (int i = 0; i < 40; ++i) d.logEvent(makeEvent("later"));
  check(d.numPendingEvents() == 90, "thirty seconds later, thirty more are allowed");
  now = 29.0; // a clock that goes backwards refills nothing
  d.logEvent(makeEvent("backwards"));
  check(d.numPendingEvents() == 90, "a clock running backwards grants no tokens");

  ThreadedAnalyticsDestination::Settings b;
  b.maxEventsPerMinute = 0.0;
  b.maxQueuedEvents = 5;
  FakeSender bounded(b);
  for (int i = 0; i < 8; ++i) bounded.logEvent(makeEvent("q" + std::to_string(i)));
  check(bounded.numPendingEvents() == 5 && bounded.counters().droppedQueueFull == 3,
        "past maxQueuedEvents the oldest are dropped and counted");
}

void testPersistence() {
  std::printf("\nThreadedAnalyticsDestination: unsent events across sessions\n");
  // A non-ASCII name, as UTF-8 bytes: the file goes through path_io.h.
  const std::string file = tempPath("analytics-\xc3\xa9v\xc3\xa9nements.json");
  std::vector<AnalyticsEvent> originals;
  {
    ThreadedAnalyticsDestination::Settings s;
    s.unsentEventsFile = file;
    s.maxEventsPerMinute = 0.0;
    FakeSender d(s);
    d.fail = true;
    for (int i = 0; i < 4; ++i) {
      AnalyticsEvent e = makeEvent("session1-" + std::to_string(i));
      e.eventType = i - 1;
      e.timestamp = 1700000000.0 + i * 0.125;
      e.parameters = {{"quote", "say \"hi\""}, {"newline", "a\nb"}, {"utf8", "\xc3\xa9\xe4\xb8\xad"}};
      e.userId = "u";
      e.userProperties = {{"tier", std::to_string(i)}};
      originals.push_back(e);
      d.logEvent(e);
    }
    d.stopAnalyticsThread(); // no thread ever ran: the queue is saved anyway
    check(d.counters().saved == 4, "stopping saves the four unsent events");
  }
  check(fileExists(file), "to the unsent-events file");
  {
    ThreadedAnalyticsDestination::Settings s;
    s.unsentEventsFile = file;
    s.batchPeriodMs = 10;
    FakeSender d(s);
    check(d.startAnalyticsThread(), "a new session starts its thread");
    check(!d.startAnalyticsThread(), "starting twice says so");
    check(d.counters().restored == 4, "and restores the four");
    check(!fileExists(file), "deleting the file it restored them from");
    for (int i = 0; i < 200 && d.deliveredCount() < 4; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    d.stopAnalyticsThread();
    check(d.deliveredCount() == 4, "the thread delivers them");
    bool same = d.batches.size() >= 1;
    size_t k = 0;
    for (auto& batch : d.batches)
      for (auto& e : batch) same = same && k < originals.size() && e == originals[k++];
    check(same && k == 4, "byte for byte as they were logged, in order");
    check(d.stopCalls.load() >= 1, "stopLoggingEvents is called when stopping");
    check(!fileExists(file), "nothing left unsent leaves no file behind");
  }
  // The .tmp a crash between write and rename leaves is restored too.
  {
    std::deque<AnalyticsEvent> one{makeEvent("from-the-crash")};
    const std::string text = ThreadedAnalyticsDestination::serialiseEvents(one);
    std::FILE* f = pathio::open((file + ".tmp").c_str(), "wb");
    if (f) {
      std::fwrite(text.data(), 1, text.size(), f);
      std::fclose(f);
    }
    ThreadedAnalyticsDestination::Settings s;
    s.unsentEventsFile = file;
    FakeSender d(s);
    d.startAnalyticsThread();
    check(d.counters().restored == 1 && !fileExists(file + ".tmp"), "a leftover .tmp is restored and removed");
    d.fail = true;
    d.stopAnalyticsThread();
    check(fileExists(file), "and saved again when it still could not be sent");
  }
  pathio::remove(file.c_str());

  // The parser, on what it must refuse and what it must keep.
  std::deque<AnalyticsEvent> out;
  size_t skipped = 0;
  check(!ThreadedAnalyticsDestination::parseEvents("", out, 100), "empty text is refused");
  check(!ThreadedAnalyticsDestination::parseEvents("not json", out, 100), "non-JSON is refused");
  check(!ThreadedAnalyticsDestination::parseEvents("{\"format\":\"other\",\"version\":1,\"events\":[]}", out, 100),
        "another format is refused");
  check(!ThreadedAnalyticsDestination::parseEvents(
            "{\"format\":\"sonore-analytics-events\",\"version\":2,\"events\":[]}", out, 100),
        "a version from the future is refused");
  const char* mixed =
      "{\"format\":\"sonore-analytics-events\",\"version\":1,\"events\":["
      "{\"name\":\"good\",\"time\":5,\"type\":1},"
      "{\"name\":\"\",\"time\":5},"
      "{\"name\":\"no time\"},"
      "{\"name\":\"bad type\",\"time\":1,\"type\":1.5},"
      "{\"name\":\"huge type\",\"time\":1,\"type\":1e300},"
      "{\"name\":\"bad param\",\"time\":1,\"parameters\":{\"k\":3}},"
      "7,"
      "{\"name\":\"also good\",\"time\":6,\"parameters\":{\"a\":\"b\"}}]}";
  check(ThreadedAnalyticsDestination::parseEvents(mixed, out, 100, &skipped), "a file with bad records is read");
  check(out.size() == 2 && out[0].name == "good" && out[1].name == "also good" && skipped == 6,
        "keeping the two good events and skipping the six bad ones");
  out.clear();
  check(ThreadedAnalyticsDestination::parseEvents(mixed, out, 1) && out.size() == 1, "maxEvents caps what is read");
}

void testRealThread() {
  std::printf("\nThreadedAnalyticsDestination: a real thread, through Analytics\n");
  Analytics a;
  ThreadedAnalyticsDestination::Settings s;
  s.batchPeriodMs = 20;
  s.maxBatchSize = 2;
  auto* d = static_cast<FakeSender*>(a.addDestination(std::unique_ptr<AnalyticsDestination>(new FakeSender(s))));
  check(d->startAnalyticsThread() && d->isThreadRunning(), "the thread starts");
  for (int i = 0; i < 5; ++i) a.logEvent("click", {{"n", std::to_string(i)}});
  const auto start = std::chrono::steady_clock::now();
  while (d->deliveredCount() < 5 && std::chrono::steady_clock::now() - start < std::chrono::seconds(3))
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  check(d->deliveredCount() == 5, "five events arrive through the thread");
  check(d->batches.size() >= 3, "in batches of at most two");
  a.removeAllDestinations(); // joins the thread: the destination is destroyed here
  check(a.numDestinations() == 0, "removing the destination stops it");
}

// ═════════════════════════════════════════════════════════════════════════════
// ButtonTracker
// ═════════════════════════════════════════════════════════════════════════════

template <typename B>
void click(B& b) {
  MouseEvent e;
  e.position = {5.0f, 5.0f};
  e.downPosition = e.position;
  b.mouseDown(e);
  b.mouseUp(e);
}

void testButtonTracker() {
  std::printf("\nButtonTracker\n");
  Analytics a;
  auto* rec = static_cast<RecordingDestination*>(a.addDestination(std::unique_ptr<AnalyticsDestination>(new RecordingDestination())));

  Button button("Save");
  button.setBounds({0.0f, 0.0f, 40.0f, 20.0f});
  int handled = 0;
  button.onClick = [&]() { ++handled; };
  {
    ButtonTracker tracker(button, "save_clicked", {{"where", "toolbar"}}, 3, &a);
    click(button);
    check(rec->events.size() == 1 && handled == 1, "a click logs one event and still runs the button's handler");
    if (!rec->events.empty()) {
      check(rec->events[0].name == "save_clicked" && rec->events[0].eventType == 3,
            "named and typed as the tracker was told");
      check(rec->events[0].parameters.count("where") == 1 && rec->events[0].parameters.count("ButtonState") == 0,
            "with its parameters, and no on/off state on a push button");
    }
    KeyPress enter;
    enter.keyCode = KeyPress::Return;
    button.keyPressed(enter);
    check(rec->events.size() == 2 && handled == 2, "a keyboard press is a click too");
    MouseEvent outside;
    outside.position = {100.0f, 100.0f};
    button.mouseDown(MouseEvent{});
    button.mouseUp(outside);
    check(rec->events.size() == 2, "a press released outside (a cancel) logs nothing");
    check(tracker.clicksTracked() == 2, "the tracker counted two clicks");
  }
  click(button);
  check(rec->events.size() == 2 && handled == 3, "a destroyed tracker logs nothing, and the handler still runs");

  Button toggle("Bypass");
  toggle.setBounds({0.0f, 0.0f, 40.0f, 20.0f});
  toggle.setToggleable(true);
  ButtonTracker toggleTracker(toggle, "bypass", {}, 0, &a);
  click(toggle);
  click(toggle);
  check(rec->events.size() == 4 && rec->events[2].parameters.at("ButtonState") == "On" &&
            rec->events[3].parameters.at("ButtonState") == "Off",
        "a toggle reports the state the click left it in: On, then Off");

  IconButton icon;
  icon.setBounds({0.0f, 0.0f, 20.0f, 20.0f});
  ButtonTracker iconTracker(icon, "icon", {}, 0, &a);
  click(icon);
  check(rec->events.size() == 5 && rec->events[4].name == "icon", "an IconButton is tracked the same way");

  // The handler that destroys its own button: the tracker logs first and
  // touches nothing afterwards (ASan is the real check here).
  auto* doomed = new Button("Close");
  doomed->setBounds({0.0f, 0.0f, 40.0f, 20.0f});
  doomed->onClick = [&doomed]() {
    delete doomed;
    doomed = nullptr;
  };
  {
    ButtonTracker closer(*doomed, "close", {}, 0, &a);
    click(*doomed);
  }
  check(doomed == nullptr && rec->events.size() == 6 && rec->events[5].name == "close",
        "a handler that deletes its own button is logged first, and nothing touches it after");
}

// ═════════════════════════════════════════════════════════════════════════════
// Video
// ═════════════════════════════════════════════════════════════════════════════

#if defined(_WIN32)

// The generated file's shape: see video_test_file.h.
constexpr int kVideoW = video_fixture::kWidth, kVideoH = video_fixture::kHeight;
constexpr int kFrames = video_fixture::kFrames, kAudioRate = video_fixture::kAudioRate;

enum class Hue { Red, Green, Blue, Other };

Hue hueOf(PremulColour c) {
  if (c.r > 150 && c.g < 90 && c.b < 90) return Hue::Red;
  if (c.g > 150 && c.r < 90 && c.b < 90) return Hue::Green;
  if (c.b > 150 && c.r < 90 && c.g < 90) return Hue::Blue;
  return Hue::Other;
}

Hue expectedHue(int frame) {
  const int band = video_fixture::bandColour(frame);
  return band == 0 ? Hue::Red : (band == 1 ? Hue::Green : Hue::Blue);
}

const char* hueName(Hue h) {
  switch (h) {
    case Hue::Red: return "red";
    case Hue::Green: return "green";
    case Hue::Blue: return "blue";
    default: return "other";
  }
}

/** The corner pattern: top-left quarter white, top-right black, bottom band
 *  black. Read away from the edges, where chroma subsampling blurs nothing. */
bool patternUpright(const Bitmap& f) {
  const PremulColour tl = f.pixelAt(f.width() / 4, f.height() / 8);
  const PremulColour tr = f.pixelAt(f.width() * 3 / 4, f.height() / 8);
  const PremulColour bottom = f.pixelAt(f.width() / 2, f.height() * 15 / 16);
  return tl.r > 200 && tl.g > 200 && tl.b > 200 && tr.r < 50 && tr.g < 50 && tr.b < 50 && bottom.r < 50 &&
         bottom.g < 50 && bottom.b < 50;
}

Hue middleHue(const Bitmap& f) { return hueOf(f.pixelAt(f.width() / 2, f.height() / 2)); }

/** Crossings of zero going up, per second: a pitch estimate good to a few Hz
 *  over half a second of a clean sine. */
double pitchOf(const std::vector<float>& mono, int rate) {
  int crossings = 0;
  size_t first = 0, last = 0;
  for (size_t i = 1; i < mono.size(); ++i)
    if (mono[i - 1] < 0.0f && mono[i] >= 0.0f) {
      if (crossings == 0) first = i;
      last = i;
      ++crossings;
    }
  if (crossings < 2) return 0.0;
  return (double) (crossings - 1) * rate / (double) (last - first);
}

void testVideoReader(const std::string& file) {
  std::printf("\nVideoReader (Media Foundation), on the generated file\n");
  VideoReader reader;
  std::string error;
  check(reader.open(file, &error), "opens the H.264 + AAC file it was handed");
  if (!reader.isOpen()) {
    std::printf("       %s\n", error.c_str());
    return;
  }
  std::printf("       %dx%d, %.3f s, %.2f fps, audio %s\n", reader.width(), reader.height(), reader.duration(),
              reader.frameRate(), reader.hasAudio() ? "yes" : "no");
  check(reader.width() == kVideoW && reader.height() == kVideoH,
        "the visible size is 160x120 (H.264 decodes 128 rows; the aperture crops it)");
  check(std::fabs(reader.duration() - 1.0) < 0.1, "the duration is one second");
  check(std::fabs(reader.frameRate() - 30.0) < 0.5, "at thirty frames a second");
  check(reader.hasAudio(), "and it has a soundtrack");

  Bitmap frame;
  double t = -1.0, previous = -1.0;
  int count = 0;
  bool hues = true, upright = true, monotonic = true;
  double t15 = -1.0;
  while (reader.readFrame(frame, &t)) {
    if (frame.width() != kVideoW || frame.height() != kVideoH) hues = false;
    if (middleHue(frame) != expectedHue(count)) {
      if (hues)
        std::printf("       frame %d: %s, expected %s\n", count, hueName(middleHue(frame)), hueName(expectedHue(count)));
      hues = false;
    }
    if (!patternUpright(frame)) upright = false;
    if (t <= previous) monotonic = false;
    if (count == 15) t15 = t;
    previous = t;
    ++count;
  }
  std::printf("       %d frames decoded\n", count);
  check(reader.lastError().empty(), "to a clean end, with no error");
  check(count == kFrames, "thirty frames, as written");
  check(hues, "every frame the colour it was written in: ten red, ten green, ten blue");
  check(upright, "the right way up and the right way round (a flip in either axis moves the white corner)");
  check(monotonic, "timestamps increase");
  check(std::fabs(t15 - 0.5) < 0.005, "frame 15 is stamped 0.5 s");

  check(reader.seek(0.5) && reader.readFrame(frame, &t), "a seek to 0.5 s");
  check(std::fabs(t - 0.5) < 0.005 && middleHue(frame) == Hue::Green, "lands exactly on frame 15, green");
  check(reader.seek(0.9) && reader.readFrame(frame, &t) && std::fabs(t - 0.9) < 0.005 && middleHue(frame) == Hue::Blue,
        "0.9 s is frame 27, blue");
  check(reader.seek(0.0) && reader.readFrame(frame, &t) && t < 0.005 && middleHue(frame) == Hue::Red,
        "back to 0 is frame 0, red");

  // The soundtrack, at the rate it was written and resampled.
  check(reader.setAudioFormat(kAudioRate, 2) && reader.audioSampleRate() == kAudioRate && reader.audioChannels() == 2,
        "the soundtrack decodes as 48 kHz stereo float");
  reader.seekAudio(0.0);
  std::vector<float> all, chunk(4096 * 2);
  size_t got = 0;
  while ((got = reader.readAudio(chunk.data(), 4096)) > 0) all.insert(all.end(), chunk.begin(), chunk.begin() + (std::ptrdiff_t) (got * 2));
  const size_t frames48 = all.size() / 2;
  std::vector<float> left;
  double sumSquares = 0.0;
  for (size_t i = frames48 / 4; i < frames48 * 3 / 4; ++i) {
    left.push_back(all[i * 2]);
    sumSquares += (double) all[i * 2] * all[i * 2];
  }
  const double rms = left.empty() ? 0.0 : std::sqrt(sumSquares / (double) left.size());
  const double pitch = pitchOf(left, kAudioRate);
  std::printf("       %zu frames at 48 kHz, %.1f Hz, rms %.3f\n", frames48, pitch, rms);
  check(frames48 > 46000 && frames48 < 50500, "about one second of it");
  check(std::fabs(pitch - 440.0) < 3.0, "at 440 Hz");
  check(std::fabs(rms - 0.3536) < 0.04, "at half scale (rms 0.354)");

  const bool resampled = reader.setAudioFormat(44100, 1);
  std::printf("       44.1 kHz mono: %s (%d Hz, %d ch)\n", resampled ? "accepted" : reader.lastError().c_str(),
              reader.audioSampleRate(), reader.audioChannels());
  check(resampled && reader.audioSampleRate() == 44100 && reader.audioChannels() == 1,
        "Media Foundation resamples and downmixes it to 44.1 kHz mono on request");
  if (resampled) {
    reader.seekAudio(0.5);
    std::vector<float> rest;
    while ((got = reader.readAudio(chunk.data(), 4096)) > 0) rest.insert(rest.end(), chunk.begin(), chunk.begin() + (std::ptrdiff_t) got);
    const double restPitch = pitchOf(std::vector<float>(rest.begin() + (std::ptrdiff_t) (rest.size() / 4), rest.begin() + (std::ptrdiff_t) (rest.size() * 3 / 4)), 44100);
    std::printf("       after seekAudio(0.5): %zu frames, %.1f Hz\n", rest.size(), restPitch);
    check(rest.size() > 20000 && rest.size() < 24500, "seekAudio(0.5) leaves half a second");
    check(std::fabs(restPitch - 440.0) < 3.0, "still at 440 Hz after resampling");

    // Closed and opened again: no soundtrack format until one is set up,
    // and the sound from the top. close() kept the last file's format and
    // where its sound had been sought to, so the next file read from 0.5 s.
    reader.close();
    char said[160];
    std::snprintf(said, sizeof(said), "closed, it delivers no soundtrack format (%d Hz, %d ch)",
                  reader.audioSampleRate(), reader.audioChannels());
    check(reader.audioSampleRate() == 0 && reader.audioChannels() == 0, said);
    if (reader.open(file, &error)) {
      const int rateBefore = reader.audioSampleRate();
      std::vector<float> again;
      while ((got = reader.readAudio(chunk.data(), 4096)) > 0) again.insert(again.end(), chunk.begin(), chunk.begin() + (std::ptrdiff_t) got);
      std::snprintf(said, sizeof(said),
                    "reopened, none until it is read (%d Hz), then all of it from the top at 44.1 kHz mono (%zu frames)",
                    rateBefore, again.size());
      check(rateBefore == 0 && reader.audioSampleRate() == 44100 && again.size() > 42000 && again.size() < 46500, said);
    }
  }

  VideoReader missing;
  check(!missing.open(file + ".does-not-exist", &error) && !error.empty(), "a missing file is refused with a reason");
  std::printf("       (%s)\n", error.c_str());
  const std::string text = tempPath("not-a-video.mp4");
  if (std::FILE* f = pathio::open(text.c_str(), "wb")) {
    std::fputs("this is plainly not a video file\n", f);
    std::fclose(f);
  }
  check(!missing.open(text, &error), "so is a text file wearing a .mp4 name");
  pathio::remove(text.c_str());
}

void paintInto(VideoComponent& v, Bitmap& b) {
  b.clear(Colour(255, 0, 255));
  Graphics g(b);
  v.paint(g);
}

void testVideoComponent(const std::string& file) {
  std::printf("\nVideoComponent, playing the generated file\n");
  VideoComponent v;
  v.setBounds({0.0f, 0.0f, 400.0f, 200.0f});
  int started = 0, stopped = 0, ended = 0, errors = 0;
  std::string lastError;
  v.onPlaybackStarted = [&]() { ++started; };
  v.onPlaybackStopped = [&]() { ++stopped; };
  v.onPlaybackEnded = [&]() { ++ended; };
  v.onErrorOccurred = [&](const std::string& e) {
    ++errors;
    lastError = e;
  };

  std::string error;
  check(!v.load(file + ".missing", &error) && errors == 1 && !error.empty(),
        "loading a missing file fails, through onErrorOccurred too");
  check(v.load(file, &error), "the file loads");
  if (!v.isVideoOpen()) {
    std::printf("       %s\n", error.c_str());
    return;
  }
  check(v.videoWidth() == kVideoW && v.videoHeight() == kVideoH && std::fabs(v.videoDuration() - 1.0) < 0.1,
        "reporting its size and duration");
  check(!v.currentFrame().isEmpty() && middleHue(v.currentFrame()) == Hue::Red,
        "paused on its first frame, already decoded: a poster, not a black box");
  check(!v.isPlaying() && v.playPosition() == 0.0, "not playing, at 0");

  Bitmap canvas(400, 200);
  paintInto(v, canvas);
  // 4:3 in a 400x200 box: 266.7 wide, centred, black bars either side.
  const PremulColour bar = canvas.pixelAt(20, 100), picture = canvas.pixelAt(200, 100);
  check(bar.r == 0 && bar.g == 0 && bar.b == 0, "painted letterboxed: the bars are the background");
  check(hueOf(picture) == Hue::Red, "and the picture is the frame");
  // The picture spans x 66.7 to 333.3; its top quarter is white on the left
  // half (to x = 200) and black on the right.
  const PremulColour white = canvas.pixelAt(130, 10), black = canvas.pixelAt(270, 10);
  check(white.r > 200 && white.g > 200 && white.b > 200 && black.r < 50 && black.g < 50 && black.b < 50,
        "scaled, with its corner pattern where it belongs");

  v.setPlaySpeed(4.0);
  check(v.playSpeed() == 4.0, "speed 4x");
  v.play();
  check(v.isPlaying() && started == 1, "play() starts it and says so");
  std::vector<Hue> seen;
  const auto t0 = std::chrono::steady_clock::now();
  while (ended == 0 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5)) {
    if (v.update()) {
      const Hue h = middleHue(v.currentFrame());
      if (seen.empty() || seen.back() != h) seen.push_back(h);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::string sequence;
  for (Hue h : seen) sequence += std::string(hueName(h)) + " ";
  std::printf("       %.3f s of wall time; colours shown: %s\n", elapsed, sequence.c_str());
  check(ended == 1 && stopped == 1, "it reaches the end: onPlaybackEnded, and onPlaybackStopped");
  check(elapsed > 0.2 && elapsed < 0.9, "one second of video in about a quarter of a second, at 4x (well under the 1 s of 1x)");
  check(seen.size() >= 2 && seen.back() == Hue::Blue && (seen.front() == Hue::Red || seen.front() == Hue::Green),
        "showing red (or green), then on to blue, in order");
  check(!v.isPlaying() && std::fabs(v.playPosition() - v.videoDuration()) < 0.05, "and stops at the end");
  const VideoComponent::Stats s = v.stats();
  std::printf("       decoded %llu, shown %llu, dropped %llu\n", (unsigned long long) s.framesDecoded,
              (unsigned long long) s.framesShown, (unsigned long long) s.framesDropped);
  check(s.framesDecoded >= (uint64_t) kFrames && s.framesShown > 10, "every frame decoded, and most shown");

  // Paused seeks deliver the frame there.
  v.setPlayPosition(0.5);
  bool arrived = false;
  for (int i = 0; i < 500 && !arrived; ++i) {
    arrived = v.update();
    if (!arrived) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  check(arrived && middleHue(v.currentFrame()) == Hue::Green && std::fabs(v.playPosition() - 0.5) < 1e-9,
        "a paused seek to 0.5 s shows frame 15 (green), and the position is exactly 0.5");

  v.play();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const double during = v.playPosition();
  v.pause();
  const double paused = v.playPosition();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  check(during > 0.5 && paused >= during && std::fabs(v.playPosition() - paused) < 1e-9,
        "the clock runs while playing and holds still when paused");
  check(stopped == 2, "pause() reports a stop");
  v.stop();
  check(v.playPosition() == 0.0 && !v.isPlaying(), "stop() goes back to the start");

  v.setPlaySpeed(100.0);
  check(v.playSpeed() == 16.0, "speed is clamped to 16x");
  v.setPlaySpeed(std::nan(""));
  check(v.playSpeed() == 1.0, "and a NaN speed is 1x");
  v.setAudioVolume(3.0f);
  check(v.audioVolume() == 1.0f, "volume is clamped to 1");
  v.setAudioVolume(0.25f);
  check(v.audioVolume() == 0.25f && !v.isAudioEnabled(), "the soundtrack is OFF until asked for");

  // The soundtrack through the SDK's own WASAPI output, if this machine has
  // a device; there is nothing to listen with, so what is checked is that it
  // was decoded and queued -- or that the reason it could not be is given.
  // Enabled while PAUSED: the device opens now (slowly, the first time a
  // process does it) and stays open, so play and seek do not pay for it.
  v.setPlaySpeed(1.0);
  v.setAudioEnabled(true);
  auto waitFor = [&](auto done, int ms) {
    const auto t = std::chrono::steady_clock::now();
    while (!done() && std::chrono::steady_clock::now() - t < std::chrono::milliseconds(ms)) {
      v.update();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
  };
  // Ten seconds: opening a device is slow the first time a process does it,
  // slower on a machine busy compiling, and this checks that it opens.
  const double openMs = waitFor([&]() { return v.stats().audioDeviceOpen || !v.stats().audioError.empty(); }, 10000);
  VideoComponent::Stats as = v.stats();
  if (!as.audioError.empty()) {
    std::printf("       soundtrack: no output here (%s)\n", as.audioError.c_str());
    check(true, "enabled, the soundtrack reports why it cannot play (no output device)");
  } else {
    std::printf("       the output device opened in %.0f ms, while paused\n", openMs);
    check(as.audioDeviceOpen && as.audioFramesQueued == 0, "enabling it while paused opens the device and queues nothing");
    const uint64_t before = as.audioFramesQueued;
    v.play();
    // What play() promises is that the device is already open and sound
    // follows, not a time: a run beside two full builds took over 300 ms to
    // queue half a second, on the same open device. The time is printed.
    const double playMs = waitFor([&]() { return v.stats().audioFramesQueued > before + 10000; }, 5000);
    std::printf("       play: %.1f ms to the first half second of sound queued\n", playMs);
    check(v.stats().audioFramesQueued > before + 10000 && v.stats().audioDeviceOpen,
          "play() starts the sound on the device it already opened -- no device to open");
    const double positionAfterStart = v.playPosition();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    check(v.playPosition() > positionAfterStart + 0.05, "and the picture's clock runs on with it");
    const uint64_t beforeSeek = v.stats().audioFramesQueued;
    v.setPlayPosition(0.1);
    // What a seek promises is a refill on the device already open -- under a
    // second even beside a full build, where reopening the device took
    // several. The time is printed.
    const double seekMs = waitFor([&]() { return v.stats().audioFramesQueued > beforeSeek + 10000; }, 5000);
    std::printf("       seek while playing: %.1f ms to refill from the new position\n", seekMs);
    check(v.stats().audioFramesQueued > beforeSeek + 10000 && seekMs < 1000,
          "a seek while playing refills the soundtrack from the new position");
    v.pause();
    v.setAudioEnabled(false);
    waitFor([&]() { return !v.stats().audioDeviceOpen; }, 5000);
    check(!v.stats().audioDeviceOpen, "disabling the soundtrack closes the device");
  }

  v.closeVideo();
  check(!v.isVideoOpen() && v.currentFrame().isEmpty() && v.playPosition() == 0.0, "closeVideo() lets it all go");
}

#endif // _WIN32

void testVideo() {
  std::printf("\nVideo\n");
#if defined(_WIN32)
  check(VideoReader::isAvailable() && VideoComponent::isAvailable(), "video is available on Windows");
  detail::mf::ComScope com(COINIT_APARTMENTTHREADED);
  detail::mf::Startup startup;
  const std::string file = tempPath("synthetic.mp4");
  std::string why;
  const bool written = startup.ok() && video_fixture::write(file, why);
  check(written, "Media Foundation's encoder writes the test file (H.264 + AAC)");
  if (!written) {
    std::printf("       %s\n", why.c_str());
    return;
  }
  testVideoReader(file);
  testVideoComponent(file);
  pathio::remove(file.c_str());
#else
  std::printf("  SKIPPED video decoding: Windows only in this test (macOS is compiled, not run)\n");
  check(!VideoComponent::isAvailable() || VideoReader::isAvailable(), "availability is consistent");
  VideoComponent v;
  std::string error;
#if !defined(__APPLE__)
  check(!v.load("anything.mp4", &error) && !error.empty(), "load() reports that video is not available here");
#endif
#endif
}

void testCamera() {
  std::printf("\nCameraDevice\n");
  const std::vector<std::string> names = CameraDevice::availableDevices();
  std::printf("       %zu camera(s)%s\n", names.size(), names.empty() ? " -- this machine has none" : "");
  for (const std::string& n : names) std::printf("         %s\n", n.c_str());
  std::string error;
  check(CameraDevice::openDevice((int) names.size(), 0, 0, 1920, 1080, &error) == nullptr && !error.empty(),
        "opening one past the last camera fails, with a reason");
  std::printf("       (%s)\n", error.c_str());
  error.clear();
  check(CameraDevice::openDevice(-1, 0, 0, 1920, 1080, &error) == nullptr && !error.empty(),
        "a negative index fails the same way");
  if (names.empty()) {
    std::printf("  NOTE no camera is attached: enumeration and the refusal paths are all that ran\n");
    return;
  }
  error.clear();
  std::unique_ptr<CameraDevice> camera = CameraDevice::openDevice(0, 0, 0, 1280, 720, &error);
  if (!camera) {
    std::printf("  NOTE camera 0 would not open (%s) -- in use, or refused by privacy settings\n", error.c_str());
    return;
  }
  struct Grab : CameraDevice::Listener {
    std::atomic<int> frames{0};
    std::atomic<int> width{0};
    void imageReceived(const Bitmap& image) override {
      width = image.width();
      ++frames;
    }
  } grab;
  camera->addListener(&grab);
  std::atomic<bool> still{false};
  camera->takeStillPicture([&](const Bitmap& image) { still = !image.isEmpty(); });
  const auto t0 = std::chrono::steady_clock::now();
  while ((grab.frames < 3 || !still) && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5))
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  camera->removeListener(&grab);
  std::printf("       %s: %d frames at %dx%d\n", camera->name().c_str(), grab.frames.load(), camera->width(),
              camera->height());
  check(grab.frames >= 3 && grab.width > 0, "an attached camera delivers frames to a listener");
  check(still.load(), "and a still picture");
}

} // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  testAnalyticsCore();
  testThreadedBatching();
  testThreadedRetry();
  testRateAndBound();
  testPersistence();
  testRealThread();
  testButtonTracker();
  testVideo();
  testCamera();
  std::printf("\n%s\n", failures ? "MODULES TESTS FAILED" : "MODULES TESTS PASSED");
  return failures ? 1 : 0;
}
