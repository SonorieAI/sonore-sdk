// SPDX-License-Identifier: Apache-2.0
// Mutation fuzzing for the audio-I/O parsers that read bytes this SDK did not
// just write:
//
//   cdtoc       CDToc::parse -- a drive's READ TOC answer
//   thumbnail   AudioThumbnail::loadFrom -- one waveform from a cache file
//   thumbcache  AudioThumbnailCache::loadFrom -- a whole cache file
//
// Same schedule as fuzz_parsers: the seed, every truncation, random byte
// flips, runs of the bytes that break size arithmetic, truncation plus flips.
// A mutant may be refused; it may not crash, hang, or -- the stronger
// property these three can promise -- be ACCEPTED in a state that breaks
// their own invariants. So every input that parses is checked: a TOC's
// tracks ascend and end before the lead-out; a thumbnail or a cache that
// loads saves back to exactly the bytes it was loaded from, or to bytes that
// load to the same thing.
//
// (System decoders get their corrupt-file pass in audio_io_test: the parser
// there is the operating system's, and what this SDK owns is that a failure
// comes back as `false`.)
//
//   fuzz_audio_io                      a quick pass (ctest)
//   fuzz_audio_io --iterations 20000   the deep pass, under ASan
#include <sonore/audio_cd.h>
#include <sonore/thumbnail_cache.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;
std::atomic<long long> g_deadline{0};
std::atomic<bool> g_done{false};
const char* g_target = "";

long long nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void fail(const char* what) {
  ++g_failures;
  std::printf("  FAIL %s: %s\n", g_target, what);
}

std::vector<uint8_t> tocSeed() {
  // Six audio tracks and a data track (an Enhanced CD), LBA addresses.
  const int32_t lba[] = {0, 15213, 32164, 46442, 63264, 80339, 92000};
  std::vector<uint8_t> t = {0, (uint8_t) (2 + 8 * 8), 1, 7};
  for (int i = 0; i < 7; ++i)
    t.insert(t.end(), {0, (uint8_t) (i == 6 ? 0x14 : 0x10), (uint8_t) (i + 1), 0,
                       (uint8_t) (lba[i] >> 24), (uint8_t) (lba[i] >> 16), (uint8_t) (lba[i] >> 8),
                       (uint8_t) lba[i]});
  t.insert(t.end(), {0, 0x10, 0xAA, 0, 0, 1, 0x74, 0x50}); // lead-out 95312
  return t;
}

sonore::AudioThumbnail thumbOf(double hz, uint32_t buckets) {
  std::vector<float> l(8000), r(8000);
  for (size_t i = 0; i < l.size(); ++i) {
    l[i] = (float) std::sin(6.283185307179586 * hz * (double) i / 44100.0);
    r[i] = 0.5f * l[i];
  }
  const float* ch[2] = {l.data(), r.data()};
  sonore::AudioThumbnail t;
  t.build(ch, 2, l.size(), buckets);
  return t;
}

std::vector<uint8_t> thumbnailSeed() {
  std::vector<uint8_t> b;
  thumbOf(440.0, 40).saveTo(b);
  return b;
}

std::vector<uint8_t> cacheSeed() {
  sonore::AudioThumbnailCache cache;
  for (int i = 0; i < 4; ++i) cache.storeThumb(thumbOf(100.0 + 100.0 * i, 10 + (uint32_t) i), 7 + (uint64_t) i);
  std::vector<uint8_t> b;
  cache.saveTo(b);
  return b;
}

void runOnce(const std::string& target, const std::vector<uint8_t>& b) {
  const uint8_t* p = b.empty() ? nullptr : b.data();
  if (target == "cdtoc") {
    sonore::CDToc toc;
    if (!sonore::CDToc::parse(p, b.size(), &toc)) return;
    if (toc.tracks.empty() || toc.firstTrack < 1 || toc.lastTrack > 99) fail("accepted a TOC with no tracks or bad numbers");
    for (size_t i = 1; i < toc.tracks.size(); ++i)
      if (toc.tracks[i].startSector <= toc.tracks[i - 1].startSector) fail("accepted tracks that go backwards");
    if (toc.leadOutSector <= toc.tracks.back().startSector) fail("accepted a lead-out before the last track");
    if (toc.leadOutSector > sonore::CDToc::msfToSector(99, 59, 74)) fail("accepted an address MSF cannot say");
    if (toc.lengthInSamples() <= 0) fail("accepted a disc with no length");
    (void) toc.cddbId();
  } else if (target == "thumbnail") {
    sonore::AudioThumbnail t;
    if (!t.loadFrom(p, b.size())) {
      if (!t.empty()) fail("a refused load changed the thumbnail");
      return;
    }
    std::vector<uint8_t> again;
    t.saveTo(again);
    if (again != b) fail("a loaded thumbnail does not save back to its own bytes");
    if (!std::isfinite(t.peak())) fail("a loaded thumbnail has a non-finite peak");
  } else if (target == "thumbcache") {
    sonore::AudioThumbnailCache cache(1u << 20);
    if (!cache.loadFrom(p, b.size())) {
      if (cache.size() != 0) fail("a refused load changed the cache");
      return;
    }
    if (cache.bytesUsed() > cache.maxBytes()) fail("a loaded cache is over its budget");
    std::vector<uint8_t> once, twice;
    cache.saveTo(once);
    sonore::AudioThumbnailCache copy(1u << 20);
    if (!copy.loadFrom(once.data(), once.size())) fail("a cache's own save does not load");
    copy.saveTo(twice);
    if (once != twice) fail("save -> load -> save is not a fixed point");
  }
}

