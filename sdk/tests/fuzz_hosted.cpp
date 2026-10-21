// SPDX-License-Identifier: Apache-2.0
// Every format, driven in every order our own host allows.
//
// fuzz_calls walks CLAP's state machine with a plugin compiled in. This walks
// sonore/host.h's -- the same random sequence of prepare and release at any
// rate, block size and channel count, blocks of any length with MIDI in them,
// resets, parameter writes, state round trips (some through a damaged blob),
// and the re-prepare a plugin asks for when its latency moves -- through
// every plugin in the folders it is given: the CLAP, VST3 and LV2 builds of
// each, and the host's three implementations with them. Two properties
// throughout:
//   * nothing crashes or reads out of bounds (run it under ASan for that);
//   * every block that went in finite comes out finite and below 100.
//
//   fuzz_hosted <plugin file or folder>... [--steps N] [--seed S]
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <sonore/host.h>

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  size_t steps = 3000;
  uint32_t seed = 0xBEEFu;
  std::vector<sonore::host::PluginDescription> plugins;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--steps") == 0 && i + 1 < argc) {
      steps = std::strtoull(argv[++i], nullptr, 10);
      continue;
    }
    if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
      seed = (uint32_t) std::strtoul(argv[++i], nullptr, 10);
      continue;
    }
    const auto inFile = sonore::host::describeFile(argv[i]);
    const auto found = inFile.empty() ? sonore::host::scanDirectory(argv[i]) : inFile;
    plugins.insert(plugins.end(), found.begin(), found.end());
  }
  if (plugins.empty()) {
    std::printf("usage: fuzz_hosted <plugin file or folder>... [--steps N] [--seed S]\n");
    return 2;
  }
  std::printf("Sonore SDK hosted-call fuzz -- %zu plugins, %zu steps each, seed %u\n",
              plugins.size(), steps, seed);

  const double rates[] = {22050.0, 44100.0, 48000.0, 88200.0, 96000.0, 192000.0};
  size_t totalBlocks = 0, failures = 0;
  for (const sonore::host::PluginDescription& d : plugins) {
    std::unique_ptr<sonore::host::HostedPlugin> p = sonore::host::loadPlugin(d);
    if (!p || !p->isValid()) {
      std::printf("  FAIL %s (%s) did not load\n", d.name.c_str(), d.format.c_str());
      ++failures;
      continue;
    }
    std::mt19937 rng(seed);
    auto pick = [&](uint32_t n) { return n ? rng() % n : 0u; };
    auto chance = [&](int percent) { return (int) pick(100) < percent; };
    bool prepared = false;
    uint32_t maxBlock = 0, channels = 2;
    double rate = 48000.0;
    size_t blocks = 0, broken = 0;
    std::vector<std::vector<float>> storage;
    std::vector<float*> chans;
    bool held[128] = {};
    sonore::MidiBuffer midi;

    for (size_t step = 0; step < steps; ++step) {
      const uint32_t roll = pick(100);
      if (!prepared || roll < 3) {
        if (prepared && chance(50)) p->release();
        rate = rates[pick(6)];
        // 16 .. 4096 mostly; one time in ten 8192 .. 65536, the blocks an
        // offline render asks for.
        maxBlock = chance(10) ? 1u << (13 + pick(4)) : 1u << (4 + pick(9));
        channels = chance(80) ? 2u : (chance(50) ? 1u : 4u);
        prepared = p->prepare(rate, maxBlock, channels);
        storage.assign(channels, std::vector<float>(maxBlock, 0.0f));
        chans.assign(channels, nullptr);
        for (uint32_t c = 0; c < channels; ++c) chans[c] = storage[c].data();
        continue;
      }
      if (roll < 70) {
        const uint32_t frames = chance(5) ? 0 : (chance(10) ? maxBlock : 1 + pick(maxBlock));
        midi.clear();
        if (p->acceptsMidi()) {
          for (uint32_t n = chance(40) ? pick(6) : 0; n > 0; --n) {
            const int at = frames ? (int) pick(frames) : 0;
            const int key = 36 + (int) pick(48);
            switch (pick(4)) {
              case 0:
              case 1:
                // Every draw in a statement of its own: a call's arguments
                // are evaluated in an unspecified order, and a seed has to
                // replay the same walk under any compiler.
                if (held[key] && chance(70)) {
                  const int channel = (int) pick(16);
                  midi.addEvent(sonore::MidiMessage::noteOff(channel, key), at);
                } else {
                  const int channel = (int) pick(16);
                  const int velocity = 1 + (int) pick(127);
                  midi.addEvent(sonore::MidiMessage::noteOn(channel, key, velocity), at);
                }
                held[key] = !held[key];
                break;
              case 2:
              {
                const int channel = (int) pick(16);
                const int controller = (int) pick(128);
                const int value = (int) pick(128);
                midi.addEvent(sonore::MidiMessage::controlChange(channel, controller, value), at);
              }
                break;
              default:
              {
                const int channel = (int) pick(16);
                const int bend = (int) pick(16384);
                midi.addEvent(sonore::MidiMessage::pitchBend(channel, bend), at);
              }
                break;
            }
          }
        }
        const int kind = (int) pick(4);
        uint32_t noise = rng();
        for (uint32_t c = 0; c < channels; ++c)
          for (uint32_t i = 0; i < frames; ++i) {
            noise = noise * 1664525u + 1013904223u;
            const float r = (float) ((double) noise / 4294967296.0 - 0.5);
            chans[c][i] = kind == 0 ? 0.0f : kind == 1 ? 0.5f * r : kind == 2 ? 4.0f * r : 1e-38f * r;
          }
        sonore::AudioBlock<float> io(chans.data(), channels, frames);
        p->process(io, &midi);
        ++blocks;
        float worst = 0.0f;
        bool finite = true;
        for (uint32_t c = 0; c < channels; ++c)
          for (uint32_t i = 0; i < frames; ++i) {
            const float v = chans[c][i];
            if (!std::isfinite(v)) finite = false;
            else if (std::fabs(v) > worst) worst = std::fabs(v);
          }
        if (!finite || worst > 100.0f) {
          ++broken;
          if (broken <= 3)
            std::printf("  FAIL %s (%s) step %zu: %u frames x %u at %g Hz came out %s (peak %g)\n",
                        d.name.c_str(), d.format.c_str(), step, frames, channels, rate,
                        finite ? "too loud" : "NOT FINITE", (double) worst);
        }
      } else if (roll < 80) {
        const int n = p->numParameters();
        if (n > 0) {
          const int i = (int) pick((uint32_t) n);
          const sonore::host::HostedParam& info = p->parameter(i);
          const double u = (double) pick(1001) / 1000.0;
          p->setParameterValue(i, chance(90) ? info.minValue + u * (info.maxValue - info.minValue)
                                             : (chance(50) ? info.maxValue + 5.0 : info.minValue - 5.0));
          (void) p->parameterText(i, p->parameterValue(i));
        }
      } else if (roll < 86) {
        p->reset();
      } else if (roll < 92) {
        std::vector<uint8_t> blob;
        if (p->saveState(blob)) {
          // A session file is not the host's to trust: one time in three the
          // blob is damaged first -- cut short, bytes flipped, a run of the
          // bytes that break size arithmetic, or garbage on the end -- and
          // whatever the plugin makes of it, refusing included, the blocks
          // after it must still come out finite.
          if (chance(33) && !blob.empty()) {
            switch (pick(4)) {
              case 0: blob.resize(pick((uint32_t) blob.size())); break;
              case 1:
                for (uint32_t n = 1 + pick(8); n > 0; --n) {
                  const size_t at = pick((uint32_t) blob.size());
                  const uint32_t bit = pick(8);
                  blob[at] ^= (uint8_t) (1u << bit);
                }
                break;
              case 2: {
                const uint8_t breakers[] = {0x00, 0xFF, 0x7F, 0x80};
                const uint8_t b = breakers[pick(4)];
                const size_t at = pick((uint32_t) blob.size());
                const size_t run = 4 + pick(12);
                for (size_t i = at; i < blob.size() && i < at + run; ++i) blob[i] = b;
                break;
              }
              default:
                for (uint32_t n = 1 + pick(64); n > 0; --n) blob.push_back((uint8_t) rng());
                break;
            }
          }
          p->loadState(blob.data(), blob.size());
        }
      } else if (roll < 96) {
        // What a host does when asked: prepare again, at the same settings.
        if (p->takeRestartRequest()) prepared = p->prepare(rate, maxBlock, channels);
        (void) p->takeStateDirty();
      } else {
        (void) p->latencySamples();
        (void) p->tailSamples();
      }
    }
    if (prepared) p->release();
    totalBlocks += blocks;
    failures += broken;
    std::printf("  %-34s %-5s %6zu blocks, %zu broken\n", d.name.c_str(), d.format.c_str(), blocks, broken);
    // A plugin the walk never got as far as processing passed every property
    // without being tested by any of them.
    if (blocks == 0) {
      ++failures;
      std::printf("  FAIL %s (%s) processed no block in %zu steps\n", d.name.c_str(), d.format.c_str(), steps);
    }
  }
  std::printf("\n%zu blocks across %zu plugins, %zu broke a property\n", totalBlocks, plugins.size(),
              failures);
  std::printf(failures == 0 ? "SONORE HOSTED-CALL FUZZ PASSED\n" : "SONORE HOSTED-CALL FUZZ FAILED\n");
  return failures == 0 ? 0 : 1;
}
