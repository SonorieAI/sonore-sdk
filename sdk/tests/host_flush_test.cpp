// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: sonore/host.h never flushes a CLAP plugin beside process().
//
// A plugin that asks for a flush while audio runs gets its events through the
// next process(), which delivers and collects exactly what a flush would; the
// caller's main thread flushes only once audio has stopped, and then not while
// a block starts. The fixture counts every flush() that finds process() still
// running, and exposes the count as its read-only parameter 1.

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
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

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 2) {
    std::printf("usage: host_flush_test <SonoreFlushFixture.clap>\n");
    return 2;
  }
  std::printf("── CLAP flush beside process: %s ──\n", argv[1]);

  const auto found = sonore::host::describeFile(argv[1]);
  check(!found.empty(), "the fixture is described");
  if (found.empty()) return 1;
  auto plugin = sonore::host::loadPlugin(found.front());
  check(plugin != nullptr && plugin->numParameters() == 2, "the fixture loads, with its two parameters");
  if (!plugin) return 1;
  check(plugin->prepare(48000.0, 128, 2), "...and prepares");

  // Audio running flat out on its own thread; the caller's UI loop draining
  // every millisecond, as a host polling for automation does.
  std::atomic<bool> stop{false};
  std::atomic<unsigned long> blocks{0};
  std::thread audio([&] {
    std::vector<float> l(128, 0.0f), r(128, 0.0f);
    float* ch[2] = {l.data(), r.data()};
    while (!stop.load()) {
      sonore::AudioBlock<float> io(ch, 2, 128);
      plugin->process(io, nullptr);
      blocks.fetch_add(1);
    }
  });
  std::vector<sonore::host::ParamEdit> edits;
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
  while (std::chrono::steady_clock::now() < until) {
    plugin->drainParameterEdits(edits);
    edits.clear();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  stop.store(true);
  audio.join();

  char line[200];
  const int overlaps = (int) plugin->parameterValue(1);
  std::snprintf(line, sizeof(line), "with audio running, no flush() lands inside a process() "
                "(%d did, over %lu blocks)", overlaps, blocks.load());
  check(overlaps == 0, line);

  // Audio stopped, the plugin's last request still standing: now the main
  // thread flushes, and the change queued meanwhile goes with it -- the
  // transport-stopped case drainParameterEdits() exists for.
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  plugin->setParameterValue(0, 0.7);
  plugin->drainParameterEdits(edits);
  std::snprintf(line, sizeof(line), "with audio stopped, the main thread flushes and the queued "
                "change arrives (value %.3f)", plugin->parameterValue(0));
  check(std::fabs(plugin->parameterValue(0) - 0.7) < 1e-9, line);

  plugin->release();
  plugin.reset();

  // A flush asked for before the plugin was ever prepared, with a change the
  // caller queued meanwhile -- a session restoring its values before audio
  // starts. The flush packed the change into an event buffer that prepare()
  // had not sized yet: it did not fit, and was dropped from the queue
  // anyway, so the plugin never heard it, not in the flush and not in the
  // first block either.
  const sonore::host::PluginDescription* early = nullptr;
  for (const auto& d : found)
    if (d.id == "com.sonorie.test.flush-early") early = &d;
  check(early != nullptr, "the fixture that asks for a flush in init() is described");
  if (early) {
    auto fresh = sonore::host::loadPlugin(*early);
    check(fresh != nullptr, "...and loads");
    if (fresh) {
      fresh->setParameterValue(0, 0.75);
      std::vector<sonore::host::ParamEdit> before;
      fresh->drainParameterEdits(before);
      const double flushed = fresh->parameterValue(0);
      const bool prepared = fresh->prepare(48000.0, 128, 2);
      std::vector<float> l(128, 0.0f), r(128, 0.0f);
      float* ch[2] = {l.data(), r.data()};
      sonore::AudioBlock<float> io(ch, 2, 128);
      fresh->process(io, nullptr);
      std::snprintf(line, sizeof(line), "a change queued before the first prepare reaches the "
                    "plugin through that flush (value %.3f) and is there after a block (%.3f)",
                    flushed, fresh->parameterValue(0));
      check(prepared && std::fabs(flushed - 0.75) < 1e-9 &&
                std::fabs(fresh->parameterValue(0) - 0.75) < 1e-9,
            line);
      fresh->release();
    }
  }

  // A note port that speaks only CLAP's own note dialect. The host sent every
  // port MIDI, and this one heard no notes at all; it must be sent CLAP note
  // events, and nothing it cannot take (a controller has no CLAP spelling).
  const sonore::host::PluginDescription* notes = nullptr;
  for (const auto& d : found)
    if (d.id == "com.sonorie.test.clap-notes") notes = &d;
  check(notes != nullptr, "the fixture whose note port speaks only CLAP is described");
  if (notes) {
    auto synth = sonore::host::loadPlugin(*notes);
    check(synth != nullptr && synth->prepare(48000.0, 128, 2), "...and loads and prepares");
    // It is handed the notes of `midi`, as a VST3 plugin with an event input
    // is, so it says it takes them: answering false, it was sent none by any
    // caller that asks first -- which is what acceptsMidi() is for.
    check(synth != nullptr && synth->acceptsMidi(), "...and says it takes MIDI (acceptsMidi)");
    if (synth) {
      std::vector<float> l(128, 0.0f), r(128, 0.0f);
      float* ch[2] = {l.data(), r.data()};
      sonore::AudioBlock<float> io(ch, 2, 128);
      sonore::MidiBuffer midi;
      midi.addEvent(sonore::MidiMessage::noteOn(1, 60, 100), 10);
      midi.addEvent(sonore::MidiMessage::controlChange(1, 1, 64), 20);
      midi.addEvent(sonore::MidiMessage::noteOff(1, 60), 30);
      synth->process(io, &midi);
      auto valueOf = [&](const char* name) {
        for (int i = 0; i < synth->numParameters(); ++i)
          if (synth->parameter(i).name == name) return synth->parameterValue(i);
        return -1.0;
      };
      const double on = valueOf("Notes on"), off = valueOf("Notes off"), raw = valueOf("MIDI events");
      std::snprintf(line, sizeof(line), "it hears the note as CLAP note events (%.0f on, %.0f off) "
                    "and is sent no MIDI (%.0f events)", on, off, raw);
      check(on == 1.0 && off == 1.0 && raw == 0.0, line);
      synth->release();
    }
  }

  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  if (g_failures == 0) std::printf("SONORE HOST FLUSH PASSED\n");
  return g_failures == 0 ? 0 : 1;
}
