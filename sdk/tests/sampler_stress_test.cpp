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

  sonore::ProcessSpec spec;
  spec.sampleRate = 48000.0;
  spec.maximumBlockSize = 256;
  spec.numChannels = 2;

  // ── The envelope runs at the speed its knob says, mono or stereo ─────────
  // A mono sample is read twice per frame (left and right both take channel
  // 0), and the envelope used to step inside that read -- so on a mono file,
  // the demo sample included, every attack and release took HALF its time.
  // The same constant tone as a mono and as a stereo file, single-threaded,
  // with a 200 ms attack: the two must reach 90% of full level together.
  auto attackSeconds = [&](int channels) -> double {
    const std::string path = dir + "/sonore-sampler-attack.wav";
    std::vector<float> tone((size_t) 96000 * (size_t) channels);
    for (size_t i = 0; i < 96000; ++i)
      for (int c = 0; c < channels; ++c)
        tone[i * (size_t) channels + (size_t) c] =
            0.5f * (float) std::sin(2.0 * 3.14159265358979 * 1000.0 * (double) i / 48000.0);
    if (!sonore::writeWav(path.c_str(), tone.data(), 96000, (uint16_t) channels, 48000))
      return -1.0;
    SonoreDsp probe;
    probe.prepare(spec);
    sonore::StateBag bag;
    bag.setString("samplePath", path);
    bag.setInt("rootNote", 60);
    probe.loadState(bag);

    const float attackParams[4] = {0.2f, 0.4f, 0.0f, 0.0f};
    std::vector<float> l(256), r(256);
    float* ch[2] = {l.data(), r.data()};
    sonore::MidiBuffer midi, midiOut;
    sonore::NoteExpressionBuffer expr;
    sonore::AudioBlock<float> aux[1] = {};
    float* scp[2] = {nullptr, nullptr};
    sonore::AudioBlock<float> sc(scp, 2, 0);
    std::vector<float> peaks;
    for (int b = 0; b < 150; ++b) {   // 0.8 s: the attack and then full level
      midi.clear();
      if (b == 0) midi.addEvent(sonore::MidiMessage::noteOn(0, 60, 127), 0);
      sonore::AudioBlock<float> block(ch, 2, 256);
      sonore::ProcessContext ctx{block, aux, 0, sc, midi, midiOut, nullptr, &expr};
      probe.process(ctx, attackParams);
      float peak = 0.0f;
      for (float v : l) peak = std::fabs(v) > peak ? std::fabs(v) : peak;
      peaks.push_back(peak);
    }
    std::remove(path.c_str());
    const float full = peaks.back();
    if (!(full > 0.0f)) return -1.0;
    for (size_t b = 0; b < peaks.size(); ++b)
      if (peaks[b] >= 0.9f * full) return (double) (b * 256) / 48000.0;
    return -1.0;
  };
  const double monoAttack = attackSeconds(1);
  const double stereoAttack = attackSeconds(2);

  // ── A key struck again in its own release tail ───────────────────────────
  // The note-on resets the new voice's tuning, and the tuning that follows it
  // retunes it -- both through voiceForNote, which found the first voice on
  // the key: the OLD one, still releasing. The repeated note kept that
  // voice's previous bend and the dying tail took the new note's. Measured
  // as the new voice's rate over one block, in semitones of tune.
  double restruckTune = -100.0, tunedUnderBend = -100.0;
  {
    SonoreDsp probe;
    probe.prepare(spec);
    float longRelease[4] = {0.005f, 2.0f, 0.0f, 0.0f};
    std::vector<float> l(256), r(256);
    float* ch[2] = {l.data(), r.data()};
    sonore::MidiBuffer midi, midiOut;
    sonore::NoteExpressionBuffer expr;
    sonore::AudioBlock<float> aux[1] = {};
    float* scp[2] = {nullptr, nullptr};
    sonore::AudioBlock<float> sc(scp, 2, 0);
    auto tune = [&](float semitones) {
      sonore::NoteExpressionBuffer::Entry e;
      e.key = 57;
      e.channel = 0;
      e.expression = sonore::kExprTuning;
      e.value = semitones;
      expr.addEvent(e);
    };
    auto run = [&](int blocks) {
      for (int b = 0; b < blocks; ++b) {
        sonore::AudioBlock<float> block(ch, 2, 256);
        sonore::ProcessContext ctx{block, aux, 0, sc, midi, midiOut, nullptr, &expr};
        probe.process(ctx, longRelease);
        midi.clear();
        expr.clear();
      }
    };
    midi.addEvent(sonore::MidiMessage::noteOn(0, 57, 127), 0);
    tune(2.0f);
    run(20);
    midi.addEvent(sonore::MidiMessage::noteOff(0, 57), 0);
    run(2);
    midi.addEvent(sonore::MidiMessage::noteOn(0, 57, 127), 0);
    tune(5.0f);
    run(1);
    // The new note is the active voice nearest the start of the sample.
    int newest = -1;
    for (int v = 0; v < probe.voices.kNumVoices; ++v)
      if (probe.voices.voice(v).isActive() &&
          (newest < 0 || probe.voices.voice(v).player.position() <
                             probe.voices.voice(newest).player.position()))
        newest = v;
    auto tuneOfNewest = [&]() {
      const double from = probe.voices.voice(newest).player.position();
      run(1);
      const double moved = probe.voices.voice(newest).player.position() - from;
      // The demo sounds A (69) at 44.1 kHz, 3.93 cents flat of its file.
      const double untuned = std::pow(2.0, (57.0 - 69.0 - 0.0393) / 12.0) * 44100.0 / 48000.0 * 256.0;
      return 12.0 * std::log2(moved / untuned);
    };
    if (newest >= 0) {
      restruckTune = tuneOfNewest();
      // And the Tune control moved under it, the +5 still held. The control
      // set the voice's whole offset, so moving it -- automation moves it
      // every block -- put every sounding note's bend back to nothing.
      longRelease[2] = 0.5f;
      run(1);
      tunedUnderBend = tuneOfNewest();
    }
  }

  // ── A one-shot that ends while its key is held ───────────────────────────
  // Its voice is free again, but its envelope was left at the level it had
  // when the sample ran out -- full, past a short attack -- and the next note
  // on that voice took its attack from there: at full level from the first
  // sample. The first 10 ms of two identical notes with a 150 ms attack.
  float firstAttackPeak = -1.0f, nextAttackPeak = -1.0f;
  {
    const std::string path = dir + "/sonore-sampler-oneshot.wav";
    std::vector<float> shot(9600); // 0.2 s
    for (size_t i = 0; i < shot.size(); ++i)
      shot[i] = 0.5f * (float) std::sin(2.0 * 3.14159265358979 * 1000.0 * (double) i / 48000.0);
    if (sonore::writeWav(path.c_str(), shot.data(), shot.size(), 1, 48000)) {
      SonoreDsp probe;
      probe.prepare(spec);
      sonore::StateBag bag;
      bag.setString("samplePath", path);
      bag.setInt("rootNote", 60);
      probe.loadState(bag);
      const float slowAttack[4] = {0.15f, 0.05f, 0.0f, 0.0f};
      std::vector<float> l(256), r(256);
      float* ch[2] = {l.data(), r.data()};
      sonore::MidiBuffer midi, midiOut;
      sonore::NoteExpressionBuffer expr;
      sonore::AudioBlock<float> aux[1] = {};
      float* scp[2] = {nullptr, nullptr};
      sonore::AudioBlock<float> sc(scp, 2, 0);
      auto run = [&](int blocks) {
        float peak = 0.0f;
        for (int b = 0; b < blocks; ++b) {
          sonore::AudioBlock<float> block(ch, 2, 256);
          sonore::ProcessContext ctx{block, aux, 0, sc, midi, midiOut, nullptr, &expr};
          probe.process(ctx, slowAttack);
          midi.clear();
          for (size_t i = 0; i < 240 && b == 0; ++i) peak = (std::max)(peak, std::fabs(l[i]));
        }
        return peak;
      };
      midi.addEvent(sonore::MidiMessage::noteOn(0, 60, 127), 0);
      firstAttackPeak = run(1);
      run(100); // the sample ends at 0.2 s; the key stays down to 0.5 s
      midi.addEvent(sonore::MidiMessage::noteOff(0, 60), 0);
      run(40);
      midi.addEvent(sonore::MidiMessage::noteOn(0, 60, 127), 0);
      nextAttackPeak = run(1);
    }
    std::remove(path.c_str());
  }

  SonoreDsp dsp;
  dsp.prepare(spec);
  // Two instances on the built-in demo play ONE copy of it: the samples are
  // the same 1.2 s in each and never written, and a copy apiece was most of
  // what an instance held.
  bool demoShared = false;
  {
    SonoreDsp other;
    demoShared = dsp.latest->data.interleaved == other.latest->data.interleaved &&
                 other.latest->data.numFrames == 52920;
  }

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

  // The wrapper's file hook, which a page's chooseFile() answers through: it
  // loads the file and tells the host the state changed, once; a cancel (an
  // empty path) does neither. The example's loader had the wrong signature,
  // so the hook was never found and nothing could load a sample.
  bool cancelIgnored = false, hookLoaded = false, dirtyOnce = false;
  {
    SonoreDsp picker;
    picker.prepare(spec);
    picker.loadFile("sample", "");
    cancelIgnored = !picker.consumeStateDirty();
    picker.loadFile("sample", pathB.c_str());
    sonore::StateBag saved;
    picker.saveState(saved);
    hookLoaded = saved.getString("samplePath", "") == pathB;
    dirtyOnce = picker.consumeStateDirty() && !picker.consumeStateDirty();
  }

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
  std::printf("  ---- 200 ms attack reaches 90%%: mono %.3f s, stereo %.3f s ----\n", monoAttack,
              stereoAttack);
  check(stereoAttack > 0.1 && stereoAttack < 0.25,
        "a stereo sample's attack takes the time its knob says");
  check(monoAttack > 0.0 && std::fabs(monoAttack - stereoAttack) <= 0.02,
        "...and a mono sample's takes the same time, not half of it");
  std::printf("  ---- a key re-struck during its release, tuned +5: the new note plays %+.2f ----\n",
              restruckTune);
  check(std::fabs(restruckTune - 5.0) < 0.05,
        "a key struck again in its release tail plays the tuning sent with it");
  std::printf("  ---- the Tune control moved to +0.5 under that +5 bend: the note plays %+.2f ----\n",
              tunedUnderBend);
  check(std::fabs(tunedUnderBend - 5.5) < 0.05,
        "moving the Tune control keeps a sounding note's bend on top of it");
  std::printf("  ---- 150 ms attack, first 5 ms: first note %.4f, next note on the voice %.4f ----\n",
              firstAttackPeak, nextAttackPeak);
  check(firstAttackPeak > 0.0f && nextAttackPeak <= firstAttackPeak * 1.05f,
        "a note on a voice whose one-shot ended while held still has its attack");
  check(hookLoaded && dirtyOnce && cancelIgnored,
        "a file picked through the wrapper's loadFile hook loads, and marks the session changed once");
  check(demoShared, "two instances on the demo sample share one copy of its 52920 frames");
  if (failures == 0) std::printf("SONORE SAMPLER HANDOVER TEST PASSED\n");
  return failures == 0 ? 0 : 1;
}
