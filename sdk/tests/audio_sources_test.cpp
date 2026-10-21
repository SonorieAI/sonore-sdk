// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the pull-model sources (audio_source.h), held to numbers.
//
// Every source is checked against something independent of it: a memory
// source against the samples it was given, the filter and reverb sources
// against the SDK's own Biquad and Reverb run by hand on the same input (to
// the bit), the resampler against resample.h's whole-buffer Resampler and
// against the frequency and length the ratio says it should produce.
//
// The threaded claims are measured, not asserted:
//
//   * BufferingAudioSource behind a reader that sleeps: the samples that come
//     out are the samples an unbuffered read gives, and the slowest audio
//     callback is timed while the reader is starved -- a callback that waited
//     on the reader would take as long as the reader sleeps.
//   * MixerAudioSource with another thread adding and removing inputs as fast
//     as it can: every block must be a sum of SOME set of whole inputs. A torn
//     list or a freed input shows up as a value no subset makes, or as a
//     crash under ASan.
//   * operator new is replaced, and counts every allocation made on a thread
//     while it is inside an audio callback. The count must be zero.
//
// Locks cannot be counted portably; scripts/audio-thread-rules.mjs reads the
// [audio thread] bodies for them instead.

#include <sonore/audio_source.h>
#include <sonore/sample_stream.h>

#include "temp_path.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <thread>
#include <vector>

using namespace sonore;

// ── Allocation audit ─────────────────────────────────────────────────────────

namespace {
thread_local bool t_inAudioCallback = false;
std::atomic<long> g_audioAllocations{0};

void noteAllocation() {
  if (t_inAudioCallback) g_audioAllocations.fetch_add(1, std::memory_order_relaxed);
}

/** Marks the current thread as inside an audio callback for its lifetime. */
struct AudioScope {
  AudioScope() { t_inAudioCallback = true; }
  ~AudioScope() { t_inAudioCallback = false; }
};

// glibc's and macOS's allocators hand the block freed last straight back to
// the next request of its size; Windows' randomises. Inside a LifoHeap scope
// this one does the former, through the sized delete, so a test can make an
// address be reused the way it is on those platforms.
thread_local bool t_lifoHeap = false;
thread_local void* t_lastFreed = nullptr;
thread_local std::size_t t_lastFreedSize = 0;
struct LifoHeap {
  LifoHeap() { t_lifoHeap = true; }
  ~LifoHeap() {
    t_lifoHeap = false;
    std::free(t_lastFreed);
    t_lastFreed = nullptr;
  }
};
} // namespace

