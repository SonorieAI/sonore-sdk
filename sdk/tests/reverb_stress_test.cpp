// SPDX-License-Identifier: Apache-2.0
// The reverb example's impulse handover, under contention.
//
// A state load is [main-thread] and may arrive while process() runs, and the
// reverb's state is an impulse response. The example used to load it straight
// into the convolvers process() was reading -- rewriting their spectra and
// resetting their history under the audio thread. It now loads each impulse
// into a Room of its own and publishes it; process() adopts it at a block
// boundary and hands the old one back to be freed.
//
// This restores state -- two recorded impulses and the built-in room in turn
// -- as fast as the main thread can while another thread renders without
// pause. Under ThreadSanitizer (scripts/sdk-sanitize.mjs) the old example is a
// race report on every load; under AddressSanitizer a free of a Room still in
// use would be one; here, a torn impulse is output that is not finite or not
// bounded. And single-threaded first, the handover itself: a load leaves the
// Room process() is using alone, and the next block adopts the new one.
#define SONORE_EXAMPLE_DSP_ONLY 1
#include "../examples/reverb/plugin.cpp"

#include <sonore/wav.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
  double seconds = 3.0;
  if (argc > 1) seconds = std::atof(argv[1]);
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  int failures = 0;
  auto check = [&](bool ok, const char* what) {
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
  };

  // Two impulses of different lengths and channel counts, both with a large
  // DC sum: fed a constant, the wet output settles at a level of about ten,
  // and moves by hundredths per sample while it ramps -- so a convolver reset
  // or a half-written impulse inside a block is a jump nothing else makes.
  const std::string dir = std::filesystem::temp_directory_path().string();
  const std::string pathA = dir + "/sonore-reverb-stress-a.wav";
  const std::string pathB = dir + "/sonore-reverb-stress-b.wav";
  {
    std::vector<float> a(9600 * 2), b(4800);
    for (size_t i = 0; i < 9600; ++i) {
      a[i * 2] = std::exp(-0.0008f * (float) i);
      a[i * 2 + 1] = std::exp(-0.0011f * (float) i);
    }
    for (size_t i = 0; i < b.size(); ++i) b[i] = std::exp(-0.002f * (float) i) * ((i % 7) ? 0.1f : 0.9f);
    if (!sonore::writeWav(pathA.c_str(), a.data(), 9600, 2, 48000) ||
        !sonore::writeWav(pathB.c_str(), b.data(), 4800, 1, 44100)) {
      std::printf("  FAIL could not write the two impulses to %s\n", dir.c_str());
      return 1;
    }
  }

  sonore::ProcessSpec spec;
  spec.sampleRate = 48000.0;
  spec.maximumBlockSize = 256;
  spec.numChannels = 2;
  const float params[4] = {12.0f, 100.0f, 1.0f, 0.0f};

  // ── The handover, one thread ──────────────────────────────────────────────
  {
    std::unique_ptr<SonoreDsp> dsp(new SonoreDsp());
    dsp->prepare(spec);
    std::vector<float> l(256, 0.0f), r(256, 0.0f);
    float* ch[2] = {l.data(), r.data()};
    sonore::AudioBlock<float> block(ch, 2, 256);
    dsp->process(block, params);

    SonoreDsp::Room* before = dsp->room;
    sonore::StateBag bag;
    bag.setString("impulsePath", pathA);
    dsp->loadState(bag);
    check(dsp->room == before, "a state load leaves the Room process() is using alone");
    // The samples are let go once the new Room holds them as spectra -- held,
    // they were 0.45 s of stereo at the session rate, read by nothing -- and
    // the tail still answers for what was loaded: pre-delay's 200 ms, the
    // file's 9600 frames and two blocks.
    char kept[160];
    std::snprintf(kept, sizeof(kept), "the response is not kept once loaded (%zu frames), and the "
                  "tail is %d", dsp->impulse.length(), dsp->tailSamples());
    check(dsp->impulse.length() == 0 && dsp->tailSamples() == 9600 + 9600 + 2 * 512, kept);
    dsp->process(block, params);
    check(dsp->room != before && dsp->room != nullptr, "...and the next block adopts the new one");
  }

  // ── Both threads at once ──────────────────────────────────────────────────
  std::unique_ptr<SonoreDsp> dsp(new SonoreDsp());
  dsp->prepare(spec);

  std::atomic<bool> stop{false}, bad{false};
  std::atomic<long> blocks{0}, loads{0}, midBlockJumps{0};
  std::thread audio([&] {
    std::vector<float> l(256), r(256);
    float* ch[2] = {l.data(), r.data()};
    while (!stop.load(std::memory_order_acquire)) {
      for (size_t i = 0; i < 256; ++i) l[i] = r[i] = 0.25f;
      sonore::AudioBlock<float> block(ch, 2, 256);
      dsp->process(block, params);
      for (size_t i = 0; i < 256; ++i) {
        if (!std::isfinite(l[i]) || !std::isfinite(r[i]) || std::fabs(l[i]) > 50.0f ||
            std::fabs(r[i]) > 50.0f)
          bad.store(true);
        // A new impulse may only take over at a block boundary.
        if (i > 0 && std::fabs(l[i] - l[i - 1]) > 1.0f) midBlockJumps.fetch_add(1);
      }
      blocks.fetch_add(1);
    }
  });

  const auto until = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds((long long) (seconds * 1000.0));
  int k = 0;
  while (std::chrono::steady_clock::now() < until) {
    sonore::StateBag bag;
    bag.setString("impulsePath", (k % 3 == 0) ? pathA : (k % 3 == 1) ? pathB : "");
    dsp->loadState(bag);
    ++k;
    loads.fetch_add(1);
  }
  stop.store(true, std::memory_order_release);
  audio.join();
  std::remove(pathA.c_str());
  std::remove(pathB.c_str());

  std::printf("  ---- %ld blocks rendered against %ld state loads in %.1fs ----\n", blocks.load(),
              loads.load(), seconds);
  check(blocks.load() > 100, "the audio thread rendered throughout");
  check(loads.load() > 20, "the main thread kept loading throughout");
  check(!bad.load(), "every rendered sample stayed finite and bounded");
  char line[200];
  std::snprintf(line, sizeof(line), "an impulse only ever changed at a block boundary, never "
                "under a running block (%ld jumps inside one)", midBlockJumps.load());
  check(midBlockJumps.load() == 0, line);
  if (failures == 0) std::printf("SONORE REVERB HANDOVER TEST PASSED\n");
  return failures == 0 ? 0 : 1;
}