void guarded(const std::string& target, const std::vector<uint8_t>& b) {
  g_deadline.store(nowMs() + 5000);
  runOnce(target, b);
  g_deadline.store(0);
}

size_t fuzzOne(const std::string& target, const std::vector<uint8_t>& seed, size_t iterations,
               uint32_t rngSeed) {
  size_t cases = 0;
  guarded(target, seed);
  ++cases;
  for (size_t len = 0; len < seed.size(); ++len, ++cases)
    guarded(target, std::vector<uint8_t>(seed.begin(), seed.begin() + (long) len));
  std::mt19937 rng(rngSeed);
  for (size_t it = 0; it < iterations; ++it, ++cases) {
    std::vector<uint8_t> b = seed;
    const int flips = 1 + (int) (rng() % 8);
    for (int k = 0; k < flips; ++k) b[rng() % b.size()] = (uint8_t) rng();
    guarded(target, b);
  }
  static const uint8_t kPatterns[] = {0x00, 0xFF, 0x7F, 0x80, 0x01, 0xFE, 0xAA};
  for (size_t it = 0; it < iterations; ++it, ++cases) {
    std::vector<uint8_t> b = seed;
    const size_t at = rng() % b.size();
    const size_t len = 1 + rng() % 16;
    const uint8_t pat = kPatterns[rng() % sizeof(kPatterns)];
    for (size_t k = 0; k < len && at + k < b.size(); ++k) b[at + k] = (rng() % 3 == 0) ? (uint8_t) rng() : pat;
    guarded(target, b);
  }
  for (size_t it = 0; it < iterations / 2; ++it, ++cases) {
    std::vector<uint8_t> b(seed.begin(), seed.begin() + (long) (rng() % seed.size()));
    for (int k = 0; k < 3 && !b.empty(); ++k) b[rng() % b.size()] = (uint8_t) rng();
    guarded(target, b);
  }
  return cases;
}

} // namespace

int main(int argc, char** argv) {
  size_t iterations = 2000;
  for (int i = 1; i + 1 < argc; ++i)
    if (std::strcmp(argv[i], "--iterations") == 0) iterations = (size_t) std::atol(argv[i + 1]);

  // A hang is a failure with a name, not a stuck CI job.
  std::thread watchdog([] {
    while (!g_done.load()) {
      const long long d = g_deadline.load();
      if (d && nowMs() > d) {
        std::printf("  FAIL %s: a case ran past five seconds (a hang)\n", g_target);
        std::fflush(stdout);
        std::_Exit(2);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  });

  std::printf("Sonore SDK audio-I/O parser fuzz: %zu iterations per phase\n", iterations);
  struct Job {
    const char* target;
    std::vector<uint8_t> seed;
  } jobs[] = {{"cdtoc", tocSeed()}, {"thumbnail", thumbnailSeed()}, {"thumbcache", cacheSeed()}};
  uint32_t rngSeed = 20261013;
  for (const Job& j : jobs) {
    g_target = j.target;
    const int before = g_failures;
    const size_t cases = fuzzOne(j.target, j.seed, iterations, rngSeed++);
    std::printf("  %s %-11s %zu cases, seed %zu bytes\n", g_failures == before ? "ok  " : "FAIL",
                j.target, cases, j.seed.size());
  }
  g_done.store(true);
  watchdog.join();
  std::printf("%s\n", g_failures ? "SONORE AUDIO IO FUZZ FAILED" : "SONORE AUDIO IO FUZZ PASSED");
  return g_failures ? 1 : 0;
}