void* operator new(std::size_t n) {
  noteAllocation();
  if (t_lifoHeap && t_lastFreed && t_lastFreedSize == n) {
    void* p = t_lastFreed;
    t_lastFreed = nullptr;
    return p;
  }
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  noteAllocation();
  return std::malloc(n ? n : 1);
}
void* operator new[](std::size_t n, const std::nothrow_t& t) noexcept { return ::operator new(n, t); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t n) noexcept {
  if (t_lifoHeap && p) {
    std::free(t_lastFreed);
    t_lastFreed = p;
    t_lastFreedSize = n;
    return;
  }
  std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

// ── Harness ──────────────────────────────────────────────────────────────────

namespace {
int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

void checkf(bool ok, const char* format, ...) {
  char text[512];
  va_list args;
  va_start(args, format);
  std::vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  check(ok, text);
}

void section(const char* name) { std::printf("\n── %s\n", name); }

using Clock = std::chrono::steady_clock;
double microsSince(Clock::time_point t) {
  return std::chrono::duration<double, std::micro>(Clock::now() - t).count();
}

/** Owned planar storage a test can hand to a source as an AudioBlock. */
struct Planar {
  std::vector<std::vector<float>> data;
  std::vector<float*> pointers;
  int frames = 0;

  Planar(int channels, int numFrames, float fill = 0.0f) : frames(numFrames) {
    data.assign((size_t) channels, std::vector<float>((size_t) numFrames, fill));
    for (auto& d : data) pointers.push_back(d.data());
  }
  int channels() const { return (int) data.size(); }
  AudioBlock<float> block() { return AudioBlock<float>(pointers.data(), pointers.size(), (size_t) frames); }
  const float* ch(int c) const { return data[(size_t) c].data(); }
};

/** Pull `total` frames through `source` in blocks of `blockSize`, inside an
 *  audio scope, into one Planar. */
Planar pullAll(AudioSource& source, int channels, int total, int blockSize, float garbage = 0.0f) {
  Planar out(channels, total, garbage);
  AudioBlock<float> whole = out.block();
  AudioScope scope;
  for (int at = 0; at < total; at += blockSize) {
    const int n = (total - at) < blockSize ? (total - at) : blockSize;
    source.getNextAudioBlock(AudioSourceChannelInfo(&whole, at, n));
  }
  return out;
}

/** A deterministic noise value, never exactly zero. */
float noise(uint32_t& state) {
  state = state * 1664525u + 1013904223u;
  const float v = (float) ((state >> 8) & 0xFFFF) / 65536.0f * 1.6f - 0.8f;
  return v == 0.0f ? 0.001f : v;
}

/** A MemoryAudioSource over planar vectors (copied). */
std::unique_ptr<MemoryAudioSource> memoryOf(const std::vector<std::vector<float>>& chans, bool loop = false) {
  std::vector<const float*> p;
  for (const auto& c : chans) p.push_back(c.data());
  return std::unique_ptr<MemoryAudioSource>(
      new MemoryAudioSource(p.data(), (int) p.size(), chans.empty() ? 0 : (int) chans[0].size(), true, loop));
}

/** Frequency by counting rising zero crossings, interpolated to a fraction
 *  of a sample, over [from, to). */
double measureFrequency(const float* x, int from, int to, double sampleRate) {
  double first = -1.0, last = -1.0;
  int crossings = 0;
  for (int i = from + 1; i < to; ++i) {
    if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
      const double t = (double) (i - 1) + (double) (-x[i - 1]) / (double) (x[i] - x[i - 1]);
      if (first < 0.0) first = t;
      last = t;
      ++crossings;
    }
  }
  if (crossings < 2) return 0.0;
  return sampleRate * (double) (crossings - 1) / (last - first);
}

double rms(const float* x, int from, int to) {
  double sum = 0.0;
  for (int i = from; i < to; ++i) sum += (double) x[i] * (double) x[i];
  return std::sqrt(sum / (double) (to - from));
}

// ── Helpers that are sources ─────────────────────────────────────────────────

/** Remembers what it was handed and writes `in + 1000 * (channel + 1)`. Its
 *  memory is reserved up front: it runs inside the audit too, and a test
 *  helper that allocated there would be counted against the SDK. */
struct ProbeSource : AudioSource {
  std::vector<std::vector<float>> received;
  int prepared = 0, released = 0;
  ProbeSource(int channels, int frames) : received((size_t) channels) {
    for (auto& r : received) r.reserve((size_t) frames);
  }
  void prepareToPlay(int, double) override { ++prepared; }
  void releaseResources() override { ++released; }
  void getNextAudioBlock(const AudioSourceChannelInfo& info) override {
    for (int c = 0; c < info.numChannels() && c < (int) received.size(); ++c) {
      float* d = info.channel(c);
      for (int i = 0; i < info.numSamples; ++i) {
        received[(size_t) c].push_back(d[i]);
        d[i] = d[i] + 1000.0f * (float) (c + 1);
      }
    }
  }
};

/** Counts its lifecycle calls; outputs a constant. */
struct CountingSource : AudioSource {
  float value;
  std::atomic<int>* destroyed;
  std::atomic<int> prepared{0}, released{0};
  CountingSource(float v, std::atomic<int>* d = nullptr) : value(v), destroyed(d) {}
  ~CountingSource() override {
    if (destroyed) destroyed->fetch_add(1);
  }
  void prepareToPlay(int, double) override { prepared.fetch_add(1); }
  void releaseResources() override { released.fetch_add(1); }
  void getNextAudioBlock(const AudioSourceChannelInfo& info) override {
    for (int c = 0; c < info.numChannels(); ++c) {
      float* d = info.channel(c);
      for (int i = 0; i < info.numSamples; ++i) d[i] = value;
    }
  }
};

/** Leaves the buffer as it found it: what a player's input copy looks like
 *  from the source's side. */
struct PassThroughSource : AudioSource {
  void prepareToPlay(int, double) override {}
  void releaseResources() override {}
  void getNextAudioBlock(const AudioSourceChannelInfo&) override {}
};

/** A positionable source that takes its time: wraps a MemoryAudioSource and
 *  sleeps on every read, the way a slow disk behaves. Records which threads
 *  it was read on. */
struct SlowSource : PositionableAudioSource {
  std::unique_ptr<MemoryAudioSource> inner;
  std::atomic<int> delayMicros{0};
  std::atomic<int> reads{0};
  std::atomic<bool> readOnTestThread{false};
  std::thread::id testThread = std::this_thread::get_id();
  uint32_t jitter = 12345;

  explicit SlowSource(std::unique_ptr<MemoryAudioSource> m) : inner(std::move(m)) {}
  void prepareToPlay(int n, double sr) override { inner->prepareToPlay(n, sr); }
  void releaseResources() override { inner->releaseResources(); }
  void getNextAudioBlock(const AudioSourceChannelInfo& info) override {
    if (std::this_thread::get_id() == testThread) readOnTestThread.store(true);
    reads.fetch_add(1);
    const int d = delayMicros.load();
    if (d > 0) {
      jitter = jitter * 1664525u + 1013904223u;
      const int us = d / 2 + (int) ((jitter >> 8) % (uint32_t) (d / 2 + 1));
      std::this_thread::sleep_for(std::chrono::microseconds(us));
    }
    inner->getNextAudioBlock(info);
  }
  void setNextReadPosition(int64_t p) override { inner->setNextReadPosition(p); }
  int64_t getNextReadPosition() const override { return inner->getNextReadPosition(); }
  int64_t getTotalLength() const override { return inner->getTotalLength(); }
  bool isLooping() const override { return inner->isLooping(); }
  void setLooping(bool b) override { inner->setLooping(b); }
};

std::vector<std::vector<float>> noiseChannels(int channels, int frames, uint32_t seed) {
  std::vector<std::vector<float>> out((size_t) channels, std::vector<float>((size_t) frames));
  uint32_t state = seed;
  for (int i = 0; i < frames; ++i)
    for (int c = 0; c < channels; ++c) out[(size_t) c][(size_t) i] = noise(state);
  return out;
}

// ── MemoryAudioSource ────────────────────────────────────────────────────────

void testMemorySource() {
  section("MemoryAudioSource");
  std::vector<std::vector<float>> data(2, std::vector<float>(10));
  for (int i = 0; i < 10; ++i) {
    data[0][(size_t) i] = (float) (i + 1);
    data[1][(size_t) i] = -(float) (i + 1);
  }

  auto once = memoryOf(data, false);
  Planar a = pullAll(*once, 2, 16, 4, 99.0f);
  bool exact = true;
  for (int i = 0; i < 16; ++i) {
    const float want = i < 10 ? (float) (i + 1) : 0.0f;
    exact = exact && a.ch(0)[i] == want && a.ch(1)[i] == -want;
  }
  check(exact, "plays its ten frames, then silence (garbage underneath overwritten)");
  checkf(once->getNextReadPosition() == 16 && once->getTotalLength() == 10,
         "the position counts on through the silence (16) past the length (10)");

  auto looped = memoryOf(data, true);
  Planar b = pullAll(*looped, 2, 25, 7);
  exact = true;
  for (int i = 0; i < 25; ++i) exact = exact && b.ch(0)[i] == (float) (i % 10 + 1) && b.ch(1)[i] == -(float) (i % 10 + 1);
  check(exact, "looping wraps mid-block, block 7 over a length of 10");
  checkf(looped->getNextReadPosition() == 5, "a looping position reads modulo the length (25 -> 5)");

  looped->setLooping(false);
  looped->setNextReadPosition(3);
  Planar c = pullAll(*looped, 2, 4, 4);
  check(c.ch(0)[0] == 4.0f && c.ch(0)[3] == 7.0f, "setNextReadPosition(3) reads frames 3..6");

  auto narrow = memoryOf(data, false);
  Planar d = pullAll(*narrow, 3, 10, 10, 9.0f);
  bool silent = true;
  for (int i = 0; i < 10; ++i) silent = silent && d.ch(2)[i] == 0.0f;
  check(silent, "a destination channel the source does not have is cleared, not left");

  WavData wav;
  wav.numChannels = 2;
  wav.sampleRate = 48000;
  for (int i = 0; i < 5; ++i) {
    wav.samples.push_back((float) i);
    wav.samples.push_back((float) (10 * i));
  }
  MemoryAudioSource fromWav(wav);
  Planar e = pullAll(fromWav, 2, 5, 5);
  check(e.ch(0)[4] == 4.0f && e.ch(1)[4] == 40.0f && fromWav.getNumChannels() == 2,
        "a WavData is de-interleaved into its channels");

  std::vector<float> borrowed = {1.0f, 2.0f, 3.0f};
  const float* bp = borrowed.data();
  MemoryAudioSource view(&bp, 1, 3, false);
  borrowed[1] = 42.0f;
  Planar f = pullAll(view, 1, 3, 3);
  check(f.ch(0)[1] == 42.0f, "copyMemory false borrows: the caller's later write is what plays");
}

// ── AudioFormatReaderSource ──────────────────────────────────────────────────

void testReaderSource() {
  section("AudioFormatReaderSource");
  const int frames = 1000;
  std::vector<float> interleaved;
  for (int i = 0; i < frames; ++i) {
    interleaved.push_back((float) i / 1000.0f);
    interleaved.push_back(-(float) i / 1000.0f);
  }
  const char* path = sonoretest::tempPath("audio-sources-stereo.wav");
  check(writeWav(path, interleaved.data(), (size_t) frames, 2, 48000), "a stereo float WAV is written");
  auto* reader = new AudioFileReader();
  check(reader->open(path), "AudioFileReader opens it");
  AudioFormatReaderSource source(reader, true);
  source.prepareToPlay(256, 48000.0);
  Planar a(2, 1100, 7.0f);
  {
    AudioBlock<float> whole = a.block();
    for (int at = 0; at < 1100; at += 300) {
      const int n = (1100 - at) < 300 ? (1100 - at) : 300;
      source.getNextAudioBlock(AudioSourceChannelInfo(&whole, at, n)); // a reading thread: no audio scope
    }
  }
  bool exact = true;
  for (int i = 0; i < 1100; ++i) {
    const float want = i < frames ? (float) i / 1000.0f : 0.0f;
    exact = exact && a.ch(0)[i] == want && a.ch(1)[i] == -want;
  }
  check(exact, "the file's frames, in blocks larger than prepared, then silence past the end");

  source.setLooping(true);
  source.setNextReadPosition(900);
  Planar b(2, 250);
  {
    AudioBlock<float> whole = b.block();
    source.getNextAudioBlock(AudioSourceChannelInfo(whole));
  }
  exact = true;
  for (int i = 0; i < 250; ++i) exact = exact && b.ch(0)[i] == (float) ((900 + i) % frames) / 1000.0f;
  check(exact, "looping from 900 reads 900..999 then 0..149");
  checkf(source.getNextReadPosition() == 150, "and the looping position is 150 (got %lld)",
         (long long) source.getNextReadPosition());

  std::vector<float> mono;
  for (int i = 0; i < 100; ++i) mono.push_back((float) i * 0.01f);
  const char* monoPath = sonoretest::tempPath("audio-sources-mono.wav");
  writeWav(monoPath, mono.data(), mono.size(), 1, 48000);
  auto* monoReader = new AudioFileReader();
  monoReader->open(monoPath);
  AudioFormatReaderSource monoSource(monoReader, true);
  monoSource.prepareToPlay(100, 48000.0);
  Planar c(3, 100, 5.0f);
  {
    AudioBlock<float> whole = c.block();
    monoSource.getNextAudioBlock(AudioSourceChannelInfo(whole));
  }
  check(c.ch(0)[50] == mono[50] && c.ch(1)[50] == mono[50] && c.ch(2)[50] == 0.0f,
        "a mono file plays on both sides of a pair; the third channel is silent");
}

// ── open() after a failed open() ─────────────────────────────────────────────

void testReopenError() {
  section("an open() that works after one that failed");
  std::vector<float> samples(64, 0.25f);
  const char* path = sonoretest::tempPath("audio-sources-reopen.wav");
  check(writeWav(path, samples.data(), samples.size(), 1, 48000), "a mono WAV is written");
  const char* missing = sonoretest::tempPath("audio-sources-not-there.wav");
  std::remove(missing);

  // error() is about the latest open(): a reader or streamer that opened a
  // file went on saying "could not open" the one before it.
  AudioFileReader reader;
  const bool refused = !reader.open(missing) && !reader.error().empty();
  const bool opened = reader.open(path);
  checkf(refused && opened && reader.error().empty(),
         "AudioFileReader: refused a missing file, then opened one; error() is \"%s\"",
         reader.error().c_str());

  SampleStreamer streamer;
  const bool streamRefused = !streamer.open(missing) && !streamer.error().empty();
  const bool streamOpened = streamer.open(path);
  checkf(streamRefused && streamOpened && streamer.error().empty(),
         "SampleStreamer: refused a missing file, then opened one; error() is \"%s\"",
         streamer.error().c_str());
  streamer.close();
  reader.close();
  std::remove(path);
}

// ── ChannelRemappingAudioSource ──────────────────────────────────────────────

void testRemapping() {
  section("ChannelRemappingAudioSource");
  auto* probe = new ProbeSource(3, 4096);
  ChannelRemappingAudioSource remap(probe, true);
  remap.setNumberOfChannelsToProduce(3);
  remap.setInputChannelMapping(0, 2); // incoming 2 feeds the source's 0
  remap.setInputChannelMapping(1, 0); // incoming 0 feeds the source's 1
  remap.setOutputChannelMapping(0, 3); // the source's 0 and 1 both go to 3
  remap.setOutputChannelMapping(1, 3);
  remap.setOutputChannelMapping(2, 0); // the source's 2 goes to 0
  remap.prepareToPlay(512, 48000.0);

  const int n = 3000; // longer than the 1024-frame chunk: crosses chunks
  Planar io(4, n);
  for (int c = 0; c < 4; ++c)
    for (int i = 0; i < n; ++i) io.data[(size_t) c][(size_t) i] = (float) (c * 10000 + i);
  Planar incoming = io;
  {
    AudioBlock<float> whole = io.block();
    AudioScope scope;
    remap.getNextAudioBlock(AudioSourceChannelInfo(whole));
  }
  bool fed = probe->received.size() == 3;
  for (int i = 0; fed && i < n; ++i)
    fed = probe->received[0][(size_t) i] == incoming.ch(2)[i] && probe->received[1][(size_t) i] == incoming.ch(0)[i] &&
          probe->received[2][(size_t) i] == 0.0f;
  check(fed, "the source's channels receive incoming 2, incoming 0, and silence (unmapped)");
  bool routed = true;
  for (int i = 0; i < n; ++i) {
    const float out0 = incoming.ch(2)[i] + 1000.0f; // source 0
    const float out1 = incoming.ch(0)[i] + 2000.0f; // source 1
    const float out2 = 0.0f + 3000.0f;              // source 2
    routed = routed && io.ch(3)[i] == out0 + out1 && io.ch(0)[i] == out2 && io.ch(1)[i] == 0.0f && io.ch(2)[i] == 0.0f;
  }
  check(routed, "result 3 is the SUM of source 0 and 1, result 0 is source 2, 1 and 2 are silent");
  check(remap.getRemappedInputChannel(0) == 2 && remap.getRemappedInputChannel(2) == -1 &&
            remap.getRemappedOutputChannel(1) == 3 && remap.getRemappedOutputChannel(5) == -1,
        "the getters report the map, -1 for unmapped");

  remap.clearAllMappings();
  Planar quiet(2, 64, 3.0f);
  {
    AudioBlock<float> whole = quiet.block();
    AudioScope scope;
    remap.getNextAudioBlock(AudioSourceChannelInfo(whole));
  }
  bool allZero = true;
  for (int c = 0; c < 2; ++c)
    for (int i = 0; i < 64; ++i) allZero = allZero && quiet.ch(c)[i] == 0.0f;
  check(allZero, "with every mapping cleared the result is silence");
}

// ── IIRFilterAudioSource ─────────────────────────────────────────────────────

void testIirSource() {
  section("IIRFilterAudioSource vs Biquad");
  const int n = 6000;
  const auto input = noiseChannels(2, n, 777);
  auto* mem = memoryOf(input).release();
  IIRFilterAudioSource filtered(mem, true);
  Biquad lowpass;
  lowpass.setSampleRate(48000.0f);
  lowpass.lowpass(1000.0f, 0.707f);
  Biquad highpass;
  highpass.setSampleRate(48000.0f);
  highpass.highpass(3000.0f, 0.9f);
  filtered.setCoefficients(lowpass);
  filtered.prepareToPlay(100, 48000.0);

  Planar out(2, n);
  AudioBlock<float> whole = out.block();
  const int switchAt = 3000, inactiveAt = 4500;
  for (int at = 0; at < n; at += 100) {
    if (at == switchAt) filtered.setCoefficients(highpass);
    if (at == inactiveAt) filtered.makeInactive();
    AudioScope scope;
    filtered.getNextAudioBlock(AudioSourceChannelInfo(&whole, at, 100));
  }
  bool same = true;
  for (int c = 0; c < 2; ++c) {
    Biquad ref;
    ref.copyCoefficientsFrom(lowpass);
    for (int i = 0; i < n; ++i) {
      if (i == switchAt) ref.copyCoefficientsFrom(highpass); // state kept, as the source keeps it
      const float x = input[(size_t) c][(size_t) i];
      const float want = i >= inactiveAt ? x : ref.process(x);
      same = same && out.ch(c)[i] == want;
    }
  }
  check(same, "bit-exact against a Biquad run by hand: lowpass, a switch to highpass with state kept, "
              "then makeInactive passing the input through");
}

// ── ReverbAudioSource ────────────────────────────────────────────────────────

void testReverbSource() {
  section("ReverbAudioSource vs Reverb");
  const int n = 12000;
  auto input = noiseChannels(2, n, 99);
  for (int c = 0; c < 2; ++c)
    for (int i = 2048; i < n; ++i) input[(size_t) c][(size_t) i] = 0.0f; // a burst, then the tail
  Reverb::Parameters params;
  params.roomSize = 0.8f;
  params.damping = 0.3f;
  params.width = 0.9f;
  params.mix = 0.5f;

  ReverbAudioSource wet(memoryOf(input).release(), true);
  wet.setParameters(params);
  wet.prepareToPlay(256, 48000.0);
  Planar out = pullAll(wet, 2, n, 256);

  auto ref = std::unique_ptr<Reverb>(new Reverb());
  ProcessSpec spec;
  spec.sampleRate = 48000.0;
  spec.maximumBlockSize = 256;
  ref->prepare(spec);
  ref->setParameters(params);
  bool same = true;
  float tail = 0.0f;
  for (int i = 0; i < n; ++i) {
    float l = input[0][(size_t) i], r = input[1][(size_t) i];
    ref->process(l, r);
    same = same && out.ch(0)[i] == l && out.ch(1)[i] == r;
    if (i > 4000) tail = (std::max)(tail, std::fabs(out.ch(0)[i]));
  }
  check(same, "stereo: bit-exact against Reverb::process on the same frames");
  checkf(tail > 1e-4f, "and it is a reverb: the tail after the burst is audible (%.4f)", tail);

  std::vector<std::vector<float>> mono(1, input[0]);
  ReverbAudioSource monoWet(memoryOf(mono).release(), true);
  monoWet.setParameters(params);
  monoWet.prepareToPlay(256, 48000.0);
  Planar monoOut = pullAll(monoWet, 1, n, 256);
  auto monoRef = std::unique_ptr<Reverb>(new Reverb());
  monoRef->prepare(spec);
  monoRef->setParameters(params);
  same = true;
  for (int i = 0; i < n; ++i) {
    float l = mono[0][(size_t) i], zero = 0.0f;
    monoRef->process(l, zero);
    same = same && monoOut.ch(0)[i] == l;
  }
  check(same, "mono: the left side fed alone, left kept -- bit-exact");

  ReverbAudioSource bypassed(memoryOf(input).release(), true);
  bypassed.setParameters(params);
  bypassed.prepareToPlay(256, 48000.0);
  bypassed.setBypassed(true);
  Planar dry = pullAll(bypassed, 2, 1024, 256);
  same = true;
  for (int i = 0; i < 1024; ++i) same = same && dry.ch(0)[i] == input[0][(size_t) i];
  check(same && bypassed.isBypassed(), "bypassed: the input untouched");
}

// ── MixerAudioSource ─────────────────────────────────────────────────────────

void testMixer() {
  section("MixerAudioSource");
  std::atomic<int> destroyed{0};
  MixerAudioSource mixer;
  auto* one = new CountingSource(1.0f, &destroyed);
  auto* two = new CountingSource(2.0f, &destroyed);
  auto* four = new CountingSource(4.0f, &destroyed);
  mixer.prepareToPlay(512, 48000.0);
  mixer.addInputSource(one, true);
  mixer.addInputSource(two, true);
  mixer.addInputSource(four, true);
  mixer.addInputSource(two, true); // already there: ignored
  check(mixer.getNumInputs() == 3, "three inputs; adding one twice is ignored");
  check(one->prepared.load() == 1, "an input added to a prepared mixer is prepared at once");

  Planar sum = pullAll(mixer, 2, 3000, 3000, 55.0f); // one block, longer than the scratch
  bool seven = true;
  for (int c = 0; c < 2; ++c)
    for (int i = 0; i < 3000; ++i) seven = seven && sum.ch(c)[i] == 7.0f;
  check(seven, "1 + 2 + 4 = 7 on every sample of a 3000-frame block (scratch is 1024)");

  mixer.removeInputSource(two);
  checkf(destroyed.load() == 1, "removing an owned input deletes it (deleted %d)", destroyed.load());
  Planar five = pullAll(mixer, 2, 64, 64);
  check(five.ch(1)[10] == 5.0f, "and the sum is 5");

  CountingSource borrowed(8.0f);
  mixer.addInputSource(&borrowed, false);
  mixer.removeInputSource(&borrowed);
  check(borrowed.released.load() == 1, "a removed input is released (and not deleted, being borrowed)");
  mixer.removeAllInputs();
  Planar none = pullAll(mixer, 2, 64, 64, 3.0f);
  check(none.ch(0)[0] == 0.0f && none.ch(1)[63] == 0.0f && destroyed.load() == 3,
        "no inputs: silence over whatever the buffer held; every owned input deleted");

  // ── stress: inputs added and removed while blocks are pulled ──
  MixerAudioSource busy;
  busy.prepareToPlay(256, 48000.0);
  std::vector<std::unique_ptr<CountingSource>> pool;
  for (int k = 0; k < 5; ++k) pool.emplace_back(new CountingSource((float) (1 << k)));
  std::atomic<bool> done{false};
  std::atomic<long> blocks{0}, torn{0};
  std::thread audio([&] {
    Planar buf(2, 256);
    AudioBlock<float> whole = buf.block();
    AudioScope scope;
    while (!done.load()) {
      busy.getNextAudioBlock(AudioSourceChannelInfo(whole));
      const float v = buf.ch(0)[0];
      bool ok = v >= 0.0f && v <= 31.0f && v == std::floor(v);
      for (int c = 0; c < 2 && ok; ++c)
        for (int i = 0; i < 256 && ok; ++i) ok = buf.ch(c)[i] == v;
      if (!ok) torn.fetch_add(1);
      blocks.fetch_add(1);
    }
  });
  uint32_t state = 4242;
  long changes = 0;
  const auto until = Clock::now() + std::chrono::milliseconds(700);
  while (Clock::now() < until) {
    state = state * 1664525u + 1013904223u;
    CountingSource* s = pool[(state >> 8) % pool.size()].get();
    if ((state >> 20) & 1) busy.addInputSource(s, false);
    else busy.removeInputSource(s);
    ++changes;
  }
  done.store(true);
  audio.join();
  busy.removeAllInputs();
  checkf(torn.load() == 0 && blocks.load() > 100,
         "%ld changes against %ld blocks: every block is a whole subset sum (%ld torn)", changes,
         blocks.load(), torn.load());

  // ── a one-shot that removes itself from its own callback, while another
  // thread adds the next ones. The adding thread used to wait out the block
  // holding the writers' lock, and the block was waiting for that lock:
  // both threads stopped for good on the first add. (Not inside the
  // allocation audit: removing from a callback copies the list, which is the
  // caller's trade, as in the reference.)
  struct OneShot : AudioSource {
    MixerAudioSource& owner;
    int blocksLeft;
    std::atomic<int>& deleted;
    OneShot(MixerAudioSource& m, int n, std::atomic<int>& d) : owner(m), blocksLeft(n), deleted(d) {}
    ~OneShot() override { deleted.fetch_add(1); }
    void prepareToPlay(int, double) override {}
    void releaseResources() override {}
    void getNextAudioBlock(const AudioSourceChannelInfo& info) override {
      for (int c = 0; c < info.numChannels(); ++c) std::memset(info.channel(c), 0, (size_t) info.numSamples * sizeof(float));
      if (--blocksLeft == 0) owner.removeInputSource(this);
    }
  };
  {
    std::atomic<int> deleted{0};
    std::atomic<long> shotBlocks{0}, added{0};
    std::atomic<bool> finished{false};
    auto shots = std::unique_ptr<MixerAudioSource>(new MixerAudioSource());
    shots->prepareToPlay(64, 48000.0);
    std::thread player([&] {
      Planar buf(2, 64);
      AudioBlock<float> whole = buf.block();
      while (!finished.load()) {
        shots->getNextAudioBlock(AudioSourceChannelInfo(whole));
        shotBlocks.fetch_add(1);
      }
    });
    std::thread ui([&] {
      for (int k = 0; !finished.load(); ++k) {
        shots->addInputSource(new OneShot(*shots, 1 + k % 3, deleted), true);
        added.fetch_add(1);
      }
    });
    // Stuck is a full second with neither thread moving. One 100 ms tick
    // without progress was enough before, and a machine busy compiling
    // starved both threads that long once in a full verify: a deadlock
    // never moves again, so the longer wait only costs a real one time.
    bool stuck = false;
    long lastBlocks = -1, lastAdded = -1;
    int still = 0;
    for (int tick = 0; tick < 30 && !stuck; ++tick) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      still = (shotBlocks.load() == lastBlocks && added.load() == lastAdded) ? still + 1 : 0;
      stuck = still >= 10;
      lastBlocks = shotBlocks.load();
      lastAdded = added.load();
      if (tick >= 7 && still == 0) break; // moving after as long as the old test ran
    }
    if (stuck) {
      // Joining would hang the test run; say what happened and stop.
      check(false, "a one-shot removing itself while another thread adds: the two threads deadlocked");
      std::printf("AUDIO SOURCES TESTS FAILED\n");
      std::fflush(stdout);
      std::_Exit(1);
    }
    finished.store(true);
    player.join();
    ui.join();
    shots.reset();
    checkf(added.load() > 50 && deleted.load() == (int) added.load(),
           "one-shots removing themselves from their own callback while %ld were added: no deadlock, "
           "every one deleted (%d)",
           added.load(), deleted.load());
  }
}

