// SPDX-License-Identifier: Apache-2.0
// The sampler example's sample handover, under contention.
//
// The example loads a file on the main thread while process() runs on the
// audio thread, and publishes each new sample through an atomic pointer with
// a lock-free retired stack going the other way (see examples/sampler). A
// design like that is only ever proven by RUNNING both sides at once: this
// test restores state -- a different file each time, the built-in demo every
// third -- as fast as the main thread can, while another thread renders
// notes without pause, for a few seconds. Under AddressSanitizer a
// use-after-free in the swap is a report; under ThreadSanitizer a missing
// fence is one; in plain ctest a torn pointer is a crash. The counters at the
// end prove both threads actually did their part.
//
// The first version of the example freed its sample vector on the main
// thread while voices read it. This test is what would have caught it.
#define SONORE_EXAMPLE_DSP_ONLY 1
#include "../examples/sampler/plugin.cpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
  double seconds = 4.0;
  if (argc > 1) seconds = std::atof(argv[1]);
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  // Two recordings with different lengths, rates and channel counts, written
  // by the SDK's own WAV writer into the temp directory.
  const std::string dir = std::filesystem::temp_directory_path().string();
  const std::string pathA = dir + "/sonore-sampler-stress-a.wav";
  const std::string pathB = dir + "/sonore-sampler-stress-b.wav";
  {
    std::vector<float> a(48000 * 2), b(12000);
    for (size_t i = 0; i < a.size(); ++i) a[i] = 0.5f * (float) std::sin(0.01 * (double) i);
    for (size_t i = 0; i < b.size(); ++i) b[i] = 0.25f * (float) std::sin(0.03 * (double) i);
    if (!sonore::writeWav(pathA.c_str(), a.data(), 48000, 2, 48000) ||
        !sonore::writeWav(pathB.c_str(), b.data(), 12000, 1, 44100)) {
      std::printf("  FAIL could not write the two recordings to %s\n", dir.c_str());
      return 1;
    }
  }

  SonoreDsp dsp;
  sonore::ProcessSpec spec;
  spec.sampleRate = 48000.0;
  spec.maximumBlockSize = 256;
  spec.numChannels = 2;
  dsp.prepare(spec);

  std::atomic<bool> stop{false};
  std::atomic<long> blocks{0}, loads{0}, silentBlocks{0};
  std::atomic<bool> bad{false};
  const float params[4] = {0.005f, 0.4f, 0.0f, 0.0f};

  std::thread audio([&] {
    std::vector<float> l(256), r(256);
    float* ch[2] = {l.data(), r.data()};
    sonore::MidiBuffer midi, midiOut;
    sonore::NoteExpressionBuffer expr;
    sonore::AudioBlock<float> aux[1] = {};
    float* scp[2] = {nullptr, nullptr};
    sonore::AudioBlock<float> sc(scp, 2, 0);
    int n = 0;
    while (!stop.load(std::memory_order_acquire)) {
      midi.clear();
      if ((n % 8) == 0) midi.addEvent(sonore::MidiMessage::noteOn(0, 48 + (n / 8) % 24, 100), 0);
      if ((n % 8) == 4) midi.addEvent(sonore::MidiMessage::noteOff(0, 48 + (n / 8) % 24), 3);
      sonore::AudioBlock<float> block(ch, 2, 256);
      sonore::ProcessContext ctx{block, aux, 0, sc, midi, midiOut, nullptr, &expr};
      dsp.process(ctx, params);
      float energy = 0.0f;
      for (int i = 0; i < 256; ++i) {
        const float v = l[(size_t) i];
        if (!(v == v) || std::fabs(v) > 10.0f) bad.store(true);
        energy += v * v;
      }
      if (energy == 0.0f) silentBlocks.fetch_add(1);
      ++n;
      blocks.fetch_add(1);
    }
  });

  const auto until = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds((long long) (seconds * 1000.0));
  int k = 0;
  while (std::chrono::steady_clock::now() < until) {
    sonore::StateBag bag;
    bag.setString("samplePath", (k % 3 == 0) ? "" : (k % 3 == 1) ? pathA : pathB);
    bag.setInt("rootNote", 60 + (k % 5));
    dsp.loadState(bag);
    sonore::StateBag out;
    dsp.saveState(out);
    ++k;
    loads.fetch_add(1);
  }
  stop.store(true, std::memory_order_release);
  audio.join();

  std::remove(pathA.c_str());
  std::remove(pathB.c_str());

  int failures = 0;
  auto check = [&](bool ok, const char* what) {
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
  };
  std::printf("  ---- %ld blocks rendered against %ld state loads in %.1fs ----\n", blocks.load(),
              loads.load(), seconds);
  check(blocks.load() > 100, "the audio thread rendered throughout");
  check(loads.load() > 20, "the main thread kept loading throughout");
  check(!bad.load(), "every rendered sample stayed finite and bounded");
  // Notes are retriggered every eight blocks and every swap kills the voices,
  // so some silence is expected; a render that was silent the whole time
  // would mean the swapped-in banks never played.
  check(silentBlocks.load() < blocks.load(), "the swapped-in samples were heard");
  if (failures == 0) std::printf("SONORE SAMPLER HANDOVER TEST PASSED\n");
  return failures == 0 ? 0 : 1;
}
