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
// Every parameter stays at its default, which is also what a freshly inserted
// plugin has in the DAW. Prints "latency <n>" so the comparison can align.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sonore/host.h>
#include <sonore/wav.h>

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 2) {
    std::printf("usage: render_tool <plugin file> <in.wav> <out.wav> [--block N] [--describe]\n");
    return 2;
  }
  uint32_t block = 512;
  bool describe = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--block") == 0 && i + 1 < argc) block = (uint32_t) std::atoi(argv[++i]);
    if (std::strcmp(argv[i], "--describe") == 0) describe = true;
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
  const uint32_t channels = 2;
  if (!plugin->prepare((double) in.sampleRate, block, channels)) {
    std::printf("error: prepare(%u Hz, %u) refused\n", in.sampleRate, block);
    return 1;
  }
  std::printf("latency %u\n", plugin->latencySamples());

  const size_t frames = in.numFrames();
  std::vector<float> left(frames), right(frames);
  for (size_t i = 0; i < frames; ++i) {
    left[i] = in.samples[i * in.numChannels];
    right[i] = in.numChannels > 1 ? in.samples[i * in.numChannels + 1] : left[i];
  }
  for (size_t at = 0; at < frames; at += block) {
    const size_t n = frames - at < block ? frames - at : block;
    float* chans[2] = {left.data() + at, right.data() + at};
    sonore::AudioBlock<float> io(chans, channels, n);
    plugin->process(io);
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