// ── ToneGeneratorAudioSource ─────────────────────────────────────────────────

void testTone() {
  section("ToneGeneratorAudioSource");
  ToneGeneratorAudioSource tone;
  tone.prepareToPlay(480, 48000.0);
  Planar out = pullAll(tone, 2, 48000, 480);
  const double f = measureFrequency(out.ch(0), 0, 48000, 48000.0);
  checkf(std::fabs(f - 1000.0) < 0.01, "the default is 1000 Hz (measured %.5f)", f);
  float peak = 0.0f;
  for (int i = 0; i < 48000; ++i) peak = (std::max)(peak, std::fabs(out.ch(0)[i]));
  const double level = rms(out.ch(0), 0, 48000);
  checkf(std::fabs(peak - 0.5f) < 1e-4f && std::fabs(level - 0.5 / std::sqrt(2.0)) < 1e-4,
         "at amplitude 0.5: peak %.5f, RMS %.5f (0.5/sqrt 2 = 0.35355)", peak, level);
  bool same = true;
  for (int i = 0; i < 48000; ++i) same = same && out.ch(0)[i] == out.ch(1)[i];
  check(same, "every channel carries the same sample");

  ToneGeneratorAudioSource other;
  other.setFrequency(441.0);
  other.setAmplitude(0.25f);
  other.prepareToPlay(441, 44100.0);
  Planar b = pullAll(other, 1, 4410, 441);
  double worst = 0.0;
  for (int i = 0; i < 4410; ++i)
    worst = (std::max)(worst, std::fabs((double) b.ch(0)[i] - 0.25 * std::sin(2.0 * 3.14159265358979323846 * 441.0 * i / 44100.0)));
  checkf(worst < 2e-6, "441 Hz at 0.25 matches 0.25 sin(2 pi 441 n / 44100) to %.2g", worst);

  other.setAmplitude(0.5f);
  Planar c = pullAll(other, 1, 441, 441);
  double slope = 0.0;
  for (int i = 1; i < 441; ++i) slope = (std::max)(slope, std::fabs((double) c.ch(0)[i] - c.ch(0)[i - 1]));
  checkf(slope < 0.5 * 2.0 * 3.14159265358979323846 * 441.0 / 44100.0 * 1.05,
         "a level change glides across the block: no step beyond the sine's own slope (%.4f)", slope);
}

