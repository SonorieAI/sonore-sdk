// SPDX-License-Identifier: Apache-2.0
// Sonore SDK -- the spectrum a plugin's interface draws: SpectrumFeed (the
// measurement), the bridge message that turns it on, and SpectrumView (the
// native display).
//
// The feed is held to the rule it shares with the Sonorie studio's browser
// preview (spectrum.ts), by physics rather than by its own numbers: a
// full-scale sine centred on a bin reads 0.00 dBFS through the window, half a
// bin off reads the Blackman window's scalloping loss, the same at every
// sample rate, and silence reads the floor. push() runs with global operator
// new counting armed: the audio thread may not allocate.
#include <sonore/gfx/audio_display.h>
#include <sonore/gfx/bitmap.h>
#include <sonore/gfx/graphics.h>
#include <sonore/gui.h>
#include <sonore/spectrum_feed.h>

#include <atomic>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

using namespace sonore;

// ── Allocation counter, armed around the audio-thread call ─────────────────
static std::atomic<bool> g_armed{false};
static std::atomic<long> g_allocations{0};

void* operator new(std::size_t n) {
  if (g_armed.load(std::memory_order_relaxed)) g_allocations.fetch_add(1);
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

static int g_checks = 0;
static int g_failures = 0;

static void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

static const double kTau = 6.283185307179586;

/** A full-scale sine, `seconds` of it, pushed in 256-frame blocks as a host
 *  would, on both channels or one. */
static void feedSine(SpectrumFeed& feed, double hz, double rate, double seconds, bool stereo = true) {
  const int frames = (int) (seconds * rate);
  std::vector<float> left(256), right(256);
  for (int start = 0; start < frames; start += 256) {
    const int n = frames - start < 256 ? frames - start : 256;
    for (int i = 0; i < n; ++i) left[i] = right[i] = (float) std::sin(kTau * hz * (start + i) / rate + 0.3);
    const float* chans[2] = {left.data(), right.data()};
    feed.push(chans, stereo ? 2u : 1u, (uint32_t) n);
  }
}

static float loudest(const float* v, int* at = nullptr) {
  float best = -1e9f;
  for (int i = 0; i < SpectrumFeed::kPoints; ++i)
    if (v[i] > best) {
      best = v[i];
      if (at) *at = i;
    }
  return best;
}

static void testCalibration() {
  std::printf("SpectrumFeed: the rule a page's analyzer is drawn by\n");
  char msg[200];
  check(SpectrumFeed::windowFor(44100) == 8192 && SpectrumFeed::windowFor(48000) == 8192 &&
            SpectrumFeed::windowFor(96000) == 16384 && SpectrumFeed::windowFor(192000) == 32768,
        "the window is ~170 ms at every rate: 8192, 16384, 32768");
  check(std::fabs(SpectrumFeed::pointHz(0) - 20.0) < 1e-9 && std::fabs(SpectrumFeed::pointHz(255) - 20000.0) < 1e-6,
        "the grid runs 20 Hz .. 20 kHz over 256 points");

  for (double rate : {48000.0, 96000.0}) {
    const uint32_t n = SpectrumFeed::windowFor(rate);
    SpectrumFeed feed;
    feed.setActive(true);
    float out[SpectrumFeed::kPoints];
    const double centred = 171.0 * rate / n;
    feedSine(feed, centred, rate, 0.4);
    feed.analyse(rate, out);
    int at = 0;
    const float top = loudest(out, &at);
    std::snprintf(msg, sizeof(msg),
                  "at %.0f kHz a full-scale sine on a bin centre (%.2f Hz) reads %.3f dBFS at %.0f Hz", rate / 1000,
                  centred, top, SpectrumFeed::pointHz(at));
    check(std::fabs(top) < 0.02f && std::fabs(SpectrumFeed::pointHz(at) / centred - 1.0) < 0.03, msg);

    SpectrumFeed off;
    off.setActive(true);
    feedSine(off, 171.5 * rate / n, rate, 0.4);
    off.analyse(rate, out);
    const float between = loudest(out);
    std::snprintf(msg, sizeof(msg), "...and half a bin off, the window's scalloping loss: %.2f dB (-1.10)", between);
    check(between < -1.0f && between > -1.2f, msg);
  }

  // A tone between grid points in the bass, where points are interpolated
  // between bins rather than read from one.
  {
    SpectrumFeed feed;
    feed.setActive(true);
    float out[SpectrumFeed::kPoints];
    feedSine(feed, 60.0, 48000.0, 0.4);
    feed.analyse(48000.0, out);
    int at = 0;
    const float top = loudest(out, &at);
    std::snprintf(msg, sizeof(msg), "a 60 Hz sine reads %.2f dBFS near %.1f Hz: the bass is interpolated, not dropped", top,
                  SpectrumFeed::pointHz(at));
    check(top > -1.5f && top < 0.1f && std::fabs(SpectrumFeed::pointHz(at) - 60.0) < 3.0, msg);
  }
}

static void testFeedRules() {
  std::printf("SpectrumFeed: what it measures and when\n");
  float out[SpectrumFeed::kPoints];
  bool allFloor = true;

  // Every instance of every plugin carries two feeds and most never draw one,
  // so an unused feed is small: the ring is made by the first setActive(true).
  {
    char msg[120];
    std::snprintf(msg, sizeof(msg), "an unused feed is %zu bytes: the 128 KB ring is made only when something asks",
                  sizeof(SpectrumFeed));
    check(sizeof(SpectrumFeed) < 512, msg);
  }

  SpectrumFeed idle; // never turned on
  feedSine(idle, 1000.0, 48000.0, 0.3);
  idle.analyse(48000.0, out);
  for (float v : out) allFloor = allFloor && v == SpectrumFeed::kFloorDb;
  check(allFloor, "until something asks (setActive), push() feeds nothing: the display reads the floor");

  // The mono average: hard left against its negative on the right cancels.
  SpectrumFeed cancel;
  cancel.setActive(true);
  {
    std::vector<float> l(4096), r(4096);
    for (int i = 0; i < 4096; ++i) {
      l[i] = (float) std::sin(kTau * 1000.0 * i / 48000.0);
      r[i] = -l[i];
    }
    for (int k = 0; k < 4; ++k) {
      const float* chans[2] = {l.data(), r.data()};
      cancel.push(chans, 2, 4096);
    }
  }
  cancel.analyse(48000.0, out);
  allFloor = true;
  for (float v : out) allFloor = allFloor && v == SpectrumFeed::kFloorDb;
  check(allFloor, "the channels are averaged to mono, as a browser analyser's down-mix does: L against -L is silence");

  // A null channel is skipped, not read and not counted.
  SpectrumFeed half;
  half.setActive(true);
  {
    std::vector<float> l(4096);
    for (int k = 0; k < 4; ++k) {
      for (int i = 0; i < 4096; ++i) l[i] = (float) std::sin(kTau * (171.0 * 48000.0 / 8192) * (k * 4096 + i) / 48000.0);
      const float* chans[2] = {l.data(), nullptr};
      half.push(chans, 2, 4096);
    }
  }
  half.analyse(48000.0, out);
  check(std::fabs(loudest(out)) < 0.02f, "a null channel pointer is skipped: one real channel still reads 0 dBFS");

  // The grid rule on its own: nothing above Nyquist, and silence is the floor.
  {
    std::vector<double> bins(4096, -20.0);
    SpectrumFeed::grid(bins.data(), 4096, 32000.0, 8192, out);
    check(out[255] == SpectrumFeed::kFloorDb && out[0] > SpectrumFeed::kFloorDb,
          "a point above Nyquist reads the floor; the rest reads the signal");
    std::vector<double> dead(4096, -1e9);
    SpectrumFeed::grid(dead.data(), 4096, 48000.0, 8192, out);
    allFloor = true;
    for (float v : out) allFloor = allFloor && v == SpectrumFeed::kFloorDb;
    check(allFloor, "nothing at all reads the floor, never a NaN");
  }

  // The audio thread allocates nothing.
  {
    SpectrumFeed feed;
    feed.setActive(true);
    std::vector<float> l(512, 0.25f), r(512, -0.1f);
    const float* chans[2] = {l.data(), r.data()};
    std::vector<double> dl(512, 0.25);
    const double* dchans[1] = {dl.data()};
    const long before = g_allocations.load();
    g_armed = true;
    for (int k = 0; k < 200; ++k) {
      feed.push(chans, 2, 512);
      feed.push(dchans, 1, 512);
    }
    g_armed = false;
    check(g_allocations.load() == before, "push(), float and double, 400 blocks: zero allocations");
  }
}

static void testTenths() {
  std::printf("SpectrumFeed::appendTenths: numbers JavaScript can read in any locale\n");
  const float v[6] = {-23.44f, -140.0f, 0.04f, -0.06f, std::nanf(""), 12.0f};
  std::string s;
  SpectrumFeed::appendTenths(s, v, 6);
  check(s == "-23.4,-140.0,0.0,-0.1,-140.0,12.0", ("tenths, comma separated, a NaN as the floor: " + s).c_str());
  // Hosts call setlocale(), and printf's %f follows it: a German locale prints
  // "-23,4", which inside generated JavaScript is a syntax error.
  const char* german = std::setlocale(LC_ALL, "de_DE.UTF-8");
  if (!german) german = std::setlocale(LC_ALL, "German_Germany.1252");
  if (!german) german = std::setlocale(LC_ALL, "de_DE");
  if (german) {
    std::string t;
    SpectrumFeed::appendTenths(t, v, 6);
    check(t == s, "...and the same digits under a German locale, where %f would write a comma");
    std::setlocale(LC_ALL, "C");
  } else {
    std::printf("  SKIP no German locale installed to prove the locale case with\n");
  }
}

static void testBridgeMessage() {
  std::printf("the bridge message that turns the feeds on\n");
  BridgeMessage m = parseBridgeMessage("{\"type\":\"spectrum\",\"on\":1,\"input\":1}");
  check(m.kind == BridgeMessage::Kind::Spectrum && m.value == 1.0 && m.index == 1,
        "{type:'spectrum', on:1, input:1} asks for the output and the input");
  m = parseBridgeMessage("{\"type\":\"spectrum\",\"on\":1}");
  check(m.kind == BridgeMessage::Kind::Spectrum && m.value == 1.0 && m.index == 0, "on alone asks for the output only");
  m = parseBridgeMessage("{\"type\":\"spectrum\"}");
  check(m.kind == BridgeMessage::Kind::Spectrum && m.value == 0.0 && m.index == 0,
        "with neither field the page asked for nothing, and nothing is turned on");
}

static void testView() {
  using namespace sonore::gfx;
  std::printf("SpectrumView: the native display\n");
  char msg[200];
  const int N = SpectrumView::kPoints;
  std::vector<float> frame(N), input(N);
  for (int i = 0; i < N; ++i) {
    frame[i] = -30.0f - 0.1f * i;
    input[i] = -36.0f - 0.1f * i;
  }

  SpectrumView view;
  view.setSmoothing(0.0);
  view.push(frame.data(), input.data());
  view.step(0.0);
  check(view.displayed()[10] == SpectrumFeed::kFloorDb, "a new frame is approached, not jumped to: at dt 0 nothing has moved");
  view.step(0.1); // a rise is fast: a 12 ms time constant
  bool landed = true;
  for (int i = 0; i < N; ++i) landed = landed && std::fabs(view.displayed()[i] - frame[i]) < 0.05f;
  check(landed, "a rise of 110 dB is within 0.05 dB after 100 ms (a 12 ms attack)");

  // A fall at the speed set: medium is a 0.22 s time constant.
  std::vector<float> quiet(N, -100.0f);
  view.push(quiet.data(), nullptr);
  view.step(0.22);
  const float expect = frame[100] + (-100.0f - frame[100]) * (float) (1.0 - std::exp(-1.0));
  std::snprintf(msg, sizeof(msg), "a fall follows the speed set: one time constant takes 63%% of the way (%.2f, want %.2f)",
                view.displayed()[100], expect);
  check(std::fabs(view.displayed()[100] - expect) < 0.05f, msg);

  // Tilt about 1 kHz, as the page's does.
  SpectrumView tilted;
  tilted.setSmoothing(0.0);
  tilted.setTilt(4.5);
  std::vector<float> flat(N, -40.0f);
  tilted.push(flat.data(), nullptr);
  tilted.step(1.0);
  int k2 = 0;
  for (int i = 0; i < N; ++i)
    if (std::fabs(SpectrumFeed::pointHz(i) - 4000.0) < std::fabs(SpectrumFeed::pointHz(k2) - 4000.0)) k2 = i;
  const double want = -40.0 + 4.5 * std::log2(SpectrumFeed::pointHz(k2) / 1000.0);
  std::snprintf(msg, sizeof(msg), "a 4.5 dB/oct tilt lifts two octaves above 1 kHz by ~9 dB (%.2f, want %.2f)",
                tilted.displayed()[k2], want);
  check(std::fabs(tilted.displayed()[k2] - want) < 0.05, msg);

  // Peak hold: held for the hold time, then let down.
  SpectrumView held;
  held.setSmoothing(0.0);
  held.setPeakHold(true, 1.0);
  std::vector<float> loud(N, -10.0f), soft(N, -60.0f);
  held.push(loud.data(), nullptr);
  held.step(0.2);
  held.push(soft.data(), nullptr);
  for (int t = 0; t < 8; ++t) held.step(0.1); // 0.8 s: still held
  check(std::fabs(held.peaks()[50] + 10.0f) < 0.05f, "the peak line holds its maximum for the hold time");
  for (int t = 0; t < 10; ++t) held.step(0.1); // past it: falling at 18 dB/s
  check(held.peaks()[50] < -12.0f && held.peaks()[50] > -60.0f, "...and is then let down, not dropped");

  // It renders in every style, with a curve over it, and draws something.
  for (SpectrumView::Style st : {SpectrumView::Style::Curve, SpectrumView::Style::Line, SpectrumView::Style::Bars,
                                 SpectrumView::Style::Led}) {
    SpectrumView v;
    v.setBounds({0.0f, 0.0f, 320.0f, 140.0f});
    v.setStyle(st);
    v.setPeakHold(true);
    v.setColour(Colour::fromRGB(0x4FD1FF));
    v.setCurve([](double hz) { return 6.0 * std::exp(-std::pow(std::log2(hz / 1000.0), 2.0)); });
    v.push(frame.data(), input.data());
    v.step(0.5);
    Bitmap bmp(320, 140);
    Graphics g(bmp);
    g.fillAll(Colour(0, 0, 0));
    v.paintTree(g);
    int lit = 0;
    for (int y = 0; y < 140; y += 2)
      for (int x = 0; x < 320; x += 2) {
        const Colour c = bmp.pixelAt(x, y).toStraight();
        if (c.b > 120 && c.g > 80) ++lit;
      }
    const char* name = st == SpectrumView::Style::Curve ? "curve" : st == SpectrumView::Style::Line ? "line"
                       : st == SpectrumView::Style::Bars ? "bars" : "LED";
    std::snprintf(msg, sizeof(msg), "the %s style draws the trace (%d bright pixels sampled)", name, lit);
    check(lit > 40, msg);
  }

  // Fed from a real feed: update() measures it and the display follows.
  {
    SpectrumFeed out;
    SpectrumView v;
    v.setFeeds(&out);
    v.setSampleRate(48000.0);
    check(out.active(), "setFeeds turns the feed on");
    feedSine(out, 171.0 * 48000.0 / 8192, 48000.0, 0.4);
    v.setSmoothing(0.0);
    v.update(0.5);
    std::snprintf(msg, sizeof(msg), "update() measures the feed: the sine reads %.2f dBFS on the display", loudest(v.displayed()));
    check(std::fabs(loudest(v.displayed())) < 0.05f, msg);
  }
}

int main() {
  testCalibration();
  testFeedRules();
  testTenths();
  testBridgeMessage();
  testView();
  std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
  if (g_failures) std::printf("SPECTRUM TESTS FAILED\n");
  return g_failures ? 1 : 0;
}
