// SPDX-License-Identifier: Apache-2.0
// Mutation fuzzing for the JPEG and GIF decoders, the sniffing in front of
// them, and SVG text: every fixture in tests/data/images and
// tests/data/svgtext as a seed, then every truncation,
// random byte flips, flips aimed at the headers, runs of the bytes that break
// size arithmetic, and seeds spliced into each other. The verdict is "did it
// come back": a mutated file may be refused, but it may not crash, hang, or --
// under ASan -- touch a byte it does not own. A watchdog turns a hang into a
// named failure with the input saved beside the process, as fuzz_parsers does.
//
//   fuzz_images                        a quick pass (ctest)
//   fuzz_images --iterations 400       the deep pass, under ASan
//   fuzz_images --data-dir <dir>       the fixtures from elsewhere
#include <sonore/gfx/font_match.h>
#include <sonore/gfx/gif.h>
#include <sonore/gfx/image_format.h>
#include <sonore/gfx/jpeg.h>
#include <sonore/gfx/svg.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifndef SONORE_TEST_DATA_DIR
#define SONORE_TEST_DATA_DIR "tests/data"
#endif

namespace {

std::atomic<long long> g_deadlineMs{0};
std::atomic<bool> g_done{false};
const char* g_phase = "";
std::string g_seedName;
size_t g_case = 0;
std::vector<uint8_t> g_current;
long g_sink = 0;

long long nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void saveCurrent(const char* name) {
  if (FILE* f = std::fopen(name, "wb")) {
    if (!g_current.empty()) std::fwrite(g_current.data(), 1, g_current.size(), f);
    std::fclose(f);
  }
}

using SignalHandler = void (*)(int);
SignalHandler g_previous[64] = {};

void onCrash(int sig) {
  saveCurrent("fuzz_images_crash.bin");
  std::fprintf(stderr, "  CRASH signal %d seed=%s phase=%s case=%zu size=%zu -- the input is in fuzz_images_crash.bin\n",
               sig, g_seedName.c_str(), g_phase, g_case, g_current.size());
  std::fflush(stderr);
  const SignalHandler previous = (sig >= 0 && sig < 64) ? g_previous[sig] : nullptr;
  if (previous && previous != SIG_DFL && previous != SIG_IGN) {
    std::signal(sig, previous);
    std::raise(sig);
  }
  std::_Exit(98);
}

void watchdog() {
  while (!g_done.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const long long deadline = g_deadlineMs.load();
    if (deadline != 0 && nowMs() > deadline) {
      std::fprintf(stderr, "  HANG seed=%s phase=%s case=%zu size=%zu -- the input is in fuzz_images_hang.bin\n",
                   g_seedName.c_str(), g_phase, g_case, g_current.size());
      saveCurrent("fuzz_images_hang.bin");
      std::fflush(stderr);
      std::_Exit(99);
    }
  }
}

/** The test fonts, registered once: SVG text is laid out against them and
 *  never against the machine's, so a case does the same work everywhere. */
sonore::gfx::FontMatcher& textFonts() {
  static sonore::gfx::FontMatcher fonts(sonore::gfx::FontMatcher::EmbeddedOnly);
  return fonts;
}

/** An SVG case: parsed with the text laid out, and drawn. */
void runSvg(const std::vector<uint8_t>& b) {
  const sonore::gfx::Drawable d =
      sonore::gfx::Drawable::parse(std::string(b.begin(), b.end()), textFonts());
  sonore::gfx::Bitmap target(64, 48);
  {
    sonore::gfx::Graphics g(target);
    d.draw(g, sonore::gfx::Rect(0.0f, 0.0f, 64.0f, 48.0f));
  }
  g_sink += d.numShapes() + d.numTexts() + target.pixelAt(10, 10).a;
  for (int i = 0; i < d.numTexts() && i < 4; ++i) g_sink += (long) d.textAt(i).toPath().points().size();
}

bool g_svgSeed = false;

/** One case: through the dispatcher with its defaults, and through each
 *  decoder directly with every frame asked for and the orientation off. */
void runOnce(const std::vector<uint8_t>& b) {
  if (g_svgSeed) {
    runSvg(b);
    return;
  }
  sonore::gfx::ImageFileFormat::Options all;
  all.allFrames = true;
  const sonore::gfx::DecodedImage d = sonore::gfx::ImageFileFormat::decode(b, all);
  g_sink += d.bitmap.width() + (long) d.frames.size() + (long) d.error.size();

  sonore::gfx::JpegDecoder::Options raw;
  raw.applyOrientation = false;
  const sonore::gfx::JpegImage j = sonore::gfx::JpegDecoder::decode(b, raw);
  g_sink += j.bitmap.height() + (long) j.warning.size();

  // A tighter canvas than the default, so a header flipped to 8000x8000 is
  // tested for being refused rather than spending the run filling 256 MB.
  sonore::gfx::GifDecoder::Options gifOptions;
  gifOptions.maxPixels = 4u * 1024u * 1024u;
  gifOptions.maxTotalPixels = 8u * 1024u * 1024u;
  const sonore::gfx::GifImage g = sonore::gfx::GifDecoder::decode(b, gifOptions);
  for (const auto& f : g.frames) g_sink += f.image.width() + f.delayMs;
}

/** The deadline per case: generous, because an ASan build is several times
 *  slower and a valid 4-megapixel GIF is legitimately work. */
void guarded(const std::vector<uint8_t>& b) {
  g_current = b;
  g_deadlineMs.store(nowMs() + 10000);
  runOnce(b);
  g_deadlineMs.store(0);
}

struct Seed {
  std::string name;
  std::vector<uint8_t> bytes;
};

size_t fuzzOne(const Seed& seed, const std::vector<Seed>& all, size_t iterations, uint32_t rngSeed) {
  size_t cases = 0;
  g_seedName = seed.name;
  g_svgSeed = seed.name.size() > 4 && seed.name.compare(seed.name.size() - 4, 4, ".svg") == 0;
  const std::vector<uint8_t>& s = seed.bytes;

  g_phase = "seed";
  g_case = 0;
  guarded(s);
  ++cases;

  g_phase = "truncate";
  const size_t step = s.size() > 400 ? s.size() / 400 : 1;
  for (size_t len = 0; len < s.size(); len += step) {
    g_case = len;
    guarded(std::vector<uint8_t>(s.begin(), s.begin() + (long) len));
    ++cases;
  }

  std::mt19937 rng(rngSeed);
  g_phase = "flip";
  for (size_t it = 0; it < iterations && !s.empty(); ++it) {
    g_case = it;
    std::vector<uint8_t> b = s;
    const int flips = 1 + (int) (rng() % 8);
    for (int k = 0; k < flips; ++k) b[rng() % b.size()] = (uint8_t) rng();
    guarded(b);
    ++cases;
  }

  // The headers: markers, lengths, sizes, table counts, sampling factors and
  // code sizes all live in the first part of the file, and random flips over
  // the whole of it spend most of their effort on entropy-coded data.
  g_phase = "header";
  const size_t headerSpan = (std::max)((size_t) 16, (std::min)(s.size(), (size_t) 700));
  for (size_t it = 0; it < iterations && !s.empty(); ++it) {
    g_case = it;
    std::vector<uint8_t> b = s;
    const int flips = 1 + (int) (rng() % 4);
    for (int k = 0; k < flips; ++k) {
      const size_t at = rng() % (std::min)(headerSpan, b.size());
      switch (rng() % 4) {
        case 0: b[at] = (uint8_t) rng(); break;
        case 1: b[at] = (uint8_t) (b[at] + 1); break;
        case 2: b[at] = (uint8_t) (b[at] - 1); break;
        default: b[at] ^= (uint8_t) (1u << (rng() % 8)); break;
      }
    }
    guarded(b);
    ++cases;
  }

  g_phase = "runs";
  static const uint8_t kPatterns[] = {0x00, 0xFF, 0x7F, 0x80, 0x01, 0xFE, 0xD8, 0xD9, 0xDA, 0x2C, 0x21,
                                      '<', '>', '"', ' ', '\n', '9', '-', 'e', '%'};
  for (size_t it = 0; it < iterations && !s.empty(); ++it) {
    g_case = it;
    std::vector<uint8_t> b = s;
    const size_t at = rng() % b.size();
    const size_t len = 1 + rng() % 16;
    const uint8_t pat = kPatterns[rng() % sizeof(kPatterns)];
    for (size_t k = 0; k < len && at + k < b.size(); ++k) b[at + k] = (rng() % 3 == 0) ? (uint8_t) rng() : pat;
    guarded(b);
    ++cases;
  }

  // One file's head on another's tail: tables from one encoder, data from
  // another, which no single mutation of either produces.
  g_phase = "splice";
  for (size_t it = 0; it < iterations / 2 && !s.empty() && all.size() > 1; ++it) {
    g_case = it;
    const Seed& other = all[rng() % all.size()];
    if (g_svgSeed != (other.name.size() > 4 && other.name.compare(other.name.size() - 4, 4, ".svg") == 0))
      continue; // a JPEG's tail on an SVG's head tests nothing the flips do not
    if (other.bytes.empty()) continue;
    std::vector<uint8_t> b(s.begin(), s.begin() + (long) (rng() % s.size()));
    const size_t from = rng() % other.bytes.size();
    b.insert(b.end(), other.bytes.begin() + (long) from, other.bytes.end());
    guarded(b);
    ++cases;
  }

  g_phase = "mixed";
  for (size_t it = 0; it < iterations / 2 && !s.empty(); ++it) {
    g_case = it;
    std::vector<uint8_t> b(s.begin(), s.begin() + (long) (rng() % s.size()));
    for (int k = 0; k < 3 && !b.empty(); ++k) b[rng() % b.size()] = (uint8_t) rng();
    guarded(b);
    ++cases;
  }
  return cases;
}

bool readAll(const std::string& path, std::vector<uint8_t>& out) {
  return sonore::gfx::ImageFileFormat::readFile(path, out);
}

} // namespace