// ── ResamplingAudioSource ────────────────────────────────────────────────────

void testResampling() {
  section("ResamplingAudioSource");
  {
    const auto input = noiseChannels(2, 5000, 31);
    ResamplingAudioSource same(memoryOf(input).release(), true, 2);
    same.prepareToPlay(512, 48000.0);
    Planar out = pullAll(same, 2, 5000, 333);
    bool exact = true;
    for (int c = 0; c < 2; ++c)
      for (int i = 0; i < 5000; ++i) exact = exact && out.ch(c)[i] == input[(size_t) c][(size_t) i];
    check(exact, "ratio 1 is bit-exact: the first output is the first input, no latency");
  }
  {
    // Length: N samples of DC at ratio r come out as N / r samples' worth.
    const int N = 10000;
    std::vector<std::vector<float>> dc(1, std::vector<float>((size_t) N, 1.0f));
    for (double r : {44100.0 / 48000.0, 2.0, 0.5}) {
      ResamplingAudioSource rs(memoryOf(dc).release(), true, 1);
      rs.setResamplingRatio(r);
      rs.prepareToPlay(512, 48000.0);
      const int total = (int) (N / r) + 4000;
      Planar out = pullAll(rs, 1, total, 512);
      double sum = 0.0;
      for (int i = 0; i < total; ++i) sum += out.ch(0)[i];
      checkf(std::fabs(sum - N / r) < 3.0, "ratio %.5f: %d samples of DC become %.2f samples' worth (N / r = %.2f)",
             r, N, sum, N / r);
    }
  }
  {
    // 44.1 kHz material into a 48 kHz stream: still 1 kHz, and it agrees with
    // resample.h's whole-buffer conversion of the same samples.
    const int N = 44100;
    std::vector<std::vector<float>> sine(1, std::vector<float>((size_t) N));
    for (int i = 0; i < N; ++i) sine[0][(size_t) i] = 0.5f * (float) std::sin(2.0 * 3.14159265358979323846 * 1000.0 * i / 44100.0);
    ResamplingAudioSource rs(memoryOf(sine).release(), true, 1);
    rs.setResamplingRatio(44100.0 / 48000.0);
    rs.prepareToPlay(480, 48000.0);
    const int outN = 48000;
    Planar out = pullAll(rs, 1, outN, 480);
    const double f = measureFrequency(out.ch(0), 1000, outN - 1000, 48000.0);
    checkf(std::fabs(f - 1000.0) < 0.05, "a 1 kHz tone at 44.1 kHz plays at 1 kHz in the 48 kHz stream (%.4f)", f);

    std::vector<float> offline(Resampler::outputLength((size_t) N, 48000.0 / 44100.0));
    const size_t got = Resampler::resample(sine[0].data(), (size_t) N, offline.data(), offline.size(), 48000.0 / 44100.0);
    double worst = 0.0;
    for (int i = 200; i < (int) got - 200 && i < outN; ++i)
      worst = (std::max)(worst, std::fabs((double) out.ch(0)[i] - offline[(size_t) i]));
    checkf(worst < 1e-3 && got == 48000, "and agrees with resample.h's Resampler sample for sample: worst %.2e "
                                         "(%.1f dB below the tone)", worst, 20.0 * std::log10(worst / 0.5));
  }
  {
    // Twice as fast: 30 kHz at a 96 kHz input would fold to 18 kHz in a
    // 48 kHz stream. It must arrive attenuated; 5 kHz must arrive intact.
    for (double hz : {5000.0, 30000.0}) {
      const int N = 96000;
      std::vector<std::vector<float>> sine(1, std::vector<float>((size_t) N));
      for (int i = 0; i < N; ++i) sine[0][(size_t) i] = 0.5f * (float) std::sin(2.0 * 3.14159265358979323846 * hz * i / 96000.0);
      ResamplingAudioSource rs(memoryOf(sine).release(), true, 1);
      rs.setResamplingRatio(2.0);
      rs.prepareToPlay(512, 48000.0);
      Planar out = pullAll(rs, 1, 40000, 512);
      const double gainDb = 20.0 * std::log10(rms(out.ch(0), 4000, 40000) / (0.5 / std::sqrt(2.0)));
      if (hz < 10000.0)
        checkf(std::fabs(gainDb) < 0.1, "ratio 2: 5 kHz passes at %.3f dB", gainDb);
      else
        checkf(gainDb < -20.0, "ratio 2: 30 kHz, which would alias to 18 kHz, arrives at %.1f dB", gainDb);
    }
  }
  {
    // A ratio change glides: the output never steps, and lands on the new pitch.
    const int N = 48000 * 3;
    std::vector<std::vector<float>> sine(1, std::vector<float>((size_t) N));
    for (int i = 0; i < N; ++i) sine[0][(size_t) i] = 0.5f * (float) std::sin(2.0 * 3.14159265358979323846 * 440.0 * i / 48000.0);
    ResamplingAudioSource rs(memoryOf(sine).release(), true, 1);
    rs.prepareToPlay(256, 48000.0);
    Planar out(1, 48000);
    AudioBlock<float> whole = out.block();
    for (int at = 0; at < 48000; at += 256) {
      if (at == 12032) rs.setResamplingRatio(1.5);
      AudioScope scope;
      rs.getNextAudioBlock(AudioSourceChannelInfo(&whole, at, (48000 - at) < 256 ? 48000 - at : 256));
    }
    double step = 0.0;
    for (int i = 1; i < 48000; ++i) step = (std::max)(step, std::fabs((double) out.ch(0)[i] - out.ch(0)[i - 1]));
    const double bound = 0.5 * 2.0 * 3.14159265358979323846 * 660.0 / 48000.0 * 1.02;
    checkf(step < bound, "a ratio change from 1 to 1.5 never steps the output (largest step %.4f, "
                         "the 660 Hz sine's own slope %.4f)", step, bound);
    const double before = measureFrequency(out.ch(0), 1000, 12000, 48000.0);
    const double after = measureFrequency(out.ch(0), 20000, 47000, 48000.0);
    checkf(std::fabs(before - 440.0) < 0.05 && std::fabs(after - 660.0) < 0.05,
           "440 Hz before, 660 Hz after (%.3f, %.3f)", before, after);
  }
  {
    // A step of more than 16 input frames per output -- a ratio raised far
    // past the one prepared for, or a block longer than promised at a high
    // one -- jumps the read point past everything the buffer holds once it
    // has been refilled inside a block. That was a negative-size memmove
    // (ASan's report). The frames jumped over must still be taken from the
    // input, every one, or the stream slips against its own position.
    struct CountingDc : AudioSource {
      long long pulled = 0;
      void prepareToPlay(int, double) override {}
      void releaseResources() override {}
      void getNextAudioBlock(const AudioSourceChannelInfo& info) override {
        for (int c = 0; c < info.numChannels(); ++c)
          for (int i = 0; i < info.numSamples; ++i) info.channel(c)[i] = 1.0f;
        pulled += info.numSamples;
      }
    };
    struct Case {
      double preparedRatio, ratio;
      int preparedBlock, block;
    };
    const Case cases[] = {{1.0, 32.0, 512, 512}, {1.0, 256.0, 512, 512}, {32.0, 32.0, 512, 2048}, {20.0, 20.0, 64, 4096}};
    for (const Case& k : cases) {
      CountingDc dc;
      ResamplingAudioSource rs(&dc, false, 2);
      rs.setResamplingRatio(k.preparedRatio);
      rs.prepareToPlay(k.preparedBlock, 48000.0);
      rs.setResamplingRatio(k.ratio);
      Planar out(2, k.block);
      AudioBlock<float> whole = out.block();
      long long worstSlip = 0;
      double worstDc = 0.0;
      for (int n = 0; n < 6; ++n) {
        const long long before = dc.pulled;
        {
          AudioScope scope;
          rs.getNextAudioBlock(AudioSourceChannelInfo(whole));
        }
        if (n < 2) continue; // the first block glides to the new ratio
        const long long slip = std::llabs(dc.pulled - before - (long long) (k.ratio * k.block));
        worstSlip = (std::max)(worstSlip, slip);
        for (int c = 0; c < 2; ++c)
          for (int i = 0; i < k.block; ++i) worstDc = (std::max)(worstDc, (double) std::fabs(out.ch(c)[i] - 1.0f));
      }
      checkf(worstSlip <= 16 && worstDc < 1e-4,
             "prepared at ratio %g for %d frames, run at %g in blocks of %d: ratio x block input frames per block "
             "(off by %lld), DC through at %.2g",
             k.preparedRatio, k.preparedBlock, k.ratio, k.block, worstSlip, worstDc);
    }
  }
}

