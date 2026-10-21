// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: sonore/host.h finds a reported parameter without scanning.
//
// Each parameter event a plugin emits names the parameter by the plugin's own
// id, and the host turns that into a position in its list -- on the audio
// thread, inside process(). Done by scanning the list, the cost of one event
// grows with the parameter count, and a preset load that reports every
// parameter costs the square of it in one block.
//
// The fixture file holds two plugins alike but for their size, 250 and 4000
// parameters, each reporting all of them every block with scattered ids. The
// test checks every report names the right parameter, then compares what one
// event costs in each: a scan makes the large one about sixteen times dearer
// per event, a hashed lookup the same.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
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

/** Nanoseconds per reported parameter for one plugin of the fixture, after
 *  checking every report in a block arrives naming the parameter it should. */
static double perEventNs(const std::vector<sonore::host::PluginDescription>& found, const char* id,
                         int expectedCount) {
  const sonore::host::PluginDescription* description = nullptr;
  for (const auto& d : found)
    if (d.id == id) description = &d;
  char line[200];
  std::snprintf(line, sizeof(line), "%s is described", id);
  check(description != nullptr, line);
  if (!description) return 0.0;

  auto plugin = sonore::host::loadPlugin(*description);
  std::snprintf(line, sizeof(line), "...loads, with its %d parameters", expectedCount);
  check(plugin != nullptr && plugin->numParameters() == expectedCount, line);
  if (!plugin) return 0.0;
  check(plugin->prepare(48000.0, 128, 2), "...and prepares");

  std::vector<float> l(128, 0.0f), r(128, 0.0f);
  float* ch[2] = {l.data(), r.data()};
  std::vector<sonore::host::ParamEdit> edits;
  edits.reserve((size_t) expectedCount);

  sonore::AudioBlock<float> warm(ch, 2, 128);
  plugin->process(warm, nullptr);
  plugin->drainParameterEdits(edits);
  int wrong = 0;
  for (const auto& e : edits)
    if (e.kind != sonore::host::ParamEdit::Kind::kValue || e.index != (int) e.value) ++wrong;
  std::snprintf(line, sizeof(line), "...every one of a block's %zu reports names its own parameter "
                "(%d did not)", edits.size(), wrong);
  check(edits.size() == (size_t) expectedCount && wrong == 0, line);

  double best = 1e9;
  for (int round = 0; round < 7; ++round) {
    edits.clear();
    sonore::AudioBlock<float> io(ch, 2, 128);
    const auto t0 = std::chrono::steady_clock::now();
    plugin->process(io, nullptr);
    best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    plugin->drainParameterEdits(edits);
  }
  plugin->release();
  return best * 1e9 / (double) expectedCount;
}

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 2) {
    std::printf("usage: host_params_test <SonoreParamsFixture.clap>\n");
    return 2;
  }
  std::printf("── CLAP parameter reports, by id: %s ──\n", argv[1]);

  const auto found = sonore::host::describeFile(argv[1]);
  check(found.size() == 5, "the five fixture plugins are described");

  const double smallNs = perEventNs(found, "com.sonorie.test.params.small", 250);
  const double largeNs = perEventNs(found, "com.sonorie.test.params.large", 4000);

  char line[200];
  std::snprintf(line, sizeof(line), "one report costs about the same with 4000 parameters as with "
                "250 (%.1f ns vs %.1f ns, %.1fx)", largeNs, smallNs,
                smallNs > 0.0 ? largeNs / smallNs : 0.0);
  check(smallNs > 0.0 && largeNs < 4.0 * smallNs, line);

  // A name, a module and a value's text are arrays the plugin fills, and a
  // plugin that copies a string as long as the array -- strncpy does --
  // leaves no terminator. The host read each to the first zero byte: a
  // name ran on into the module after it, and a module, the struct's last
  // string, ran off the end of the struct into the host's stack.
  {
    const sonore::host::PluginDescription* d = nullptr;
    for (const auto& candidate : found)
      if (candidate.id == "com.sonorie.test.params.unterminated") d = &candidate;
    auto plugin = d ? sonore::host::loadPlugin(*d) : nullptr;
    check(plugin != nullptr && plugin->numParameters() == 3,
          "the plugin with unterminated strings loads, with its three parameters");
    if (plugin && plugin->numParameters() == 3) {
      const std::string name = plugin->parameter(0).name;
      std::snprintf(line, sizeof(line), "a 256-byte name with no terminator is those 256 bytes "
                    "(%zu read)", name.size());
      check(name == std::string(256, 'N'), line);
      const std::string module = plugin->parameter(1).module;
      std::snprintf(line, sizeof(line), "...a 1024-byte module is those 1024 bytes (%zu read)",
                    module.size());
      check(module == std::string(1024, 'M'), line);
      const std::string text = plugin->parameterText(0, 0.5);
      std::snprintf(line, sizeof(line), "...and a value's text filling its buffer stops at the "
                    "buffer's end (%zu read)", text.size());
      check(!text.empty() && text.size() <= 128 && text == std::string(text.size(), 'T'), line);
    }
  }

  // A count is the plugin's word too. One that says 4,294,967,295 -- a
  // field it never set -- had the host reserve a HostedParam for each, and
  // loadPlugin() threw std::bad_alloc into the caller.
  {
    const sonore::host::PluginDescription* d = nullptr;
    for (const auto& candidate : found)
      if (candidate.id == "com.sonorie.test.params.garbage-count") d = &candidate;
    bool threw = false;
    int count = -1;
    const auto t0 = std::chrono::steady_clock::now();
    try {
      auto plugin = d ? sonore::host::loadPlugin(*d) : nullptr;
      count = plugin ? plugin->numParameters() : -1;
    } catch (const std::exception&) {
      threw = true;
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::snprintf(line, sizeof(line), "a plugin claiming 4294967295 parameters loads with the %d "
                  "it has, in %.2f s%s", count, secs, threw ? " -- NO, loadPlugin() threw" : "");
    check(!threw && count == 3 && secs < 5.0, line);
  }

  // A parameter list that changes. An edit names a parameter by its place
  // in the list, and a block's edits wait in a queue for the caller's next
  // drain. A state loaded in between that takes the plugin from 250
  // parameters to 10 -- CLAP_PARAM_RESCAN_ALL, while it is inactive -- left
  // the queue naming places 10 to 249, handed out after the list was 10
  // long: a caller doing parameter(e.index) read past the end of it.
  {
    const sonore::host::PluginDescription* d = nullptr;
    for (const auto& candidate : found)
      if (candidate.id == "com.sonorie.test.params.shrinks") d = &candidate;
    auto plugin = d ? sonore::host::loadPlugin(*d) : nullptr;
    const bool ready = plugin && plugin->numParameters() == 250 && plugin->prepare(48000.0, 128, 2);
    check(ready, "the plugin whose list shrinks loads with 250 parameters and prepares");
    if (ready) {
      std::vector<float> l(128, 0.0f), r(128, 0.0f);
      float* ch[2] = {l.data(), r.data()};
      sonore::AudioBlock<float> io(ch, 2, 128);
      plugin->process(io, nullptr); // 250 edits waiting
      plugin->release();
      std::vector<uint8_t> blob;
      const bool loaded = plugin->saveState(blob) && plugin->loadState(blob.data(), blob.size());
      std::vector<sonore::host::ParamEdit> edits;
      plugin->drainParameterEdits(edits);
      size_t outside = 0;
      for (const auto& e : edits)
        if (e.index < 0 || e.index >= plugin->numParameters()) ++outside;
      std::snprintf(line, sizeof(line), "after a state takes it to %d parameters, no edit drained "
                    "names a place past them (%zu of %zu did)", plugin->numParameters(), outside,
                    edits.size());
      check(loaded && plugin->numParameters() == 10 && outside == 0, line);
    }
  }

  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  if (g_failures == 0) std::printf("SONORE HOST PARAMS PASSED\n");
  return g_failures == 0 ? 0 : 1;
}
