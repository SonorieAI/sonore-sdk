// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: sonore/host.h negotiates a VST3 plugin's buses and honours
// the answer.
//
// Mono is asked for as kMono, every audio bus is part of the request, and a
// refusal is followed by asking what the plugin will use instead. process()
// then gets a buffer for every bus at its agreed width, whatever the caller's
// own channel count; the caller's channels meet the MAIN buses, as they do
// for CLAP.
//
// The fixture processors count every block whose buses differ from what they
// agreed to (SONORE_VST3_BUS_VIOLATIONS in the process environment) and leave
// it untouched; one that matches comes out at twice its main input.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <sonore/host.h>

static int g_checks = 0;
static int g_failures = 0;

static void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

static long long readEnv(const char* name) {
  char text[32] = {0};
#if defined(_WIN32)
  GetEnvironmentVariableA(name, text, (DWORD) sizeof(text));
#else
  if (const char* v = std::getenv(name)) std::snprintf(text, sizeof(text), "%s", v);
#endif
  return std::atoll(text);
}

static void clearEnv(const char* name) {
#if defined(_WIN32)
  SetEnvironmentVariableA(name, "0");
#else
  setenv(name, "0", 1);
#endif
}

struct Result {
  long long violations = -1, requested = 0;
  std::vector<float> first;
};

static Result run(const std::vector<sonore::host::PluginDescription>& found, const char* name,
                  const std::vector<float>& inputs) {
  Result r;
  const sonore::host::PluginDescription* description = nullptr;
  for (const auto& d : found)
    if (d.name == name) description = &d;
  if (!description) return r;
  auto plugin = sonore::host::loadPlugin(*description);
  if (!plugin) return r;
  clearEnv("SONORE_VST3_BUS_VIOLATIONS");
  clearEnv("SONORE_VST3_BUS_REQUESTED");
  const uint32_t chans = (uint32_t) inputs.size();
  if (!plugin->prepare(48000.0, 64, chans)) return r;
  r.requested = readEnv("SONORE_VST3_BUS_REQUESTED");

  std::vector<std::vector<float>> buffers(chans);
  std::vector<float*> ptrs(chans);
  for (uint32_t c = 0; c < chans; ++c) {
    buffers[c].assign(64, 0.0f);
    ptrs[c] = buffers[c].data();
  }
  for (int block = 0; block < 4; ++block) {
    for (uint32_t c = 0; c < chans; ++c) buffers[c].assign(64, inputs[c]);
    sonore::AudioBlock<float> io(ptrs.data(), (int) chans, 64);
    plugin->process(io, nullptr);
  }
  r.violations = readEnv("SONORE_VST3_BUS_VIOLATIONS");
  for (uint32_t c = 0; c < chans; ++c) r.first.push_back(buffers[c][0]);
  plugin->release();
  return r;
}

static bool same(float a, float b) { return std::fabs(a - b) < 1e-6f; }

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 2) {
    std::printf("usage: vst3_bus_test <SonoreVst3BusFixture.vst3>\n");
    return 2;
  }
  std::printf("── VST3 bus negotiation: %s ──\n", argv[1]);
  const auto found = sonore::host::describeFile(argv[1]);
  check(found.size() == 2, "both fixture processors are described");

  char line[240];
  const long long kMono = (long long) Steinberg_Vst_SpeakerArr_kMono;
  {
    const Result r = run(found, "Sonore Bus Fixture (sidechain)", {0.25f, -0.5f});
    std::snprintf(line, sizeof(line), "a processor with a sidechain gets every bus at its width "
                  "(%lld mismatched blocks)", r.violations);
    check(r.violations == 0, line);
    std::snprintf(line, sizeof(line), "...its main bus carries the caller's stereo and the "
                  "sidechain is silent (%.3f, %.3f)", r.first.size() > 0 ? r.first[0] : 0.0f,
                  r.first.size() > 1 ? r.first[1] : 0.0f);
    check(r.first.size() == 2 && same(r.first[0], 0.5f) && same(r.first[1], -1.0f), line);
  }
  {
    const Result r = run(found, "Sonore Bus Fixture (sidechain)", {0.25f});
    std::snprintf(line, sizeof(line), "mono is asked for as kMono (0x%llx), not one left speaker",
                  r.requested);
    check(r.requested == kMono, line);
    std::snprintf(line, sizeof(line), "...which a processor taking mono accepts, and runs "
                  "(%lld mismatched blocks, out %.3f)", r.violations,
                  r.first.empty() ? 0.0f : r.first[0]);
    check(r.violations == 0 && r.first.size() == 1 && same(r.first[0], 0.5f), line);
  }
  {
    const Result r = run(found, "Sonore Bus Fixture (stereo only)", {0.25f});
    std::snprintf(line, sizeof(line), "a stereo-only processor that refuses mono still gets the "
                  "stereo it kept (%lld mismatched blocks)", r.violations);
    check(r.violations == 0, line);
    std::snprintf(line, sizeof(line), "...the mono caller heard on both sides and the left back "
                  "(%.3f)", r.first.empty() ? 0.0f : r.first[0]);
    check(r.first.size() == 1 && same(r.first[0], 0.5f), line);
  }

  // A factory's vendor and a class's name are char arrays the plugin fills,
  // and one that copies a string as long as the array leaves no terminator.
  // The scan read each to the first zero byte: the vendor ran on into the
  // URL after it, and the name -- PClassInfo's last field -- off the end of
  // the struct into the scanning host's stack.
  {
#if defined(_WIN32)
    SetEnvironmentVariableA("SONORE_VST3_BUS_UNTERMINATED", "1");
#else
    setenv("SONORE_VST3_BUS_UNTERMINATED", "1", 1);
#endif
    const auto unterminated = sonore::host::describeFile(argv[1]);
    clearEnv("SONORE_VST3_BUS_UNTERMINATED");
    const std::string name = unterminated.empty() ? std::string() : unterminated.front().name;
    const std::string vendor = unterminated.empty() ? std::string() : unterminated.front().vendor;
    std::snprintf(line, sizeof(line), "a 64-byte class name and vendor with no terminator are "
                  "read as those 64 bytes (%zu and %zu read)", name.size(), vendor.size());
    check(unterminated.size() == 2 && name == std::string(64, 'C') &&
              vendor == std::string(64, 'V'),
          line);
  }

  // A class count is the plugin's word too. 2^31 - 1 of them, from a field
  // it never set, was two billion getClassInfo calls in one scan.
  {
#if defined(_WIN32)
    SetEnvironmentVariableA("SONORE_VST3_BUS_GARBAGE_COUNT", "1");
#else
    setenv("SONORE_VST3_BUS_GARBAGE_COUNT", "1", 1);
#endif
    const auto t0 = std::chrono::steady_clock::now();
    const auto counted = sonore::host::describeFile(argv[1]);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    clearEnv("SONORE_VST3_BUS_GARBAGE_COUNT");
    std::snprintf(line, sizeof(line), "a factory claiming 2147483647 classes is scanned for the %zu "
                  "it has, in %.2f s", counted.size(), secs);
    check(counted.size() == 2 && secs < 2.0, line);
  }

  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  if (g_failures == 0) std::printf("SONORE VST3 BUS PASSED\n");
  return g_failures == 0 ? 0 : 1;
}