// ── BufferingAudioSource ─────────────────────────────────────────────────────

/** Spin, then give the core away: hot when both threads have one each, and
 *  still finishing on a machine that has fewer. */
template <typename Ready>
void spinUntil(Ready ready) {
  for (int spins = 0; !ready(); ++spins)
    if (spins > 2000) std::this_thread::yield();
}

void testBuffering() {
  section("BufferingAudioSource");
  {
    // The ring's range read the way a block reads it -- four tries, then
    // silence -- while a writer publishes as fast as it can. With one copy
    // the reader found the writer mid-write on every try for as long as the
    // writer was stopped there; under load, a reader thread preempted
    // mid-publish cost a whole block of silence with the audio in the ring.
    // With two, the published copy is never the one being written.
    struct Range {
      int64_t start, end;
    };
    sourcedetail::SeqBox<Range> box{Range{0, 1000}};
    std::atomic<bool> stop{false};
    std::thread writer([&] {
      for (int64_t i = 1; !stop.load(std::memory_order_relaxed); ++i) box.write(Range{i, i + 1000});
    });
    long empty = 0, torn = 0;
    const long reads = 1000000;
    for (long k = 0; k < reads; ++k) {
      Range r{0, 1000};
      bool have = false;
      for (int tries = 0; tries < 4 && !have; ++tries) have = box.tryRead(r);
      if (!have) ++empty;
      else if (r.end != r.start + 1000) ++torn;
    }
    stop.store(true);
    writer.join();
    // Under one in a hundred: the one-copy box lost six in a hundred here,
    // and the two-copy one loses a read only when this thread is stopped
    // inside one while the writer laps it twice -- a few in a million
    // alone, some hundreds beside a full build.
    checkf(empty <= reads / 100 && torn == 0,
           "a block reads the range whole while it is published as fast as possible "
           "(%ld of %ld reads found nothing in four tries, %ld torn)",
           empty, reads, torn);
  }
  {
    // The ring's hand-over on its own, as readNextChunk() makes it: publish a
    // range through a SeqBox -- whose last store is a RELEASE -- then read
    // the BlockEpoch count; meanwhile another thread enters a block and reads
    // the range. Forbidden: the count read from BEFORE that block entered
    // while the block read the OLD range, after which the reader would
    // overwrite slots the block is copying. x86 reorders a release store with
    // a later load; before blockInFlight() fenced, this found a few hundred
    // of 500000.
    struct Range {
      int64_t start, end;
    };
    sourcedetail::SeqBox<Range> box{Range{0, 0}};
    sourcedetail::BlockEpoch epoch;
    std::atomic<long> go{-1}, done{-1};
    int64_t blockRead = 0;
    bool consistent = false;
    const long rounds = 500000;
    std::thread block([&] {
      for (long i = 0; i < rounds; ++i) {
        spinUntil([&] { return go.load(std::memory_order_acquire) == i; });
        epoch.enter();
        Range r{0, 0};
        consistent = box.tryRead(r);
        blockRead = r.start;
        epoch.leave();
        done.store(i, std::memory_order_release);
      }
    });
    long violations = 0;
    for (long i = 0; i < rounds; ++i) {
      go.store(i, std::memory_order_release);
      box.write(Range{i + 1, i + 1});
      const uint64_t seen = epoch.blockInFlight();
      spinUntil([&] { return done.load(std::memory_order_acquire) == i; });
      if (consistent && seen == 2u * (uint64_t) i && blockRead != i + 1) ++violations;
    }
    block.join();
    checkf(violations == 0,
           "publish, then read the block count: no block ever reads the old range after the count said "
           "none had started (%ld of %ld did)",
           violations, rounds);
  }
  const int N = 200000;
  const auto data = noiseChannels(2, N, 2024);
  ReadAheadThread thread;

  {
    auto* slow = new SlowSource(memoryOf(data));
    slow->delayMicros.store(3000); // every read sleeps 1.5..3 ms
    BufferingAudioSource buffered(slow, thread, true, 32768, 2, true);
    buffered.prepareToPlay(512, 48000.0);
    Planar out(2, 100000);
    AudioBlock<float> whole = out.block();
    double worst = 0.0;
    bool allReady = true;
    for (int at = 0; at < 100000; at += 512) {
      const int n = (100000 - at) < 512 ? 100000 - at : 512;
      const AudioSourceChannelInfo info(&whole, at, n);
      allReady = buffered.waitForNextAudioBlockReady(info, 5000) && allReady;
      const auto t = Clock::now();
      {
        AudioScope scope;
        buffered.getNextAudioBlock(info);
      }
      worst = (std::max)(worst, microsSince(t));
    }
    bool same = true;
    for (int c = 0; c < 2; ++c)
      for (int i = 0; i < 100000; ++i) same = same && out.ch(c)[i] == data[(size_t) c][(size_t) i];
    check(allReady, "waitForNextAudioBlockReady says every block is ready in time");
    checkf(same && buffered.getUnderrunFrames() == 0,
           "behind a reader that sleeps on every read: 100000 frames identical to the unbuffered "
           "samples, 0 underruns (%llu)", (unsigned long long) buffered.getUnderrunFrames());
    check(!slow->readOnTestThread.load() && slow->reads.load() > 0,
          "and the source was never read on the audio thread -- only on the read-ahead thread");
    checkf(worst < 5000.0, "slowest callback while waiting for each block to be ready: %.1f us", worst);

    buffered.setNextReadPosition(150000);
    Planar seek(2, 512);
    AudioBlock<float> sw = seek.block();
    const AudioSourceChannelInfo si(sw);
    const bool ready = buffered.waitForNextAudioBlockReady(si, 5000);
    {
      AudioScope scope;
      buffered.getNextAudioBlock(si);
    }
    same = ready;
    for (int i = 0; i < 512; ++i) same = same && seek.ch(0)[i] == data[0][(size_t) (150000 + i)];
    check(same, "after a forward seek to 150000, the frames from there");
    buffered.setNextReadPosition(1000);
    buffered.waitForNextAudioBlockReady(si, 5000);
    {
      AudioScope scope;
      buffered.getNextAudioBlock(si);
    }
    same = true;
    for (int i = 0; i < 512; ++i) same = same && seek.ch(1)[i] == data[1][(size_t) (1000 + i)];
    check(same, "after a backward seek to 1000, the frames from there");
  }

  {
    // Up to four times real time, with a reader that is slow but on average
    // fast enough: no waiting by the callback, and still nothing missing.
    // Paced RELATIVELY: an absolute schedule turns any stall of this thread
    // on a busy machine into a burst of back-to-back blocks, which measures
    // the machine rather than the ring.
    auto* slow = new SlowSource(memoryOf(data));
    slow->delayMicros.store(4000);
    BufferingAudioSource buffered(slow, thread, true, 48000, 2, true);
    buffered.prepareToPlay(512, 48000.0);
    Planar out(2, 96000);
    AudioBlock<float> whole = out.block();
    double worst = 0.0;
    for (int at = 0; at < 96000; at += 512) {
      std::this_thread::sleep_for(std::chrono::microseconds(512 * 1000000 / 48000 / 4));
      const int n = (96000 - at) < 512 ? 96000 - at : 512;
      const auto t = Clock::now();
      {
        AudioScope scope;
        buffered.getNextAudioBlock(AudioSourceChannelInfo(&whole, at, n));
      }
      worst = (std::max)(worst, microsSince(t));
    }
    // What must hold however loaded the machine: every frame is the source's,
    // or silence where an underrun was counted -- never a stale or torn one --
    // and the callback never read the source itself. What a loaded machine
    // may cost: a few underrun blocks (eight copies of this test at once
    // missed one of 188), and a callback descheduled for a while.
    bool sound = true;
    uint64_t silent = 0;
    for (int c = 0; c < 2; ++c)
      for (int i = 0; i < 96000; ++i) {
        const float v = out.ch(c)[i];
        if (v == data[(size_t) c][(size_t) i]) continue;
        if (v == 0.0f) ++silent;
        else sound = false;
      }
    const uint64_t under = buffered.getUnderrunFrames();
    checkf(sound && silent <= 2 * under && !slow->readOnTestThread.load(),
           "free-running at up to 4x real time: every frame the source's, or silence where an underrun "
           "was counted (%llu frames), none read on the audio thread",
           (unsigned long long) under);
    checkf(under <= 2048 && worst < 100000.0,
           "and the ring kept up: %llu underrun frames (a busy machine may cost a few blocks), "
           "slowest callback %.1f us", (unsigned long long) under, worst);
  }

  {
    // Starved, then fed. For the first hundred blocks every read sleeps
    // 100-200 ms, far longer than the blocks it would fill; then the reader
    // recovers. The callback must not care in either phase, and what it
    // delivers once the reader recovers must be the right samples.
    auto* slow = new SlowSource(memoryOf(data));
    BufferingAudioSource buffered(slow, thread, true, 8192, 2, false);
    buffered.prepareToPlay(512, 48000.0);
    slow->delayMicros.store(200000);
    Planar out(2, 512 * 300);
    AudioBlock<float> whole = out.block();
    double worst = 0.0;
    uint64_t starvedUnderruns = 0;
    for (int at = 0, k = 0; at < 512 * 300; at += 512, ++k) {
      if (k == 100) {
        starvedUnderruns = buffered.getUnderrunFrames();
        slow->delayMicros.store(0);
      }
      std::this_thread::sleep_for(std::chrono::microseconds(2600));
      const auto t = Clock::now();
      {
        AudioScope scope;
        buffered.getNextAudioBlock(AudioSourceChannelInfo(&whole, at, 512));
      }
      worst = (std::max)(worst, microsSince(t));
    }
    bool honest = true;
    long delivered = 0;
    for (int i = 0; i < 512 * 300; ++i) {
      const bool zero = out.ch(0)[i] == 0.0f && out.ch(1)[i] == 0.0f;
      const bool right = out.ch(0)[i] == data[0][(size_t) i] && out.ch(1)[i] == data[1][(size_t) i];
      honest = honest && (zero || right);
      if (right) ++delivered;
    }
    checkf(starvedUnderruns > 0, "a starved reader shows as underruns (%llu frames), not as waiting",
           (unsigned long long) starvedUnderruns);
    checkf(honest && delivered > 0,
           "every frame is either the right sample or silence -- never a stale slot (%ld of %d delivered "
           "once the reader recovered)",
           delivered, 512 * 300);
    checkf(worst < 5000.0,
           "the slowest callback took %.1f us while the reader slept 100-200 ms per read: it never waited",
           worst);
  }

  {
    std::vector<std::vector<float>> shortLoop(1, std::vector<float>(1000));
    for (int i = 0; i < 1000; ++i) shortLoop[0][(size_t) i] = (float) (i + 1);
    BufferingAudioSource buffered(memoryOf(shortLoop, true).release(), thread, true, 4096, 1, true);
    buffered.prepareToPlay(256, 48000.0);
    Planar out(1, 5000);
    AudioBlock<float> whole = out.block();
    for (int at = 0; at < 5000; at += 256) {
      const int n = (5000 - at) < 256 ? 5000 - at : 256;
      const AudioSourceChannelInfo info(&whole, at, n);
      buffered.waitForNextAudioBlockReady(info, 5000);
      AudioScope scope;
      buffered.getNextAudioBlock(info);
    }
    bool same = true;
    for (int i = 0; i < 5000; ++i) same = same && out.ch(0)[i] == (float) (i % 1000 + 1);
    checkf(same && buffered.getNextReadPosition() == 0,
           "a looping source loops through the ring; the position reads modulo the length (%lld)",
           (long long) buffered.getNextReadPosition());
  }
}

