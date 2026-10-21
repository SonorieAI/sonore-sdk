// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: sonore/host.h keeps every kind of MIDI a VST3 plugin emits.
//
// A VST3 plugin hands its output MIDI to the host as events: notes as note
// events, controllers, channel pressure, pitch bend and program changes as
// kLegacyMIDICCOutEvent, poly pressure as its own event, SysEx as data
// events. The host kept the notes and refused the rest, so an arpeggiator's
// pass-through controller -- or a plugin driving a synth's filter -- never
// reached the next plugin in a graph.
//
// The SDK's own arpeggiator passes on what it does not understand; a
// controller sent in has to come back out.

#include <cstdio>
#include <string>

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
    std::printf("usage: vst3_host_midi_test <plugin dir>\n");
    return 2;
  }
  const std::string path = std::string(argv[1]) + "/SonoreArpVST3.vst3";
  std::printf("── VST3 MIDI out, through the host: %s ──\n", path.c_str());
  const auto found = sonore::host::describeFile(path);
  check(!found.empty(), "the arpeggiator's VST3 is described");
  if (found.empty()) return 1;
  auto plugin = sonore::host::loadPlugin(found.front());
  check(plugin != nullptr && plugin->prepare(48000.0, 256, 2), "it loads and prepares");
  if (!plugin) return 1;

  float l[256] = {}, r[256] = {};
  float* ch[2] = {l, r};
  // A controller, a program change, a pitch bend and channel pressure, every
  // block: each travels a different VST3 road out (kLegacyMIDICCOutEvent with
  // its own pseudo-controller number), and each has to come back as itself.
  int controllers = 0, program = 0, bend = 0, pressure = 0;
  for (int block = 0; block < 4; ++block) {
    sonore::MidiBuffer in;
    in.addEvent(sonore::MidiMessage(0xB0, 7, 100), 10);
    in.addEvent(sonore::MidiMessage(0xC0, 5, 0), 20);
    in.addEvent(sonore::MidiMessage::pitchBend(0, 12000), 30);
    in.addEvent(sonore::MidiMessage::channelPressure(0, 90), 40);
    sonore::AudioBlock<float> io(ch, 2, 256);
    plugin->process(io, &in);
    for (const auto& e : plugin->producedMidi()) {
      const auto& m = e.getMessage();
      if (m.getRawStatus() == 0xB0 && m.getRawData1() == 7 && m.getRawData2() == 100) ++controllers;
      if (m.getRawStatus() == 0xC0 && m.getRawData1() == 5) ++program;
      if (m.getRawStatus() == 0xE0 && (m.getRawData1() | (m.getRawData2() << 7)) == 12000) ++bend;
      if (m.getRawStatus() == 0xD0 && m.getRawData1() == 90) ++pressure;
    }
  }
  char line[200];
  std::snprintf(line, sizeof(line), "a controller the arpeggiator passes on reaches the host "
                "(%d of 4 blocks)", controllers);
  check(controllers == 4, line);
  std::snprintf(line, sizeof(line), "…and a program change (%d of 4)", program);
  check(program == 4, line);
  std::snprintf(line, sizeof(line), "…and a pitch bend, all 14 bits of it (%d of 4)", bend);
  check(bend == 4, line);
  std::snprintf(line, sizeof(line), "…and channel pressure (%d of 4)", pressure);
  check(pressure == 4, line);
  plugin->release();
  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  if (g_failures == 0) std::printf("SONORE VST3 HOST MIDI PASSED\n");
  return g_failures == 0 ? 0 : 1;
}