int main(int argc, char** argv) {
  size_t iterations = 60; // the ctest pass: seconds
  std::string dataDir = SONORE_TEST_DATA_DIR;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--iterations" && i + 1 < argc) iterations = (size_t) std::atol(argv[++i]);
    else if (a == "--data-dir" && i + 1 < argc) dataDir = argv[++i];
    else {
      std::fprintf(stderr, "usage: fuzz_images [--iterations N] [--data-dir dir]\n");
      return 2;
    }
  }
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::thread dog(watchdog);
  dog.detach();
  for (int sig : {SIGSEGV, SIGABRT, SIGILL, SIGFPE}) {
    const SignalHandler previous = std::signal(sig, onCrash);
    if (sig >= 0 && sig < 64) g_previous[sig] = previous == SIG_ERR ? nullptr : previous;
  }

  static const char* const kSeeds[] = {
      "baseline_444.jpg", "baseline_422.jpg", "baseline_420.jpg", "progressive_420.jpg", "progressive_444.jpg",
      "restart_420.jpg", "restart_progressive.jpg", "grey.jpg", "grey_progressive.jpg", "cmyk.jpg", "rgb.jpg",
      "sampling_440.jpg", "sampling_411.jpg", "sampling_mixed.jpg", "sampling_32.jpg", "separate_scans.jpg",
      "quant16.jpg", "ycck.jpg", "orient_6.jpg", "still_87a.gif", "interlaced.gif", "lzw_deferred.gif",
      "local_tables.gif", "anim_pillow.gif", "anim_keep.gif", "basic.svg", "anchor.svg", "tspans.svg",
      "whitespace.svg", "weights.svg", "styled.svg", "fallback.svg", "clip.svg"};
  std::vector<Seed> seeds;
  for (const char* name : kSeeds) {
    Seed s;
    s.name = name;
    const std::string folder = std::string(name).find(".svg") != std::string::npos ? "/svgtext/" : "/images/";
    if (!readAll(dataDir + folder + name, s.bytes)) {
      std::fprintf(stderr, "  FAIL cannot read seed %s%s%s\n", dataDir.c_str(), folder.c_str(), name);
      return 1;
    }
    seeds.push_back(std::move(s));
  }
  for (const char* face : {"SonoreText-Regular.ttf", "SonoreText-Bold.ttf", "SonoreText-Italic.ttf",
                           "SonoreTextVar.ttf"}) {
    std::vector<uint8_t> bytes;
    if (!readAll(dataDir + "/svgtext/" + face, bytes) || !textFonts().registerTypeface(bytes.data(), bytes.size())) {
      std::fprintf(stderr, "  FAIL cannot register %s\n", face);
      return 1;
    }
  }
  textFonts().setGenericFamily("serif", "Sonore Text");

  std::printf("Sonore SDK image fuzz -- %zu iterations per phase, %zu seeds\n", iterations, seeds.size());
  size_t total = 0;
  const long long started = nowMs();
  for (size_t i = 0; i < seeds.size(); ++i) {
    const size_t cases = fuzzOne(seeds[i], seeds, iterations, 0x5EED0000u + (uint32_t) i);
    std::printf("  ok   %-24s %6zu cases\n", seeds[i].name.c_str(), cases);
    total += cases;
  }
  g_done.store(true);
  std::printf("%zu cases in %.1f s, none crashed or hung (sink %ld)\n", total, (double) (nowMs() - started) / 1000.0,
              g_sink);
  std::printf("FUZZ IMAGES PASSED\n");
  return 0;
}