// ── AudioTransportSource ─────────────────────────────────────────────────────

void testTransport() {
  section("AudioTransportSource");
  const int N = 48000;
  std::vector<std::vector<float>> ramp(2, std::vector<float>((size_t) N));
  std::vector<std::vector<float>> dc(2, std::vector<float>((size_t) N, 1.0f));
  for (int i = 0; i < N; ++i) ramp[0][(size_t) i] = ramp[1][(size_t) i] = (float) (i + 1) / (float) N;

  auto dcSource = memoryOf(dc);
  AudioTransportSource transport;
  transport.setSource(dcSource.get());
  transport.prepareToPlay(256, 48000.0);
  checkf(std::fabs(transport.getLengthInSeconds() - 1.0) < 1e-12, "length 48000 frames at 48 kHz is 1.0 s");

  Planar idle = pullAll(transport, 2, 256, 256, 4.0f);
  check(idle.ch(0)[0] == 0.0f && idle.ch(1)[255] == 0.0f && transport.getNextReadPosition() == 0,
        "before start(): silence, and the source is not pulled");

  transport.start();
  check(transport.isPlaying(), "start() plays");
  Planar in = pullAll(transport, 2, 512, 256);
  bool rampUp = in.ch(0)[0] == 0.0f;
  for (int i = 0; i < 256; ++i) rampUp = rampUp && std::fabs(in.ch(0)[i] - (float) i / 256.0f) < 1e-6f;
  bool full = true;
  for (int i = 256; i < 512; ++i) full = full && in.ch(0)[i] == 1.0f && in.ch(1)[i] == 1.0f;
  check(rampUp, "start fades in: sample i is i/256 over the first 256");
  check(full, "then unity, exactly");
  checkf(std::fabs(transport.getCurrentPosition() - 512.0 / 48000.0) < 1e-12, "position %.6f s after 512 frames",
         transport.getCurrentPosition());

  transport.setGain(0.5f);
  Planar g = pullAll(transport, 2, 512, 256);
  bool gainRamp = true;
  for (int i = 0; i < 256; ++i) gainRamp = gainRamp && std::fabs(g.ch(0)[i] - (1.0f - 0.5f * (float) i / 256.0f)) < 1e-6f;
  for (int i = 256; i < 512; ++i) gainRamp = gainRamp && g.ch(1)[i] == 0.5f;
  check(gainRamp, "setGain(0.5) ramps across the next block, then holds");
  transport.setGain(1.0f);
  pullAll(transport, 2, 256, 256);

  // Stopping with blocks shorter than the fade: the fade crosses blocks.
  transport.stop();
  check(!transport.isPlaying(), "stop() returns at once and isPlaying() says so");
  const double stoppedAt = transport.getCurrentPosition();
  Planar out = pullAll(transport, 2, 512, 64);
  bool rampDown = true;
  for (int i = 0; i < 256; ++i) rampDown = rampDown && std::fabs(out.ch(0)[i] - (1.0f - (float) i / 256.0f)) < 1e-5f;
  bool after = true;
  for (int i = 256; i < 512; ++i) after = after && out.ch(0)[i] == 0.0f;
  check(rampDown, "stop fades out over 256 samples, across four 64-sample blocks");
  check(after, "then silence");
  checkf(std::fabs(transport.getCurrentPosition() - (stoppedAt + 256.0 / 48000.0)) < 1e-12,
         "and the position stops where the fade ended (+256 frames)");

  // Position, seconds and frames, with a ramp so the frame read is visible.
  auto rampSource = memoryOf(ramp);
  transport.setSource(rampSource.get());
  transport.setPosition(0.5);
  checkf(transport.getNextReadPosition() == 24000 && std::fabs(transport.getCurrentPosition() - 0.5) < 1e-12,
         "setPosition(0.5) while stopped: position 24000 frames, 0.5 s");
  transport.start();
  Planar from = pullAll(transport, 2, 512, 256);
  check(from.ch(0)[300] == ramp[0][24300], "plays from frame 24000 (frame 300 of output is source frame 24300)");

  // A seek while playing: out at the old place, in at the new.
  transport.setNextReadPosition(10000);
  checkf(transport.getNextReadPosition() == 10000, "a seek while playing reports the new position at once");
  Planar jump = pullAll(transport, 2, 1024, 512);
  bool declicked = true;
  for (int i = 1; i < 1024; ++i) declicked = declicked && std::fabs(jump.ch(0)[i] - jump.ch(0)[i - 1]) < 1.0f / 100.0f;
  const float at127 = jump.ch(0)[127];
  check(declicked, "and it is declicked: no step larger than a fade step anywhere around the jump");
  checkf(std::fabs(at127 - ramp[0][24512 + 127] / 128.0f) < 1e-5f && jump.ch(0)[128] == 0.0f,
         "the old place fades out over 128 samples, the new one starts from zero");
  check(jump.ch(0)[900] == ramp[0][(size_t) (10000 + 900 - 128)], "and the new place is frame 10000 onward");
  transport.stop();
  pullAll(transport, 2, 512, 256);

  // Playing off the end.
  std::vector<std::vector<float>> shortData(2, std::vector<float>(1000, 0.25f));
  auto shortSource = memoryOf(shortData);
  transport.setSource(shortSource.get());
  transport.dispatchPendingChanges();
  transport.start();
  transport.dispatchPendingChanges();
  pullAll(transport, 2, 2048, 256);
  check(transport.hasStreamFinished() && !transport.isPlaying(), "past the end: finished, and no longer playing");
  check(transport.dispatchPendingChanges(), "and a change message is waiting for the UI tick");
  // The sources above were declared after the transport and die before it;
  // a transport releases its source when it lets go of it, so it lets go now.
  transport.setSource(nullptr);

  // Loading the next file the reference's way: setSource(nullptr), then
  // setSource(next) and start(), with no block in between. The new chain is
  // made just after the old one is freed, and where the allocator hands that
  // block back the two share an address. Told apart by address, the new one
  // kept the old one's fade and started at full level -- a click.
  {
    auto first = memoryOf(dc), second = memoryOf(dc);
    transport.setSource(first.get());
    transport.start();
    pullAll(transport, 2, 1024, 256); // playing, faded all the way in
    {
      LifoHeap lifo;
      transport.setSource(nullptr);
      transport.setSource(second.get());
    }
    transport.start();
    Planar next = pullAll(transport, 2, 256, 256);
    checkf(next.ch(0)[0] == 0.0f && std::fabs(next.ch(0)[128] - 0.5f) < 1e-6f,
           "the next file, loaded with no block in between: fades in from %g, as start() promises",
           (double) next.ch(0)[0]);
    transport.setSource(nullptr);
  }

  // Sample-rate correction: 44.1 kHz material in a 48 kHz transport.
  std::vector<std::vector<float>> sine(1, std::vector<float>(44100));
  for (int i = 0; i < 44100; ++i) sine[0][(size_t) i] = 0.5f * (float) std::sin(2.0 * 3.14159265358979323846 * 1000.0 * i / 44100.0);
  auto sineSource = memoryOf(sine);
  AudioTransportSource corrected;
  corrected.setSource(sineSource.get(), 0, nullptr, 44100.0, 1);
  corrected.prepareToPlay(480, 48000.0);
  checkf(corrected.getTotalLength() == 48000 && std::fabs(corrected.getLengthInSeconds() - 1.0) < 1e-12,
         "44100 frames at 44.1 kHz are 48000 frames, 1.0 s, at 48 kHz (resample.h's count)");
  corrected.start();
  Planar played = pullAll(corrected, 1, 24000, 480);
  const double f = measureFrequency(played.ch(0), 2000, 24000, 48000.0);
  checkf(std::fabs(f - 1000.0) < 0.05, "and the 1 kHz tone is 1 kHz in the output (%.4f)", f);
  checkf(std::fabs(corrected.getCurrentPosition() - 0.5) < 1e-12, "position 0.5 s after 24000 output frames");

  // Read-ahead in the chain: a slow source, buffered, plays the same samples.
  ReadAheadThread thread;
  SlowSource slow(memoryOf(ramp));
  slow.delayMicros.store(2000);
  AudioTransportSource buffered;
  buffered.setSource(&slow, 16384, &thread, 0.0, 2);
  buffered.prepareToPlay(256, 48000.0);
  buffered.start();
  Planar bufOut(2, 20000);
  AudioBlock<float> whole = bufOut.block();
  // Paced at twice real time, as a bounce would pull -- and waiting for the
  // read-ahead between blocks, as a bounce must: a machine busy enough fell
  // behind a 2 ms-a-read source, and the underrun's silence failed this check,
  // which is about WHICH samples play, not how fast a loaded machine reads.
  const auto start = Clock::now();
  bool allReady = true;
  for (int at = 0, k = 0; at < 20000; at += 256, ++k) {
    std::this_thread::sleep_until(start + std::chrono::microseconds((long long) k * 256 * 1000000 / 48000 / 2));
    const int n = (20000 - at) < 256 ? 20000 - at : 256;
    allReady = buffered.waitForNextAudioBlockReady(n, 5000) && allReady;
    AudioScope scope;
    buffered.getNextAudioBlock(AudioSourceChannelInfo(&whole, at, n));
  }
  bool same = true;
  for (int i = 256; i < 20000; ++i) same = same && bufOut.ch(0)[i] == ramp[0][(size_t) i];
  check(allReady, "the transport's waitForNextAudioBlockReady says each block's source frames are buffered");
  check(same && !slow.readOnTestThread.load(),
        "with read-ahead: after the fade-in, every sample is the source's, read off the audio thread");
  buffered.setSource(nullptr);
}

