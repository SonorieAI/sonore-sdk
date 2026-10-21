// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: render a WAV through a plugin, the way our own host does it.
//
// The reference half of the DAW render-parity check (scripts/daw-render.mjs):
// REAPER renders a file through a plugin, this renders the same file through
// the same binary with sonore/host.h, and the two must agree sample for
// sample once the plugin's reported latency is accounted for. A plugin whose
// sound depends on the host's block size, or that reports a latency other
// than the one it has, disagrees here and nowhere else.
//
//   render_tool <plugin file> <in.wav> <out.wav> [--block N] [--describe]
//
// --describe also lists the plugin's presets, each loaded in turn with every
// parameter's value and text after it: what a host's preset menu must set.
//               [--notes notes.txt] [--param <index>=<value>]... [--state blob]
//
// Every parameter stays at its default unless --param moves it, which is also
// what a freshly inserted plugin has in the DAW. Prints "latency <n>" so the
// comparison can align.
//
// --notes plays an instrument: one note per line, "<on frame> <off frame>
// <key> <velocity>", each event delivered at its own frame inside whichever
// block it falls in -- the same notes daw-render puts in a REAPER MIDI item.
//
// --param sets parameter <index> to a PLAIN value (the format's own, as CLAP
// speaks it) before the first block, the way a DAW session that moved a knob
// hands it over -- scripts/reaper-host-test.mjs renders the same values in
// REAPER. --state loads a saved state blob first (the SNRS bytes a CLAP
// session stores), which is how that script gives the sampler a file to play
// without a file dialog.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sonore/host.h>
#include <sonore/wav.h>

