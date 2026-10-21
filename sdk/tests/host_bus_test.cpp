// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: sonore/host.h hands a CLAP plugin the ports it declared.
//
// One buffer per declared audio port, each as wide as its port, whatever the
// caller's own channel count: CLAP requires it, and a plugin reading its
// sidechain or its second channel without checking is within its rights. The
// caller's channels meet the MAIN ports -- a mono caller is heard on both
// sides of a stereo port, a mono port takes the caller's first channel, and a
// mono output fills every channel the caller has. Aux inputs are silent, aux
// outputs are the plugin's own business and never reach the caller.
//
// The fixture plugins count every block whose shape does not match what they
// declared (read-only parameter 0) and leave it untouched; one that matches
// comes out at twice its main input, plus the sidechain.
//
// And CLAP's start_processing() and reset() are the audio thread's: blocks run
// on a thread of their own here, as an audio callback's do, and the fixture
// counts a call that came from any other (parameter 1). A plugin that refuses
// to start is not processed -- its blocks are left as they came in -- and is
// asked again on the next one.

#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <sonore/host.h>

static int g_checks = 0;
static int g_failures = 0;

static void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

struct Result {
  int violations = -1;
  std::vector<float> first; // each caller channel's first output sample
  std::vector<float> blockFirst; // channel 0's first output sample, block by block
  int wrongThread = -1;
  int resets = -1;
};

/** One block of `inputs` (one constant per caller channel) through a plugin of
 *  the fixture prepared for that many channels. */
static Result run(const std::vector<sonore::host::PluginDescription>& found, const char* id,
                  const std::vector<float>& inputs) {
  Result r;
  const sonore::host::PluginDescription* description = nullptr;
  for (const auto& d : found)
    if (d.id == id) description = &d;
  if (!description) return r;
  auto plugin = sonore::host::loadPlugin(*description);
  if (!plugin) return r;
  const uint32_t chans = (uint32_t) inputs.size();
  if (!plugin->prepare(48000.0, 64, chans)) return r;

  std::vector<std::vector<float>> buffers(chans);
  std::vector<float*> ptrs(chans);
  for (uint32_t c = 0; c < chans; ++c) {
    buffers[c].assign(64, inputs[c]);
    ptrs[c] = buffers[c].data();
  }
  // Two blocks on one thread, a reset() from this one with that thread gone,
  // two blocks on another: an audio callback's thread can change across a
  // stop, and every call that belongs to it must follow it.
  auto blocks = [&](int count) {
    std::thread audio([&] {
      for (int block = 0; block < count; ++block) {
        for (uint32_t c = 0; c < chans; ++c) buffers[c].assign(64, inputs[c]);
        sonore::AudioBlock<float> io(ptrs.data(), (int) chans, 64);
        plugin->process(io, nullptr);
        r.blockFirst.push_back(buffers[0][0]);
      }
    });
    audio.join();
  };
  blocks(2);
  plugin->reset();
  blocks(2);
  r.violations = (int) plugin->parameterValue(0);
  r.wrongThread = (int) plugin->parameterValue(1);
  r.resets = (int) plugin->parameterValue(2);
  for (uint32_t c = 0; c < chans; ++c) r.first.push_back(buffers[c][0]);
  plugin->release();
  return r;
}

static bool same(float a, float b) { return std::fabs(a - b) < 1e-6f; }

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 2) {
    std::printf("usage: host_bus_test <SonoreBusFixture.clap>\n");
    return 2;
  }
  std::printf("── CLAP audio ports, as declared: %s ──\n", argv[1]);
  const auto found = sonore::host::describeFile(argv[1]);
  check(found.size() == 4, "all four fixture plugins are described");

  char line[240];
  {
    const Result r = run(found, "com.sonorie.test.bus.sidechain", {0.25f, -0.5f});
    std::snprintf(line, sizeof(line), "a sidechain plugin gets both its input ports and both its "
                  "output ports, at their widths (%d mismatched blocks)", r.violations);
    check(r.violations == 0, line);
    std::snprintf(line, sizeof(line), "...its main ports carry the caller's stereo, the sidechain "
                  "is silent, the aux output stays inside (%.3f, %.3f)",
                  r.first.size() > 0 ? r.first[0] : 0.0f, r.first.size() > 1 ? r.first[1] : 0.0f);
    check(r.first.size() == 2 && same(r.first[0], 0.5f) && same(r.first[1], -1.0f), line);
  }
  {
    const Result r = run(found, "com.sonorie.test.bus.stereo", {0.25f});
    std::snprintf(line, sizeof(line), "a stereo plugin hosted for one channel still gets two "
                  "(%d mismatched blocks)", r.violations);
    check(r.violations == 0, line);
    std::snprintf(line, sizeof(line), "...the mono input is heard on both, and the left comes "
                  "back (%.3f)", r.first.empty() ? 0.0f : r.first[0]);
    check(r.first.size() == 1 && same(r.first[0], 0.5f), line);
  }
  {
    const Result r = run(found, "com.sonorie.test.bus.mono", {0.25f, -0.5f});
    std::snprintf(line, sizeof(line), "a mono plugin hosted for two channels gets one "
                  "(%d mismatched blocks)", r.violations);
    check(r.violations == 0, line);
    std::snprintf(line, sizeof(line), "...it takes the first channel, and its output fills both "
                  "(%.3f, %.3f)", r.first.size() > 0 ? r.first[0] : 0.0f,
                  r.first.size() > 1 ? r.first[1] : 0.0f);
    check(r.first.size() == 2 && same(r.first[0], 0.5f) && same(r.first[1], 0.5f), line);
  }
  {
    const Result r = run(found, "com.sonorie.test.bus.stereo", {0.25f, -0.5f});
    std::snprintf(line, sizeof(line), "start_processing() and reset() run on the thread that "
                  "processes, not the one that prepared (%d from elsewhere)", r.wrongThread);
    check(r.wrongThread == 0, line);
    std::snprintf(line, sizeof(line), "...and the reset reaches the plugin, once (%d)", r.resets);
    check(r.resets == 1, line);
  }
  {
    const Result r = run(found, "com.sonorie.test.bus.reluctant", {0.25f, -0.5f});
    const auto at = [&r](size_t i) { return i < r.blockFirst.size() ? r.blockFirst[i] : -9.0f; };
    std::snprintf(line, sizeof(line), "a plugin that refuses to start twice: two blocks left as "
                  "they came, then processed (%.3f %.3f %.3f %.3f)", at(0), at(1), at(2), at(3));
    check(r.blockFirst.size() == 4 && same(at(0), 0.25f) && same(at(1), 0.25f) &&
              same(at(2), 0.5f) && same(at(3), 0.5f),
          line);
  }

  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  if (g_failures == 0) std::printf("SONORE HOST BUS PASSED\n");
  return g_failures == 0 ? 0 : 1;
}