// ── AudioSourcePlayer ────────────────────────────────────────────────────────

/** A device in the shape of the SDK's output backends: it owns a thread and
 *  calls one render function with interleaved buffers of varying size. */
struct FakeDevice {
  using RenderFn = std::function<void(float* interleaved, uint32_t frames, uint32_t channels)>;
  RenderFn render;
  uint32_t channels = 2;
  std::vector<float> captured;

  bool open(RenderFn fn) {
    render = std::move(fn);
    return true;
  }
  void runFor(int totalFrames) {
    captured.assign((size_t) totalFrames * channels, -1.0f);
    std::thread device([this, totalFrames] {
      static const int sizes[] = {480, 441, 1024, 7, 2148, 512};
      std::vector<float> buffer(4096 * (size_t) channels);
      AudioScope scope; // everything from here on is the device's callback thread
      int at = 0;
      for (int k = 0; at < totalFrames; ++k) {
        const int want = sizes[k % 6];
        const int n = (totalFrames - at) < want ? totalFrames - at : want;
        render(buffer.data(), (uint32_t) n, channels);
        std::memcpy(captured.data() + (size_t) at * channels, buffer.data(), (size_t) n * channels * sizeof(float));
        at += n;
      }
    });
    device.join();
  }
};

void testPlayer() {
  section("AudioSourcePlayer");
  const int N = 20000;
  std::vector<std::vector<float>> ramp(2, std::vector<float>((size_t) N));
  for (int i = 0; i < N; ++i) {
    ramp[0][(size_t) i] = (float) i;
    ramp[1][(size_t) i] = -(float) i;
  }
  auto source = memoryOf(ramp);
  AudioSourcePlayer player;
  FakeDevice device;
  device.channels = 6;
  player.setSource(source.get());
  device.open(player.renderFunction());
  player.audioDeviceAboutToStart(48000.0, 512, 6, 0);
  device.runFor(15000);
  bool same = true;
  for (int i = 0; i < 15000; ++i) {
    const float* frame = device.captured.data() + (size_t) i * 6;
    same = same && frame[0] == (float) i && frame[1] == -(float) i;
    for (int c = 2; c < 6; ++c) same = same && frame[c] == 0.0f;
  }
  check(same, "a fake device calling render() with 480, 441, 1024, 7, 2148 frames: the source, "
              "interleaved, its two channels and four silent ones");

  // Gain ramps, the reference's formula, across a callback.
  player.setGain(0.5f);
  std::vector<float> buffer(256 * 2);
  {
    AudioScope scope;
    player.render(buffer.data(), 256, 2);
  }
  bool ramped = true;
  for (int i = 0; i < 256; ++i) {
    const float src = (float) (15000 + i);
    const float g = 1.0f + (0.5f - 1.0f) * (float) i / 256.0f;
    ramped = ramped && std::fabs(buffer[(size_t) i * 2] - src * g) <= 1e-3f;
  }
  check(ramped, "setGain(0.5) ramps across the next callback");

  // Input reaches the source.
  PassThroughSource pass;
  player.setSource(&pass);
  std::vector<float> input(128 * 2), output(128 * 2, 9.0f);
  for (size_t i = 0; i < input.size(); ++i) input[i] = (float) i * 0.001f;
  player.setGain(1.0f);
  {
    AudioScope scope;
    player.renderWithInput(input.data(), output.data(), 128, 2);
    player.renderWithInput(input.data(), output.data(), 128, 2); // past the gain ramp
  }
  check(output == input, "renderWithInput (ASIO's shape): the input is copied in before the source runs");

  // The reference's planar form, with a null (disabled) channel skipped.
  std::vector<float> a(64, 5.0f), b(64, 5.0f);
  float* outs[] = {a.data(), nullptr, b.data()};
  CountingSource three(3.0f);
  player.setSource(&three);
  {
    AudioScope scope;
    player.audioDeviceIOCallback(nullptr, 0, outs, 3, 64);
  }
  check(a[10] == 3.0f && b[63] == 3.0f, "audioDeviceIOCallback fills the channels it was given, skipping a null one");

  // Swapping the source under a running device.
  CountingSource x(1.0f), y(2.0f);
  std::atomic<bool> stop{false};
  std::atomic<long> bad{0};
  player.setSource(&x);
  std::thread deviceThread([&] {
    std::vector<float> buf(300 * 2);
    AudioScope scope;
    while (!stop.load()) {
      player.render(buf.data(), 300, 2);
      const float v = buf[0];
      for (float s : buf)
        if (s != v || (v != 1.0f && v != 2.0f)) {
          bad.fetch_add(1);
          break;
        }
    }
  });
  for (int k = 0; k < 400; ++k) player.setSource(k % 2 ? &x : &y);
  stop.store(true);
  deviceThread.join();
  checkf(bad.load() == 0 && x.released.load() > 0 && y.released.load() > 0,
         "400 swaps under a running device: every callback whole, each old source released (%d, %d)",
         x.released.load(), y.released.load());
  player.audioDeviceStopped();
  player.setSource(nullptr);
}

