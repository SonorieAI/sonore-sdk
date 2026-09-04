// SPDX-License-Identifier: Apache-2.0
// The JACK backend, against a real server.
//
// jackd can be unpacked and started without root, in dummy mode, which gives a
// real server with a real real-time thread calling a real client. The only thing missing is actual
// hardware, and this backend never touches hardware -- JACK does.
//
// What that buys is the property no amount of reading gets you: that the
// callback is CALLED, at the rate the server runs at, and stops when asked.
//
// It skips loudly rather than failing where there is no server. A machine
// without JACK is the ordinary case for a plugin developer, and a test that
// failed there would fail on every CI runner and teach people to ignore it.
#include <sonore/audio_jack.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

static int g_checks = 0, g_failures = 0;
static void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}

int main() {
  using namespace sonore::standalone;
  std::printf("── jack ──\n");
  char msg[240];

  if (!JackOutput::isAvailable()) {
    std::printf("  ---- SKIPPED: libjack is not on this machine ----\n");
    return 0;
  }
  check(true, "libjack resolved every symbol the backend needs");

  // ── Opening without a server ─────────────────────────────────────────────
  //
  // Checked FIRST, because it is the case most machines are in and the one a
  // wrong answer is invisible in: a backend that reported success with no
  // server would give a standalone that appears to play and makes no sound.
  {
    JackOutput probe;
    const bool opened = probe.open([](float*, uint32_t, uint32_t) {});
    if (!opened) {
      std::snprintf(msg, sizeof(msg), "with no server it refuses, saying \"%s\"",
                    probe.error().c_str());
      check(!probe.error().empty(), msg);
      std::printf("  ---- SKIPPED the live half: no JACK server is running ----\n");
      std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
      return g_failures == 0 ? 0 : 1;
    }
    probe.stop();
  }

  // ── A real client on a real server ───────────────────────────────────────
  JackOutput jack;
  std::atomic<uint64_t> frames{0};
  std::atomic<uint32_t> calls{0};
  std::atomic<uint32_t> channelsSeen{0};
  std::atomic<bool> sawNull{false};
  double phase = 0.0;

  const bool opened = jack.open([&](float* out, uint32_t n, uint32_t ch) {
    if (!out || ch == 0) {
      sawNull.store(true);
      return;
    }
    channelsSeen.store(ch);
    for (uint32_t i = 0; i < n; ++i) {
      const float v = (float) std::sin(phase) * 0.2f;
      phase += 2.0 * 3.14159265358979 * 440.0 / 48000.0;
      for (uint32_t c = 0; c < ch; ++c) out[i * ch + c] = v;
    }
    frames.fetch_add(n);
    calls.fetch_add(1);
  });
  std::snprintf(msg, sizeof(msg), "a client registers and its ports open (%s)",
                opened ? "ok" : jack.error().c_str());
  check(opened, msg);
  if (!opened) {
    std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
    return 1;
  }

  // The rate and channel count are the SERVER's, not ours. A backend that
  // reported its own preference would have the DSP prepared at the wrong rate
  // and everything a semitone out.
  std::snprintf(msg, sizeof(msg), "the server's rate is %.0f Hz and it gives %u channels",
                jack.sampleRate(), jack.channels());
  check(jack.sampleRate() > 0.0 && jack.channels() > 0, msg);

  check(jack.run(), "the client activates");

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  const uint64_t got = frames.load();
  const uint32_t n = calls.load();
  std::snprintf(msg, sizeof(msg), "in half a second the server called back %u times", n);
  check(n > 0, msg);
  check(!sawNull.load(), "and never with a null buffer or zero channels");

  // THE measurement: frames delivered against wall-clock time. A callback that
  // fires at the wrong rate, or twice per cycle, or with the wrong frame count
  // shows up here and nowhere else.
  const double seconds = (double) got / jack.sampleRate();
  std::snprintf(msg, sizeof(msg), "which is %llu frames -- %.3f seconds of audio for 0.5 seconds "
                "of wall clock", (unsigned long long) got, seconds);
  check(seconds > 0.35 && seconds < 0.75, msg);

  std::snprintf(msg, sizeof(msg), "at %u channels, as the server said", channelsSeen.load());
  check(channelsSeen.load() == jack.channels(), msg);

  // ── Stopping ─────────────────────────────────────────────────────────────
  //
  // deactivate WAITS for the current callback to finish. Without that, closing
  // frees the ports underneath a callback that is still running -- which is a
  // crash in the server's thread, not ours, and therefore one that looks like
  // JACK's fault.
  const uint32_t before = calls.load();
  jack.stop();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const uint32_t after = calls.load();
  std::snprintf(msg, sizeof(msg), "after stop, %u further callbacks arrived", after - before);
  check(after == before, msg);

  // Stopping twice is what a host does, and must not be a crash.
  jack.stop();
  check(true, "and stopping again is harmless");

  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  if (g_failures == 0) std::printf("SONORE JACK TEST PASSED\n");
  return g_failures == 0 ? 0 : 1;
}