namespace {
struct Note {
  size_t on = 0, off = 0;
  int key = 60, velocity = 100;
};

bool readNotes(const char* path, std::vector<Note>& out) {
  std::FILE* f = std::fopen(path, "r");
  if (!f) return false;
  unsigned long long on = 0, off = 0;
  int key = 0, velocity = 0;
  while (std::fscanf(f, "%llu %llu %d %d", &on, &off, &key, &velocity) == 4)
    // A note must end after it starts. One that ends on its own first frame
    // had its off delivered before its on (offs go first at a shared frame),
    // and then rang to the end of the render.
    if (off > on) out.push_back({(size_t) on, (size_t) off, key, velocity});
    else std::printf("warning: note %d at frame %llu ends where it starts; skipped\n", key, on);
  std::fclose(f);
  return !out.empty();
}
} // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 2) {
    std::printf("usage: render_tool <plugin file> <in.wav> <out.wav> [--block N] [--describe]\n");
    return 2;
  }
  uint32_t block = 512;
  bool describe = false;
  std::vector<Note> notes;
  std::vector<std::pair<int, double>> params;
  std::vector<uint8_t> state;
  bool haveState = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--block") == 0 && i + 1 < argc) block = (uint32_t) std::atoi(argv[++i]);
    if (std::strcmp(argv[i], "--describe") == 0) describe = true;
    if (std::strcmp(argv[i], "--notes") == 0 && i + 1 < argc && !readNotes(argv[++i], notes)) {
      std::printf("error: cannot read notes from %s\n", argv[i]);
      return 1;
    }
    if (std::strcmp(argv[i], "--param") == 0 && i + 1 < argc) {
      const char* spec = argv[++i];
      const char* eq = std::strchr(spec, '=');
      if (!eq) {
        std::printf("error: --param wants <index>=<value>, got %s\n", spec);
        return 2;
      }
      params.push_back({std::atoi(spec), std::strtod(eq + 1, nullptr)});
    }
    if (std::strcmp(argv[i], "--state") == 0 && i + 1 < argc) {
      std::FILE* f = std::fopen(argv[++i], "rb");
      if (!f) {
        std::printf("error: cannot read state from %s\n", argv[i]);
        return 1;
      }
      uint8_t chunk[4096];
      size_t got = 0;
      while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0) state.insert(state.end(), chunk, chunk + got);
      std::fclose(f);
      haveState = true;
    }
  }
  const std::vector<sonore::host::PluginDescription> found =
      sonore::host::describeFile(argv[1]);
  if (found.empty()) {
    std::printf("error: %s describes no plugin\n", argv[1]);
    return 1;
  }
  if (describe) {
    for (const auto& d : found)
      std::printf("plugin\t%s\t%s\t%s\t%s\n", d.format.c_str(), d.name.c_str(), d.vendor.c_str(),
                  d.isInstrument ? "instrument" : "effect");
    // And every parameter, with the text the plugin itself gives its default:
    // what a DAW's generic panel must show for a freshly inserted instance.
    std::unique_ptr<sonore::host::HostedPlugin> probe = sonore::host::loadPlugin(found[0]);
    for (int i = 0; probe && probe->isValid() && i < probe->numParameters(); ++i) {
      const sonore::host::HostedParam& p = probe->parameter(i);
      std::printf("param\t%d\t%s\t%.9g\t%.9g\t%.9g\t%d\t%s\n", i, p.name.c_str(), p.minValue,
                  p.maxValue, p.defaultValue, p.stepped ? 1 : 0,
                  probe->parameterText(i, p.defaultValue).c_str());
    }
    for (int k = 0; probe && probe->isValid() && k < probe->numPresets(); ++k) {
      std::printf("preset\t%d\t%s\n", k, probe->presetName(k).c_str());
      if (!probe->loadPreset(k)) continue;
      for (int i = 0; i < probe->numParameters(); ++i) {
        const double v = probe->parameterValue(i);
        std::printf("presetparam\t%d\t%d\t%.9g\t%s\n", k, i, v, probe->parameterText(i, v).c_str());
      }
    }
    if (argc < 4) return 0;
  }
  if (argc < 4) return 2;

  sonore::WavData in;
  if (!sonore::readWav(argv[2], &in) || in.numChannels == 0 || in.numFrames() == 0) {
    std::printf("error: cannot read %s\n", argv[2]);
    return 1;
  }
  std::unique_ptr<sonore::host::HostedPlugin> plugin = sonore::host::loadPlugin(found[0]);
  if (!plugin || !plugin->isValid()) {
    std::printf("error: %s did not load\n", found[0].name.c_str());
    return 1;
  }
  // Before prepare, as a DAW restores a session: the state arrives with the
  // instance, and activation comes after.
  if (haveState && !plugin->loadState(state.data(), state.size())) {
    std::printf("error: the plugin refused the state blob\n");
    return 1;
  }
  const uint32_t channels = 2;
  if (!plugin->prepare((double) in.sampleRate, block, channels)) {
    std::printf("error: prepare(%u Hz, %u) refused\n", in.sampleRate, block);
    return 1;
  }
  for (const auto& p : params)
    if (!plugin->setParameterValue(p.first, p.second)) {
      std::printf("error: parameter %d does not exist\n", p.first);
      return 1;
    }
  std::printf("latency %u\n", plugin->latencySamples());

  const size_t frames = in.numFrames();
  std::vector<float> left(frames), right(frames);
  for (size_t i = 0; i < frames; ++i) {
    left[i] = in.samples[i * in.numChannels];
    right[i] = in.numChannels > 1 ? in.samples[i * in.numChannels + 1] : left[i];
  }
  sonore::MidiBuffer midi;
  for (size_t at = 0; at < frames; at += block) {
    const size_t n = frames - at < block ? frames - at : block;
    float* chans[2] = {left.data() + at, right.data() + at};
    sonore::AudioBlock<float> io(chans, channels, n);
    midi.clear();
    // Offs before ons at the same frame, so a key struck again on the frame
    // it was released is a new note rather than a cancelled one.
    for (const Note& note : notes)
      if (note.off >= at && note.off < at + n)
        midi.addEvent(sonore::MidiMessage::noteOff(0, note.key), (int) (note.off - at));
    for (const Note& note : notes)
      if (note.on >= at && note.on < at + n)
        midi.addEvent(sonore::MidiMessage::noteOn(0, note.key, note.velocity), (int) (note.on - at));
    plugin->process(io, notes.empty() ? nullptr : &midi);
  }
  std::vector<float> interleaved(frames * 2);
  for (size_t i = 0; i < frames; ++i) {
    interleaved[i * 2] = left[i];
    interleaved[i * 2 + 1] = right[i];
  }
  if (!sonore::writeWav(argv[3], interleaved.data(), frames, 2, in.sampleRate)) {
    std::printf("error: cannot write %s\n", argv[3]);
    return 1;
  }
  std::printf("rendered %zu frames of %s through %s at %u Hz in blocks of %u\n", frames, argv[2],
              found[0].name.c_str(), in.sampleRate, block);
  return 0;
}