// ── a source that hands over to the next from inside its own block ──────────
//
// The transport and the player each waited out the block in flight while
// holding the lock their other controls take. A source that set the next
// source from inside its block -- the reference allows it, its lock is
// recursive -- then waited for that lock while the other thread waited for
// the block: both stopped for good. (Not inside the allocation audit: the new
// source is prepared in the block, which is the caller's trade.)

/** Counts its lifecycle, and every few blocks calls `handOver` from inside
 *  its own block. */
struct HandOver : PositionableAudioSource {
  std::atomic<bool> prepared{false};
  std::atomic<int> prepares{0}, releases{0}, pulledReleased{0};
  int blocks = 0;
  int every = 3;
  int64_t position = 0;
  void (*handOver)(HandOver* self, void* context) = nullptr;
  void* context = nullptr;
  HandOver* other = nullptr;

  void prepareToPlay(int, double) override {
    prepared.store(true);
    prepares.fetch_add(1);
  }
  void releaseResources() override {
    prepared.store(false);
    releases.fetch_add(1);
  }
  void getNextAudioBlock(const AudioSourceChannelInfo& info) override {
    if (!prepared.load()) pulledReleased.fetch_add(1);
    for (int c = 0; c < info.numChannels(); ++c)
      for (int i = 0; i < info.numSamples; ++i) info.channel(c)[i] = 0.25f;
    position += info.numSamples;
    if (++blocks % every == 0 && handOver) handOver(this, context);
  }
  void setNextReadPosition(int64_t p) override { position = p; }
  int64_t getNextReadPosition() const override { return position; }
  int64_t getTotalLength() const override { return (int64_t) 1 << 40; }
  bool isLooping() const override { return false; }
};

void stuckAndStop(const char* what) {
  // Joining would hang the test run; say what happened and stop.
  check(false, what);
  std::printf("AUDIO SOURCES TESTS FAILED\n");
  std::fflush(stdout);
  std::_Exit(1);
}

/** Run `pull` on one thread and `control` on another until each has done
 *  `enough` rounds. Neither moving for 800 ms is a deadlock: reported as
 *  `stuck`, and the run ends there. */
template <typename Pull, typename Control>
void runTogether(Pull pull, Control control, long enough, std::atomic<long>& pulls, std::atomic<long>& controls,
                 const char* stuck) {
  std::atomic<bool> finished{false};
  std::thread audio([&] {
    while (!finished.load()) {
      pull();
      pulls.fetch_add(1);
    }
  });
  std::thread ui([&] {
    for (int k = 0; !finished.load(); ++k) {
      control(k);
      controls.fetch_add(1);
    }
  });
  long lastPulls = -1, lastControls = -1;
  int still = 0;
  while (pulls.load() < enough || controls.load() < enough) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const bool moved = pulls.load() != lastPulls || controls.load() != lastControls;
    lastPulls = pulls.load();
    lastControls = controls.load();
    still = moved ? 0 : still + 1;
    if (still >= 8) stuckAndStop(stuck);
  }
  finished.store(true);
  audio.join();
  ui.join();
}

void testHandOver() {
  section("a source setting the next one from inside its own block");
  {
    HandOver a, b, c;
    std::atomic<long> handOvers{0};
    {
      AudioTransportSource transport;
      transport.prepareToPlay(64, 48000.0);
      struct Ctx { AudioTransportSource* t; std::atomic<long>* n; } ctx{&transport, &handOvers};
      auto relay = [](HandOver* self, void* context) {
        Ctx* x = (Ctx*) context;
        x->t->setSource(self->other);
        x->t->start();
        (void) x->t->getCurrentPosition(); // a control that takes the same lock
        x->n->fetch_add(1);
      };
      for (HandOver* h : {&a, &b, &c}) {
        h->handOver = relay;
        h->context = &ctx;
      }
      a.other = &b;
      b.other = &a;
      c.other = &a;
      transport.setSource(&a);
      transport.start();
      std::atomic<long> pulls{0}, controls{0};
      Planar buf(2, 64);
      AudioBlock<float> whole = buf.block();
      runTogether(
          [&] { transport.getNextAudioBlock(AudioSourceChannelInfo(whole)); },
          [&](int k) {
            transport.setSource(k % 3 == 0 ? &c : (k % 3 == 1 ? &a : &b));
            transport.start();
          },
          2000, pulls, controls, "transport: a source setting the next while another thread does: deadlocked");
      checkf(handOvers.load() > 50, "transport: %ld hand-overs from inside a block while another thread made %ld, "
             "across %ld blocks, without a deadlock", handOvers.load(), controls.load(), pulls.load());
    }
    const int pulledReleased = a.pulledReleased + b.pulledReleased + c.pulledReleased;
    checkf(pulledReleased == 0, "transport: no source pulled after its release (%d times)", pulledReleased);
    checkf(!a.prepared && !b.prepared && !c.prepared && a.releases > 0 && b.releases > 0 && c.releases > 0,
           "transport: every source ends released, the ones set down in a block too (%d/%d, %d/%d, %d/%d "
           "releases/prepares)", a.releases.load(), a.prepares.load(), b.releases.load(), b.prepares.load(),
           c.releases.load(), c.prepares.load());
  }
  {
    HandOver a, b, c;
    std::atomic<long> handOvers{0};
    {
      AudioSourcePlayer player;
      player.audioDeviceAboutToStart(48000.0, 64, 2, 0);
      struct Ctx { AudioSourcePlayer* p; std::atomic<long>* n; } ctx{&player, &handOvers};
      auto relay = [](HandOver* self, void* context) {
        Ctx* x = (Ctx*) context;
        x->p->setSource(self->other);
        x->n->fetch_add(1);
      };
      for (HandOver* h : {&a, &b, &c}) {
        h->handOver = relay;
        h->context = &ctx;
      }
      a.other = &b;
      b.other = &a;
      c.other = &b;
      player.setSource(&a);
      std::atomic<long> pulls{0}, controls{0};
      std::vector<float> buf(64 * 2);
      runTogether(
          [&] { player.render(buf.data(), 64, 2); },
          [&](int k) {
            if (k % 50 == 49) {
              // The device stopping and starting again waits out the
              // callback the same way.
              player.audioDeviceStopped();
              player.audioDeviceAboutToStart(48000.0, 64, 2, 0);
            } else {
              player.setSource(k % 3 == 0 ? &c : (k % 3 == 1 ? &a : &b));
            }
          },
          2000, pulls, controls, "player: a source setting the next while another thread does: deadlocked");
      checkf(handOvers.load() > 50, "player: %ld hand-overs from inside the callback while another thread made %ld, "
             "across %ld callbacks, without a deadlock", handOvers.load(), controls.load(), pulls.load());
      player.audioDeviceStopped();
    }
    const int pulledReleased = a.pulledReleased + b.pulledReleased + c.pulledReleased;
    checkf(pulledReleased == 0, "player: no source pulled after its release (%d times)", pulledReleased);
    checkf(!a.prepared && !b.prepared && !c.prepared && a.releases > 0 && b.releases > 0 && c.releases > 0,
           "player: every source ends released, the ones set down in the callback too (%d/%d, %d/%d, %d/%d "
           "releases/prepares)", a.releases.load(), a.prepares.load(), b.releases.load(), b.prepares.load(),
           c.releases.load(), c.prepares.load());
  }
}

} // namespace

int main() {
  std::printf("audio_source.h: the pull-model sources\n");
  testMemorySource();
  testReaderSource();
  testReopenError();
  testRemapping();
  testIirSource();
  testReverbSource();
  testMixer();
  testTone();
  testResampling();
  testBuffering();
  testTransport();
  testPlayer();
  testHandOver();

  section("real-time audit");
  checkf(g_audioAllocations.load() == 0, "allocations made inside audio callbacks, across every test above: %ld",
         g_audioAllocations.load());

  std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
  std::printf("%s\n", g_failures == 0 ? "AUDIO SOURCES TESTS PASSED" : "AUDIO SOURCES TESTS FAILED");
  return g_failures == 0 ? 0 : 1;
}
