// SPDX-License-Identifier: Apache-2.0
// Images: the JPEG and GIF decoders against what Pillow decodes the same files
// to, the JPEG encoder by round trip, the format sniffing, ImageCache's
// sharing, expiry and budget, ScaledImage, the image types, Parallelogram's
// geometry, and SVG text drawn with real fonts against resvg.
//
// The fixtures and their expected pixels are made by
// tests/data/images/make_images.py (Pillow's libjpeg-turbo and GIF reader are
// the oracle) and tests/data/svgtext/make_svgtext.py (resvg).
#include <sonore/gfx/font_match.h>
#include <sonore/gfx/gif.h>
#include <sonore/gfx/graphics.h>
#include <sonore/gfx/image.h>
#include <sonore/gfx/image_format.h>
#include <sonore/gfx/jpeg.h>
#include <sonore/gfx/parallelogram.h>
#include <sonore/gfx/png.h>
#include <sonore/gfx/svg.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "temp_path.h"

#ifndef SONORE_TEST_DATA_DIR
#define SONORE_TEST_DATA_DIR "tests/data"
#endif

using namespace sonore::gfx;

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
  std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_failures;
}

std::string dataPath(const std::string& rel) { return std::string(SONORE_TEST_DATA_DIR) + "/" + rel; }

std::vector<uint8_t> readBytes(const std::string& path) {
  std::vector<uint8_t> out;
  ImageFileFormat::readFile(path, out);
  return out;
}

uint64_t fnv64(const uint8_t* p, size_t n) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
  return h;
}

// ── JPEG against Pillow ──────────────────────────────────────────────────────

struct Diff {
  int max = 0;
  double mean = 0.0;
  bool sameSize = false;
};

Diff compareRgb(const Bitmap& got, const Bitmap& want) {
  Diff d;
  d.sameSize = got.width() == want.width() && got.height() == want.height();
  if (!d.sameSize) return d;
  double sum = 0.0;
  for (int y = 0; y < got.height(); ++y)
    for (int x = 0; x < got.width(); ++x) {
      const PremulColour a = got.pixelAt(x, y), b = want.pixelAt(x, y);
      const int dr = std::abs(a.r - b.r), dg = std::abs(a.g - b.g), db = std::abs(a.b - b.b);
      d.max = (std::max)(d.max, (std::max)(dr, (std::max)(dg, db)));
      sum += dr + dg + db;
      if (a.a != 255) d.max = 999; // a JPEG is opaque
    }
  d.mean = sum / (3.0 * got.width() * got.height());
  return d;
}

void testJpegFixtures(const std::vector<std::vector<std::string>>& lines) {
  std::printf("JPEG fixtures, against Pillow (libjpeg-turbo)\n");
  int count = 0, worst = 0;
  double worstMean = 0.0;
  for (const auto& f : lines) {
    if (f.empty() || f[0] != "jpeg" || f.size() < 6) continue;
    ++count;
    const std::vector<uint8_t> bytes = readBytes(dataPath("images/" + f[1]));
    const PngImage want = PngDecoder::decode(readBytes(dataPath("images/" + f[2])));
    const JpegImage got = JpegDecoder::decode(bytes);
    const Diff d = compareRgb(got.bitmap, want.bitmap);
    char msg[256];
    std::snprintf(msg, sizeof(msg), "%-24s %dx%d  max |diff| %d, mean %.4f", f[1].c_str(), got.bitmap.width(),
                  got.bitmap.height(), d.max, d.mean);
    // Bit-exact is what this decoder does today; the tolerance is so that
    // a libjpeg that changes its rounding does not fail the build.
    check(got.ok() && got.warning.empty() && want.ok() && d.sameSize && d.max <= 2 && d.mean <= 0.05, msg);
    worst = (std::max)(worst, d.max);
    worstMean = (std::max)(worstMean, d.mean);
    check(got.orientation == std::atoi(f[3].c_str()), "  orientation reported as the EXIF says");
    check(got.progressive == (std::atoi(f[4].c_str()) != 0), "  progressive reported");
    check(got.components == std::atoi(f[5].c_str()), "  component count reported");
    if (got.orientation >= 5) {
      JpegDecoder::Options raw;
      raw.applyOrientation = false;
      const JpegImage stored = JpegDecoder::decode(bytes, raw);
      check(stored.bitmap.width() == got.bitmap.height() && stored.bitmap.height() == got.bitmap.width() &&
                !stored.orientationApplied && got.orientationApplied,
            "  orientation off: the pixels as stored, on their side");
    }
  }
  char msg[160];
  std::snprintf(msg, sizeof(msg), "%d JPEG fixtures; worst max |diff| %d, worst mean %.4f", count, worst, worstMean);
  check(count >= 26, msg);
}

void testJpegRefusals() {
  std::printf("JPEG: what is refused, and damage that is not\n");
  const std::vector<uint8_t> good = readBytes(dataPath("images/baseline_420.jpg"));
  auto withMarker = [&](uint8_t from, uint8_t to) {
    std::vector<uint8_t> b = good;
    for (size_t i = 0; i + 1 < b.size(); ++i)
      if (b[i] == 0xFF && b[i + 1] == from) {
        b[i + 1] = to;
        break;
      }
    return b;
  };
  check(JpegDecoder::decode(withMarker(0xC0, 0xC9)).error == "arithmetic-coded JPEG is not supported",
        "arithmetic coding refused by name");
  check(JpegDecoder::decode(withMarker(0xC0, 0xC3)).error == "lossless JPEG is not supported", "lossless refused by name");
  check(JpegDecoder::decode(withMarker(0xC0, 0xC5)).error == "hierarchical JPEG is not supported",
        "hierarchical refused by name");
  {
    std::vector<uint8_t> b = good;
    for (size_t i = 0; i + 4 < b.size(); ++i)
      if (b[i] == 0xFF && b[i + 1] == 0xC0) {
        b[i + 4] = 12; // precision
        break;
      }
    check(JpegDecoder::decode(b).error == "12-bit JPEGs are not supported", "12-bit refused by name");
  }
  {
    // 60000 x 60000: refused before a byte is allocated for it.
    std::vector<uint8_t> b = good;
    for (size_t i = 0; i + 8 < b.size(); ++i)
      if (b[i] == 0xFF && b[i + 1] == 0xC0) {
        b[i + 5] = 0xEA;
        b[i + 6] = 0x60;
        b[i + 7] = 0xEA;
        b[i + 8] = 0x60;
        break;
      }
    check(JpegDecoder::decode(b).error == "JPEG is larger than this decoder will allocate", "a giant header refused");
    // 4000 x 4000 is under the cap, and far more than these bytes can be.
    b = good;
    for (size_t i = 0; i + 8 < b.size(); ++i)
      if (b[i] == 0xFF && b[i + 1] == 0xC0) {
        b[i + 5] = 0x0F;
        b[i + 6] = 0xA0;
        b[i + 7] = 0x0F;
        b[i + 8] = 0xA0;
        break;
      }
    check(JpegDecoder::decode(b).error == "JPEG claims more blocks than the file could hold",
          "a header claiming more than the file holds refused");
  }
  {
    // Two hundred one-bit codes in a Huffman table: refused before the
    // first code past the room is entered -- the lookup table was written
    // first, 100 KB past its end, and a 225-byte file broke the stack.
    std::vector<uint8_t> b = {0xFF, 0xD8, 0xFF, 0xC4, 0x00, (uint8_t) (2 + 17 + 200), 0x00, 200};
    b.resize(b.size() + 15 + 200, 0);
    b.push_back(0xFF);
    b.push_back(0xD9);
    check(JpegDecoder::decode(b).error == "JPEG Huffman table is not a prefix code",
          "a Huffman table with more short codes than their length holds refused, nothing written past it");
  }
  check(JpegDecoder::decode(nullptr, 0).error == "not a JPEG file", "nothing is not a JPEG");
  check(!JpegDecoder::decode(readBytes(dataPath("images/still_87a.gif"))).ok(), "a GIF is not a JPEG");
  {
    // Cut in the second row of MCUs: a picture still, the rest grey, and a
    // warning saying so.
    size_t sos = 0;
    for (size_t i = 0; i + 1 < good.size(); ++i)
      if (good[i] == 0xFF && good[i + 1] == 0xDA) sos = i;
    std::vector<uint8_t> half(good.begin(), good.begin() + (long) (sos + (good.size() - sos) * 3 / 4));
    const JpegImage cut = JpegDecoder::decode(half);
    check(cut.ok() && !cut.warning.empty() && cut.bitmap.width() == 37, "a truncated JPEG decodes with a warning");
    const PngImage want = PngDecoder::decode(readBytes(dataPath("images/baseline_420.png")));
    // The first row of MCUs arrived; it matches, but for its last two rows,
    // whose chroma is filtered with the row below.
    int maxTop = 0;
    for (int y = 0; y < 14; ++y)
      for (int x = 0; x < 37; ++x) {
        const PremulColour a = cut.bitmap.pixelAt(x, y), b = want.bitmap.pixelAt(x, y);
        maxTop = (std::max)(maxTop, std::abs(a.g - b.g));
      }
    check(maxTop == 0, "  the part that arrived is exact");
  }
  {
    // A progressive file stopped after its first scans: blurry, not broken.
    std::vector<uint8_t> p = readBytes(dataPath("images/progressive_420.jpg"));
    size_t secondScan = 0;
    int scans = 0;
    for (size_t i = 0; i + 1 < p.size(); ++i)
      if (p[i] == 0xFF && p[i + 1] == 0xDA && ++scans == 3) {
        secondScan = i;
        break;
      }
    std::vector<uint8_t> early(p.begin(), p.begin() + (long) secondScan);
    early.push_back(0xFF);
    early.push_back(0xD9);
    const JpegImage blurry = JpegDecoder::decode(early);
    const PngImage want = PngDecoder::decode(readBytes(dataPath("images/progressive_420.png")));
    // The DC terms and the first few of the luma's AC: each 16x16 area's
    // average colour is already right, though its detail is not. (libjpeg's
    // own decode of the same bytes smooths the blocks and is 4.4 off.)
    double worstArea = 0.0, sumArea = 0.0;
    int areas = 0;
    for (int by = 0; by + 16 <= 29; by += 16)
      for (int bx = 0; bx + 16 <= 37; bx += 16) {
        double got[3] = {0, 0, 0}, ref[3] = {0, 0, 0};
        for (int y = by; y < by + 16; ++y)
          for (int x = bx; x < bx + 16; ++x) {
            const PremulColour a = blurry.bitmap.pixelAt(x, y), b = want.bitmap.pixelAt(x, y);
            got[0] += a.r, got[1] += a.g, got[2] += a.b;
            ref[0] += b.r, ref[1] += b.g, ref[2] += b.b;
          }
        for (int k = 0; k < 3; ++k) {
          const double e = std::fabs(got[k] - ref[k]) / 256.0;
          worstArea = (std::max)(worstArea, e);
          sumArea += e;
        }
        ++areas;
      }
    char msg[200];
    std::snprintf(msg, sizeof(msg),
                  "a progressive JPEG cut after two scans: each 16x16 area's colour right (mean %.2f, worst %.2f)",
                  sumArea / (3.0 * areas), worstArea);
    check(blurry.ok() && blurry.bitmap.width() == 37 && sumArea / (3.0 * areas) < 2.0 && worstArea < 4.0, msg);
  }
}

void testJpegEncoder() {
  std::printf("JPEG encoder, by round trip\n");
  const PngImage art = PngDecoder::decode(readBytes(dataPath("images/baseline_444.png")));
  auto psnr = [](const Bitmap& a, const Bitmap& b) {
    double se = 0.0;
    for (int y = 0; y < a.height(); ++y)
      for (int x = 0; x < a.width(); ++x) {
        const PremulColour p = a.pixelAt(x, y), q = b.pixelAt(x, y);
        se += (p.r - q.r) * (p.r - q.r) + (p.g - q.g) * (p.g - q.g) + (p.b - q.b) * (p.b - q.b);
      }
    const double mse = se / (3.0 * a.width() * a.height());
    return mse <= 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
  };
  JpegEncoder::Options high;
  high.quality = 95;
  high.subsampleChroma = false;
  const std::vector<uint8_t> fine = JpegEncoder::encode(art.bitmap, high);
  JpegEncoder::Options low;
  low.quality = 40;
  const std::vector<uint8_t> coarse = JpegEncoder::encode(art.bitmap, low);
  const JpegImage a = JpegDecoder::decode(fine), b = JpegDecoder::decode(coarse);
  char msg[160];
  std::snprintf(msg, sizeof(msg), "quality 95, 4:4:4: %zu bytes, PSNR %.1f dB", fine.size(), psnr(a.bitmap, art.bitmap));
  check(a.ok() && a.warning.empty() && psnr(a.bitmap, art.bitmap) > 38.0, msg);
  std::snprintf(msg, sizeof(msg), "quality 40, 4:2:0: %zu bytes, PSNR %.1f dB", coarse.size(),
                psnr(b.bitmap, art.bitmap));
  // Pillow's libjpeg at the same settings: 803 bytes and 21.0 dB on this
  // picture, whose red and blue edges are what 4:2:0 is worst at.
  check(b.ok() && b.warning.empty() && psnr(b.bitmap, art.bitmap) > 20.0 && coarse.size() < fine.size(), msg);
  check(fine.size() > 4 && fine[0] == 0xFF && fine[1] == 0xD8 && fine[fine.size() - 2] == 0xFF &&
            fine[fine.size() - 1] == 0xD9,
        "starts with SOI and ends with EOI");
  // Transparency flattened over the background, not left as black.
  Bitmap clear(16, 16);
  JpegEncoder::Options onRed;
  onRed.background = Colour(255, 0, 0);
  const JpegImage flat = JpegDecoder::decode(JpegEncoder::encode(clear, onRed));
  const PremulColour c = flat.bitmap.pixelAt(8, 8);
  check(flat.ok() && c.r > 240 && c.g < 16 && c.b < 16, "a transparent bitmap encodes as its background");
  check(JpegEncoder::encode(Bitmap()).empty(), "an empty bitmap encodes to nothing");
  {
    // Sizes that are not whole MCUs: the edges repeat rather than ring.
    Bitmap odd(17, 9);
    odd.clear(Colour(30, 160, 90));
    const JpegImage o = JpegDecoder::decode(JpegEncoder::encode(odd, low));
    const PremulColour e = o.bitmap.pixelAt(16, 8);
    check(o.bitmap.width() == 17 && o.bitmap.height() == 9 && std::abs(e.g - 160) <= 3 && std::abs(e.r - 30) <= 3,
          "a 17x9 image keeps its corner colour");
  }
}

// ── GIF against Pillow ───────────────────────────────────────────────────────

void testGifFixtures(const std::vector<std::vector<std::string>>& lines) {
  std::printf("GIF fixtures, against Pillow -- exact\n");
  int files = 0, frames = 0;
  for (size_t i = 0; i < lines.size(); ++i) {
    const auto& f = lines[i];
    if (f.empty() || f[0] != "gif" || f.size() < 6) continue;
    ++files;
    const GifImage got = GifDecoder::decode(readBytes(dataPath("images/" + f[1])));
    const int w = std::atoi(f[2].c_str()), h = std::atoi(f[3].c_str());
    const int loop = std::atoi(f[4].c_str()), n = std::atoi(f[5].c_str());
    char msg[200];
    std::snprintf(msg, sizeof(msg), "%-18s %dx%d, %d frame(s), loop %d", f[1].c_str(), got.width, got.height,
                  (int) got.frames.size(), got.loopCount);
    check(got.ok() && got.warning.empty() && got.width == w && got.height == h && (int) got.frames.size() == n &&
              got.loopCount == loop,
          msg);
    for (int k = 0; k < n && k < (int) got.frames.size(); ++k) {
      const auto& e = lines[i + 1 + (size_t) k];
      if (e.size() < 8) continue;
      const GifFrame& fr = got.frames[(size_t) k];
      ++frames;
      const uint64_t hash = fnv64(fr.image.data(), fr.image.sizeInBytes());
      const uint64_t want = std::strtoull(e[7].c_str(), nullptr, 16);
      std::snprintf(msg, sizeof(msg), "  frame %d: %d ms, disposal %d, at %d,%d %dx%d, pixels %s", k, fr.delayMs,
                    (int) fr.disposal, fr.left, fr.top, fr.width, fr.height, hash == want ? "exact" : "DIFFER");
      check(hash == want && fr.delayMs == std::atoi(e[1].c_str()) && (int) fr.disposal == std::atoi(e[2].c_str()) &&
                fr.left == std::atoi(e[3].c_str()) && fr.top == std::atoi(e[4].c_str()) &&
                fr.width == std::atoi(e[5].c_str()) && fr.height == std::atoi(e[6].c_str()),
            msg);
    }
    // The still is the first frame.
    check(!got.frames.empty() && got.bitmap.sizeInBytes() == got.frames[0].image.sizeInBytes() &&
              std::memcmp(got.bitmap.data(), got.frames[0].image.data(), got.bitmap.sizeInBytes()) == 0,
          "  the still image is the first frame");
  }
  char msg[120];
  std::snprintf(msg, sizeof(msg), "%d GIF files, %d frames", files, frames);
  check(files >= 6 && frames >= 14, msg);
}

void testGifRefusals() {
  std::printf("GIF: what is refused\n");
  const std::vector<uint8_t> good = readBytes(dataPath("images/local_tables.gif"));
  check(GifDecoder::decode(nullptr, 0).error == "not a GIF file", "nothing is not a GIF");
  {
    std::vector<uint8_t> b = good;
    b[6] = b[7] = b[8] = b[9] = 0xFF; // 65535 x 65535
    check(GifDecoder::decode(b).error == "GIF is larger than this decoder will allocate", "a giant canvas refused");
    b = good;
    b[6] = b[7] = 0;
    check(GifDecoder::decode(b).error == "GIF has no pixels", "a canvas of no width refused");
  }
  {
    // A tight budget: the first frames kept, and the rest named.
    GifDecoder::Options tight;
    tight.maxTotalPixels = 26 * 20 * 2;
    const GifImage some = GifDecoder::decode(good, tight);
    check(some.ok() && some.frames.size() == 2 && !some.warning.empty(), "an animation past the budget keeps its first frames");
    GifDecoder::Options still;
    still.allFrames = false;
    const GifImage one = GifDecoder::decode(good, still);
    check(one.ok() && one.frames.size() == 1, "allFrames off: the first frame only");
  }
  {
    // A frame's rectangle is not bounded by its canvas. A 1x1 screen with a
    // 65535x65535 frame is refused -- it was four gigabytes of indices,
    // allocated and zeroed for 28 bytes -- and a frame larger than its
    // canvas but under the cap counts its own size against the budget.
    auto screenWithFrames = [](int frames, int frameSide) {
      std::vector<uint8_t> b = {'G', 'I', 'F', '8', '9', 'a', 1, 0, 1, 0, 0, 0, 0};
      for (int i = 0; i < frames; ++i) {
        const uint8_t lo = (uint8_t) (frameSide & 0xFF), hi = (uint8_t) (frameSide >> 8);
        const uint8_t frame[] = {0x2C, 0, 0, 0, 0, lo, hi, lo, hi, 0, 2, 1, 0x44, 0};
        b.insert(b.end(), frame, frame + sizeof(frame));
      }
      b.push_back(0x3B);
      return b;
    };
    check(GifDecoder::decode(screenWithFrames(1, 65535)).error == "GIF frame is larger than this decoder will allocate",
          "a frame far larger than its 1x1 canvas refused before its indices are allocated");
    const GifImage clipped = GifDecoder::decode(screenWithFrames(2, 100));
    GifDecoder::Options budget;
    budget.maxTotalPixels = 15000;
    const GifImage counted = GifDecoder::decode(screenWithFrames(2, 100), budget);
    check(clipped.ok() && clipped.frames.size() == 2 && clipped.width == 1 && counted.ok() &&
              counted.frames.size() == 1 && !counted.warning.empty(),
          "frames larger than the canvas are clipped, and counted by their own size against the budget");
  }
  {
    std::vector<uint8_t> half(good.begin(), good.begin() + (long) (good.size() / 2));
    const GifImage cut = GifDecoder::decode(half);
    check(cut.ok() && !cut.frames.empty() && !cut.warning.empty(), "a truncated GIF keeps what arrived, with a warning");
  }
  check(GifFrame{}.displayMs() == 100, "a delay of 0 displays as a browser shows it, 100 ms");
}

// ── The dispatcher ───────────────────────────────────────────────────────────

void testFormats() {
  std::printf("ImageFileFormat: by the bytes, not the name\n");
  const std::vector<uint8_t> png = readBytes(dataPath("png/rgba4.png"));
  const std::vector<uint8_t> jpg = readBytes(dataPath("images/baseline_444.jpg"));
  const std::vector<uint8_t> gif = readBytes(dataPath("images/anim_keep.gif"));
  check(ImageFileFormat::sniff(png) == ImageFormat::Png, "a PNG is sniffed as PNG");
  check(ImageFileFormat::sniff(jpg) == ImageFormat::Jpeg, "a JPEG is sniffed as JPEG");
  check(ImageFileFormat::sniff(gif) == ImageFormat::Gif, "a GIF is sniffed as GIF");
  const DecodedImage dj = ImageFileFormat::decode(jpg);
  const JpegImage direct = JpegDecoder::decode(jpg);
  check(dj.ok() && dj.format == ImageFormat::Jpeg && dj.bitmap.sizeInBytes() == direct.bitmap.sizeInBytes() &&
            std::memcmp(dj.bitmap.data(), direct.bitmap.data(), dj.bitmap.sizeInBytes()) == 0,
        "decode() gives what the JPEG decoder gives");
  const DecodedImage dp = ImageFileFormat::decode(png);
  check(dp.ok() && dp.format == ImageFormat::Png && !dp.bitmap.isEmpty(), "decode() reads a PNG");
  ImageFileFormat::Options all;
  all.allFrames = true;
  const DecodedImage dg = ImageFileFormat::decode(gif, all);
  check(dg.ok() && dg.frames.size() == 3 && dg.loopCount == 2, "decode() gives a GIF's frames when asked");
  check(ImageFileFormat::decode(gif).frames.empty(), "and only its first when not");
  const DecodedImage byPath = ImageFileFormat::loadFile(dataPath("images/orient_6.jpg"));
  check(byPath.ok() && byPath.orientation == 6 && byPath.bitmap.width() == 5 && byPath.bitmap.height() == 11,
        "loadFile reads a file and turns it upright");
  const uint8_t webp[] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P', 'V', 'P', '8', ' '};
  const DecodedImage w = ImageFileFormat::decode(webp, sizeof(webp));
  check(!w.ok() && w.error.find("WebP") != std::string::npos, "a WebP is named in the refusal");
  const uint8_t junk[] = {1, 2, 3, 4, 5, 6, 7, 8};
  check(ImageFileFormat::decode(junk, sizeof(junk)).error == "not an image this can read (PNG, JPEG or GIF)",
        "anything else is not an image");
  check(!ImageFileFormat::loadFile(dataPath("images/no_such_file.png")).ok(), "a missing file is an error");
}

// ── ImageCache ───────────────────────────────────────────────────────────────

void testCache() {
  std::printf("ImageCache\n");
  ImageCache cache;
  int64_t clock = 1000;
  cache.setClock([&clock] { return clock; });
  cache.setCacheTimeout(5000);

  const std::vector<uint8_t> jpg = readBytes(dataPath("images/baseline_420.jpg"));
  ImageCache::Image a = cache.getFromMemory(jpg.data(), jpg.size());
  ImageCache::Image b = cache.getFromMemory(jpg.data(), jpg.size());
  check(a && a == b && cache.numImages() == 1, "the same bytes twice: decoded once, one image shared");
  check(cache.getFromHashCode(ImageCache::hashOf(jpg.data(), jpg.size())) == a, "found again by its hash");

  const std::string path = dataPath("images/baseline_444.jpg");
  ImageCache::Image f1 = cache.getFromFile(path);
  ImageCache::Image f2 = cache.getFromFile(path);
  check(f1 && f1 == f2 && f1->width() == 37, "the same file twice: one image");

  // Held elsewhere: never expires, however old.
  clock += 60000;
  cache.purge();
  check(cache.numImages() == 2, "images somebody holds survive the timeout");
  a.reset();
  b.reset();
  clock += 1000;
  cache.purge();
  check(cache.numImages() == 2, "let go a second ago after an hour held: still here");
  clock += 5001;
  cache.purge();
  check(cache.numImages() == 1, "let go and unasked for past the timeout: released");
  ImageCache::Image again = cache.getFromMemory(jpg.data(), jpg.size());
  check(again && cache.numImages() == 2, "asked for again: decoded again");
  again.reset();
  cache.releaseUnusedImages();
  check(cache.numImages() == 1, "releaseUnusedImages releases only what nobody holds");
  f1.reset();
  f2.reset();
  cache.releaseUnusedImages();
  check(cache.numImages() == 0 && cache.bytesHeld() == 0, "and everything once nobody holds it");

  // The budget: unused images go, oldest asked-for first.
  cache.setMaxBytes(3 * 100 * 100 * 4);
  for (uint64_t id = 1; id <= 3; ++id) {
    cache.addImageToCache(Bitmap(100, 100), id);
    clock += 10;
  }
  check(cache.numImages() == 3 && cache.bytesHeld() == 3u * 40000u, "three images at the budget");
  ImageCache::Image keep = cache.getFromHashCode(1); // now the most recent, and held
  clock += 10;
  cache.addImageToCache(Bitmap(100, 100), 4);
  check(cache.numImages() == 3 && cache.getFromHashCode(2) == nullptr && cache.getFromHashCode(1) == keep,
        "a fourth past the budget evicts the least recently used unheld image");
  cache.setMaxBytes(0);
  check(cache.numImages() == 1 && cache.getFromHashCode(1) == keep, "a budget of nothing keeps only what is held");
  check(!cache.getFromFile(dataPath("images/no_such_file.jpg")), "a file that is not there is null");
  check(!cache.getFromMemory("nonsense", 8), "bytes that are not an image are null");
  check(&ImageCache::shared() == &ImageCache::shared(), "one shared cache per process");
}

// ── ScaledImage and the image types ──────────────────────────────────────────

void testScaledAndTypes() {
  std::printf("ScaledImage and ImageType\n");
  Bitmap art(40, 20);
  art.clear(Colour(200, 50, 50));
  ScaledImage x2(art, 2.0f);
  const Rect b = x2.getScaledBounds();
  check(x2.isValid() && b.w == 20.0f && b.h == 10.0f && x2.getScale() == 2.0f, "@2x art of 40x20 means 20x10");
  {
    Bitmap target(60, 40);
    Graphics g(target);
    x2.drawAt(g, Point(5.0f, 5.0f));
    check(target.pixelAt(5, 5).a == 255 && target.pixelAt(24, 14).a == 255 && target.pixelAt(25, 5).a == 0 &&
              target.pixelAt(5, 15).a == 0,
          "drawn at its logical size: 20x10 at 1x");
  }
  {
    Bitmap target(120, 80);
    Graphics g(target);
    g.addTransform(Transform::scaling(2.0f, 2.0f));
    x2.drawAt(g, Point(5.0f, 5.0f));
    check(target.pixelAt(10, 10).a == 255 && target.pixelAt(49, 29).a == 255 && target.pixelAt(50, 10).a == 0,
          "and on a 2x display, a device pixel for each of its pixels");
  }
  std::vector<ScaledImage> set = {ScaledImage(Bitmap(10, 10), 1.0f), ScaledImage(Bitmap(20, 20), 2.0f),
                                  ScaledImage(Bitmap(30, 30), 3.0f)};
  check(ScaledImage::pickFor(set, 1.0f) == 0 && ScaledImage::pickFor(set, 1.5f) == 1 &&
            ScaledImage::pickFor(set, 2.0f) == 1 && ScaledImage::pickFor(set, 4.0f) == 2,
        "the rendition a density wants: the smallest at least as dense, else the densest");
  check(ScaledImage::pickFor({}, 2.0f) == -1 && !ScaledImage().isValid(), "nothing to pick from: -1");

  NativeImageType native;
  SoftwareImageType software;
  const Bitmap n = native.create(7, 5, false), s = software.create(7, 5);
  bool clear = true;
  for (size_t i = 0; i < n.sizeInBytes(); ++i) clear = clear && n.data()[i] == 0;
  check(n.width() == 7 && n.height() == 5 && s.width() == 7 && clear, "both types create a cleared bitmap");
  check(native.getTypeID() != software.getTypeID(), "and say which they are");
  Bitmap copy = software.convert(art);
  copy.setPixel(0, 0, PremulColour(0, 0, 0, 0));
  check(art.pixelAt(0, 0).a == 255, "convert is a copy the caller may change");
}

// ── Parallelogram ────────────────────────────────────────────────────────────

bool approx(float a, float b, float tolerance = 1e-4f) { return std::fabs(a - b) <= tolerance; }

void testParallelogram() {
  std::printf("Parallelogram\n");
  const Parallelogram r(Rect(10.0f, 20.0f, 30.0f, 40.0f));
  check(approx(r.getBottomRight().x, 40.0f) && approx(r.getBottomRight().y, 60.0f), "a rectangle's fourth corner");
  check(approx(r.getWidth(), 30.0f) && approx(r.getHeight(), 40.0f) && approx(r.area(), 1200.0f), "its edges and area");
  const Point c = r.getRelativePoint(Point(0.5f, 0.5f));
  check(approx(c.x, 25.0f) && approx(c.y, 40.0f), "the relative point (0.5, 0.5) is the centre");
  check(r.contains(Point(10.0f, 20.0f)) && r.contains(Point(39.9f, 59.9f)) && !r.contains(Point(40.0f, 30.0f)) &&
            !r.contains(Point(9.9f, 30.0f)),
        "contains: top and left in, bottom and right out, as Rect");

  // Turned a quarter about the origin: (x, y) -> (-y, x).
  const Parallelogram turned = r.transformedBy(Transform::rotation(1.5707963f));
  const Rect box = turned.getBoundingBox();
  check(approx(box.x, -60.0f, 1e-3f) && approx(box.y, 10.0f, 1e-3f) && approx(box.w, 40.0f, 1e-3f) &&
            approx(box.h, 30.0f, 1e-3f),
        "turned a quarter, its bounding box is the turned rectangle's");
  check(approx(turned.area(), 1200.0f, 1e-2f), "a rotation keeps the area");

  // Sheared: the bounding box takes the lean.
  const Parallelogram leaning(Point(0.0f, 0.0f), Point(10.0f, 0.0f), Point(5.0f, 10.0f));
  const Rect lb = leaning.getBoundingBox();
  check(approx(lb.x, 0.0f) && approx(lb.w, 15.0f) && approx(lb.h, 10.0f), "a sheared one's box spans its lean");
  check(leaning.contains(Point(12.0f, 9.0f)) && !leaning.contains(Point(1.0f, 9.0f)),
        "and contains follows the lean, not the box");
  Point rel;
  check(leaning.toRelative(Point(7.5f, 5.0f), rel) && approx(rel.x, 0.5f) && approx(rel.y, 0.5f), "toRelative inverts getRelativePoint");

  // The transform that puts a picture on it: every corner lands.
  const Rect source(0.0f, 0.0f, 64.0f, 32.0f);
  const Transform t = leaning.transformFrom(source);
  const Point p0 = t.apply(Point(0.0f, 0.0f)), p1 = t.apply(Point(64.0f, 0.0f)), p2 = t.apply(Point(0.0f, 32.0f)),
              p3 = t.apply(Point(64.0f, 32.0f));
  check(approx(p0.x, 0.0f) && approx(p0.y, 0.0f) && approx(p1.x, 10.0f) && approx(p1.y, 0.0f) && approx(p2.x, 5.0f) &&
            approx(p2.y, 10.0f) && approx(p3.x, 15.0f) && approx(p3.y, 10.0f),
        "transformFrom puts the source's corners on the parallelogram's");
  const Parallelogram back = Parallelogram(source).transformedBy(t);
  check(back == leaning, "and the source rectangle, transformed by it, is the parallelogram");
  {
    // Drawn: an image through transformFrom covers the parallelogram.
    Bitmap pic(8, 8);
    pic.clear(Colour(0, 0, 255));
    Bitmap target(40, 30);
    Graphics g(target);
    const Parallelogram where(Point(5.0f, 5.0f), Point(30.0f, 5.0f), Point(10.0f, 25.0f));
    g.drawImageTransformed(pic, where.transformFrom(Rect(0.0f, 0.0f, 8.0f, 8.0f)));
    int inside = 0, insideInk = 0, outsideInk = 0;
    for (int y = 0; y < 30; ++y)
      for (int x = 0; x < 40; ++x) {
        const Point centre((float) x + 0.5f, (float) y + 0.5f);
        Point q;
        const bool deep = where.toRelative(centre, q) && q.x > 0.1f && q.x < 0.9f && q.y > 0.1f && q.y < 0.9f;
        const bool outside = !where.toRelative(centre, q) || q.x < -0.1f || q.x > 1.1f || q.y < -0.1f || q.y > 1.1f;
        const uint8_t a = target.pixelAt(x, y).a;
        if (deep) {
          ++inside;
          if (a == 255) ++insideInk;
        }
        if (outside && a != 0) ++outsideInk;
      }
    check(inside > 200 && insideInk == inside && outsideInk == 0,
          "an image drawn through transformFrom fills the parallelogram and nothing else");
  }
  check(Parallelogram(Point(0, 0), Point(5, 5), Point(10, 10)).isEmpty() && !leaning.isEmpty(),
        "corners on one line make an empty parallelogram");
  check(!Parallelogram(Point(0, 0), Point(5, 5), Point(10, 10)).contains(Point(5, 5)), "which contains nothing");
  const Path outline = leaning.toPath();
  check(outline.points().size() >= 4, "its outline is a path");
  check(leaning.translated(3.0f, 4.0f).topLeft.x == 3.0f && leaning.translated(3.0f, 4.0f).bottomLeft.y == 14.0f,
        "translated moves every corner");
}

// ── SVG text, against resvg ──────────────────────────────────────────────────

Rect geometryBoundsOf(const Path& p) {
  float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
  for (const Point& q : p.points()) {
    x0 = (std::min)(x0, q.x);
    y0 = (std::min)(y0, q.y);
    x1 = (std::max)(x1, q.x);
    y1 = (std::max)(y1, q.y);
  }
  return x1 < x0 ? Rect() : Rect(x0, y0, x1 - x0, y1 - y0);
}

struct InkCompare {
  double iou = 0.0, meanAlpha = 0.0, meanColour = 0.0, mass = 0.0;
  int inkGot = 0, inkWant = 0;
  Rect boxGot, boxWant;
};

Rect inkBox(const Bitmap& b, int y0 = 0, int y1 = -1) {
  if (y1 < 0) y1 = b.height();
  int x0 = b.width(), x1 = -1, top = y1, bottom = -1;
  for (int y = y0; y < y1; ++y)
    for (int x = 0; x < b.width(); ++x)
      if (b.pixelAt(x, y).a >= 128) {
        x0 = (std::min)(x0, x);
        x1 = (std::max)(x1, x);
        top = (std::min)(top, y);
        bottom = (std::max)(bottom, y);
      }
  if (x1 < 0) return {};
  return {(float) x0, (float) top, (float) (x1 - x0 + 1), (float) (bottom - top + 1)};
}

InkCompare compareInk(const Bitmap& got, const Bitmap& want) {
  InkCompare c;
  int both = 0, either = 0, coloured = 0;
  double alpha = 0.0, colour = 0.0, massGot = 0.0, massWant = 0.0;
  for (int y = 0; y < got.height(); ++y)
    for (int x = 0; x < got.width(); ++x) {
      const PremulColour a = got.pixelAt(x, y), b = want.pixelAt(x, y);
      const bool ia = a.a >= 128, ib = b.a >= 128;
      both += ia && ib;
      either += ia || ib;
      c.inkGot += ia;
      c.inkWant += ib;
      alpha += std::abs(a.a - b.a);
      massGot += a.a;
      massWant += b.a;
      if (a.a >= 200 && b.a >= 200) {
        colour += (std::abs(a.r - b.r) + std::abs(a.g - b.g) + std::abs(a.b - b.b)) / 3.0;
        ++coloured;
      }
    }
  c.iou = either ? (double) both / either : 1.0;
  c.mass = massWant > 0.0 ? massGot / massWant : 0.0;
  c.meanAlpha = alpha / (double) (got.width() * got.height());
  c.meanColour = coloured ? colour / coloured : 0.0;
  c.boxGot = inkBox(got);
  c.boxWant = inkBox(want);
  return c;
}

std::shared_ptr<Typeface> loadFace(const std::string& rel) {
  auto face = std::make_shared<Typeface>();
  if (!face->load(readBytes(dataPath(rel)))) return nullptr;
  return face;
}

std::string readText(const std::string& rel) {
  const std::vector<uint8_t> b = readBytes(dataPath(rel));
  return std::string(b.begin(), b.end());
}

void testSvgText() {
  std::printf("SVG text, against resvg\n");
  FontMatcher fonts(FontMatcher::EmbeddedOnly);
  const bool loaded = fonts.registerTypeface(loadFace("svgtext/SonoreText-Regular.ttf")) &&
                      fonts.registerTypeface(loadFace("svgtext/SonoreText-Bold.ttf")) &&
                      fonts.registerTypeface(loadFace("svgtext/SonoreText-Italic.ttf")) &&
                      fonts.registerTypeface(loadFace("svgtext/SonoreTextVar.ttf"));
  check(loaded, "the four test faces register, named by their own name tables");
  fonts.setGenericFamily("serif", "Sonore Text");
  fonts.setDefaultFamily("Sonore Text");

  std::vector<std::vector<std::string>> lines;
  {
    std::ifstream in(dataPath("svgtext/svgtext_expected.txt"));
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty() || line[0] == '#') continue;
      std::istringstream words(line);
      std::vector<std::string> w;
      std::string word;
      while (words >> word) w.push_back(word);
      lines.push_back(w);
    }
  }
  check(lines.size() >= 8, "svgtext_expected.txt lists the fixtures");
  for (const auto& f : lines) {
    if (f.size() < 5 || f[0] != "svg") continue;
    const Drawable d = Drawable::parse(readText("svgtext/" + f[1]), fonts);
    const int w = std::atoi(f[3].c_str()), h = std::atoi(f[4].c_str());
    Bitmap got(w, h);
    {
      Graphics g(got);
      d.draw(g, Rect(0.0f, 0.0f, (float) w, (float) h));
    }
    const PngImage want = PngDecoder::decode(readBytes(dataPath("svgtext/" + f[2])));
    const InkCompare c = compareInk(got, want.bitmap);
    char msg[320];
    std::snprintf(msg, sizeof(msg),
                  "%-15s ink %4d vs %4d, IoU %.2f, coverage %.3f of resvg's, |alpha diff| %.2f, colour %.1f, "
                  "box %.0f,%.0f %.0fx%.0f vs %.0f,%.0f %.0fx%.0f",
                  f[1].c_str(), c.inkGot, c.inkWant, c.iou, c.mass, c.meanAlpha, c.meanColour, c.boxGot.x, c.boxGot.y,
                  c.boxGot.w, c.boxGot.h, c.boxWant.x, c.boxWant.y, c.boxWant.w, c.boxWant.h);
    const bool boxClose = std::fabs(c.boxGot.x - c.boxWant.x) <= 1.0f && std::fabs(c.boxGot.y - c.boxWant.y) <= 1.0f &&
                          std::fabs(c.boxGot.right() - c.boxWant.right()) <= 1.0f &&
                          std::fabs(c.boxGot.bottom() - c.boxWant.bottom()) <= 1.0f;
    // The glyphs' stems are under two pixels wide at these sizes, so the two
    // rasterisers' antialiasing moves pixels across the half-coverage line
    // that IoU counts; the total coverage, the ink's bounds to a pixel and
    // the mean difference are what say the same glyphs landed in the same
    // places.
    check(want.ok() && c.inkWant > 0 && std::fabs(c.mass - 1.0) <= 0.06 && c.iou >= 0.75 && c.meanAlpha <= 3.0 &&
              c.meanColour <= 12.0 && boxClose,
          msg);
  }

  // What the parse kept of the text, as DrawableText.
  {
    const Drawable d = Drawable::parse(readText("svgtext/basic.svg"), fonts);
    const Drawable ws = Drawable::parse(readText("svgtext/whitespace.svg"), fonts);
    check(ws.numTexts() == 5 && ws.textAt(0).getText() == "Two words here" && ws.textAt(1).getText() == "  Kept   spaces" &&
              ws.textAt(2).getText() == "Line break " && ws.textAt(3).getText() == "and" &&
              ws.textAt(4).getText() == " span",
          "whitespace: collapsed and trimmed by default, a line break a space, kept where preserved");
    // Across a span's edge the collapse is the text's, not the span's: the
    // span's own leading space goes when the text before it ended in one.
    const Drawable edge = Drawable::parse(
        "<svg viewBox='0 0 10 10'><text font-family='Sonore Text'>a <tspan> b </tspan> c</text></svg>", fonts);
    check(edge.numTexts() == 3 && edge.textAt(0).getText() == "a " && edge.textAt(1).getText() == "b " &&
              edge.textAt(2).getText() == "c",
          "spaces collapse across span edges as one string");
    check(d.numTexts() == 1 && d.textAt(0).getText() == "Hello, SVG 42!" &&
              d.textAt(0).getMatchedFamily() == "Sonore Text" && std::fabs(d.textAt(0).getFont().height() - 22.0f) < 1e-3f,
          "a <text> is kept as a DrawableText: its words, its face, its size");
  }
  {
    const Drawable d = Drawable::parse(readText("svgtext/weights.svg"), fonts);
    int axis700 = -1, axis300 = -1, bold = -1, six = -1, regular = -1;
    for (int i = 0; i < d.numTexts(); ++i) {
      const std::string& t = d.textAt(i).getText();
      if (t == "Axis 700") axis700 = i;
      if (t == "Axis 300") axis300 = i;
      if (t == "Bold face") bold = i;
      if (t == "Six hundred is bold") six = i;
      if (t == "Regular 400") regular = i;
    }
    bool onAxis = false;
    if (axis700 >= 0)
      for (const auto& v : d.textAt(axis700).getFont().variations())
        onAxis = onAxis || (v.first == Typeface::tagOf("wght") && v.second == 700.0f);
    check(axis700 >= 0 && axis300 >= 0 && onAxis && d.textAt(axis700).getMatchedFamily() == "Sonore Text Var",
          "font-weight 700 on a variable family is set on its wght axis");
    check(bold >= 0 && six >= 0 && regular >= 0 &&
              d.textAt(bold).getFont().typeface() == d.textAt(six).getFont().typeface() &&
              d.textAt(bold).getFont().typeface() != d.textAt(regular).getFont().typeface(),
          "bold, and 600, find the family's 700 face by CSS's rule");
  }
  {
    // The matcher on its own: CSS's weight order, the style, and nothing
    // from the system for a matcher that looks only at what it was given.
    const FontMatch regular = fonts.match("Sonore Text", 400), w500 = fonts.match("Sonore Text", 500),
                    w650 = fonts.match("Sonore Text", 650), w300 = fonts.match("Sonore Text", 300),
                    it = fonts.match("'Sonore Text'", 700, true), arial = fonts.match("Arial, Helvetica", 400);
    check(regular.weight == 400 && w500.weight == 400 && w650.weight == 700 && w300.weight == 400,
          "weights 400, 500 and 300 find the 400 face; 650 finds 700");
    check(it.italic && it.weight == 400, "italic is chosen before weight: the italic 400 over the roman 700");
    check(arial.isValid() && arial.family == "Sonore Text" && !arial.fromSystem && !arial.fallback,
          "an embedded-only matcher answers a missing family with its own face, never the system's");
    const FontMatch v = fonts.match("Sonore Text Var", 250);
    check(v.weightOnAxis && v.weight == 250, "a variable face matches any weight its axis reaches, exactly");
    const std::vector<std::string> names = fonts.familyNames();
    check(std::find(names.begin(), names.end(), "Sonore Text") != names.end() &&
              std::find(names.begin(), names.end(), "Sonore Text Var") != names.end(),
          "familyNames lists the registered families");
  }
  {
    // DrawableText on its own: in a box, justified, turned with it.
    DrawableText t;
    t.setText("Abc");
    t.setFont(fonts.match("Sonore Text").font(20.0f));
    t.setColour(Colour(0, 0, 0));
    t.setJustification(Justify::Right);
    t.setBoundingBox(Parallelogram(Rect(10.0f, 10.0f, 100.0f, 30.0f)));
    Bitmap b(130, 50);
    {
      Graphics g(b);
      t.draw(g);
    }
    const Rect ink = inkBox(b);
    check(ink.w > 10.0f && ink.right() <= 110.5f && ink.right() >= 100.0f && ink.y >= 10.0f && ink.bottom() <= 40.0f,
          "a DrawableText right-justified ends at its box's right edge");
    t.setBoundingBox(Parallelogram(Rect(10.0f, 10.0f, 100.0f, 30.0f))
                         .transformedBy(Transform::rotationAbout(1.5707963f, Point(10.0f, 10.0f))));
    const Rect turned = geometryBoundsOf(t.toPath());
    check(turned.w < turned.h && turned.right() <= 10.5f, "and turned a quarter with its box, it runs downwards");
  }
  {
    // A font-size in points is 4/3 of one in pixels: 18pt draws as 24px.
    const char* kPt = "<svg viewBox='0 0 120 40' width='120' height='40'><text x='4' y='30' "
                      "font-family='Sonore Text' font-size='18pt'>Size</text></svg>";
    const char* kPx = "<svg viewBox='0 0 120 40' width='120' height='40'><text x='4' y='30' "
                      "font-family='Sonore Text' font-size='24'>Size</text></svg>";
    Bitmap a(120, 40), b(120, 40);
    {
      Graphics g(a);
      Drawable::parse(kPt, fonts).draw(g, Rect(0.0f, 0.0f, 120.0f, 40.0f));
    }
    {
      Graphics g(b);
      Drawable::parse(kPx, fonts).draw(g, Rect(0.0f, 0.0f, 120.0f, 40.0f));
    }
    check(a.sizeInBytes() == b.sizeInBytes() && std::memcmp(a.data(), b.data(), a.sizeInBytes()) == 0 &&
              inkBox(a).w > 20.0f,
          "font-size 18pt draws exactly as 24px");
  }
  {
    // Ink where the anchors put it.
    const Drawable d = Drawable::parse(readText("svgtext/anchor.svg"), fonts);
    Bitmap out(180, 90);
    {
      Graphics g(out);
      d.draw(g, Rect(0.0f, 0.0f, 180.0f, 90.0f));
    }
    const Rect start = inkBox(out, 0, 30), middle = inkBox(out, 30, 56), end = inkBox(out, 56, 90);
    char msg[200];
    std::snprintf(msg, sizeof(msg), "text-anchor at x = 90: start begins at %.0f, middle centres on %.1f, end ends at %.0f",
                  start.x, middle.x + middle.w * 0.5f, end.right());
    check(std::fabs(start.x - 90.0f) <= 2.0f && std::fabs(middle.x + middle.w * 0.5f - 90.0f) <= 3.0f &&
              std::fabs(end.right() - 90.0f) <= 2.0f,
          msg);
  }
  {
    // Text that names nothing a matcher can find draws nothing rather than
    // failing, and a document's text is bounded.
    FontMatcher empty(FontMatcher::EmbeddedOnly);
    const Drawable none = Drawable::parse("<svg viewBox='0 0 10 10'><text x='1' y='8'>A</text></svg>", empty);
    check(none.numTexts() == 0 && none.isEmpty(), "with no face at all, text draws nothing rather than failing");
    std::string big = "<svg viewBox='0 0 10 10'><text>";
    big.append(250000, 'A');
    big += "</text></svg>";
    const Drawable bounded = Drawable::parse(big, fonts);
    check(!bounded.error().empty(), "a quarter of a million characters is past the limit, and says so");
    // A position list is read again at every layout of its text, so a long
    // one used many times is paid for from the same budget: this was 3.4 s
    // of parsing, unbounded in the number of uses, and drew every copy.
    std::string lists = "<svg viewBox='0 0 10 10' xmlns:xlink='http://www.w3.org/1999/xlink'><defs><text id='t' x='";
    for (int i = 0; i < 100000; ++i) lists += "0 ";
    lists += "'>A</text></defs>";
    for (int i = 0; i < 200; ++i) lists += "<use xlink:href='#t'/>";
    lists += "</svg>";
    const Drawable listed = Drawable::parse(lists, fonts);
    check(!listed.error().empty() && listed.numTexts() > 0 && listed.numTexts() < 200,
          "a 200 KB x list on a text used 200 times exhausts the character budget, and says so");
  }
  if (systemTypeface()) {
    // The machine's own fonts: only that some are found, and that a family
    // the index found resolves to a face of the system's.
    FontMatcher system;
    const size_t faces = system.systemFaceCount();
    const std::vector<std::string> names = system.familyNames();
    char msg[160];
    std::snprintf(msg, sizeof(msg), "this machine's font folders: %zu faces, %zu names", faces, names.size());
    check(faces > 0 && !names.empty(), msg);
    if (!names.empty()) {
      const FontMatch first = system.match("\"" + names.front() + "\"", 400);
      check(first.isValid() && first.fromSystem, "a family the index found matches a face of the system's");
    }
    // The files are read on several threads and the faces kept in the walk's
    // order, which is the order a family's faces are compared in: a second
    // index is the same, and every family resolves to the same face in both.
    FontMatcher again;
    bool same = again.systemFaceCount() == faces && again.familyNames() == names;
    for (size_t i = 0; same && i < names.size(); ++i)
      for (int weight : {300, 400, 700}) {
        const FontMatch a = system.match("\"" + names[i] + "\"", weight, (i & 1) != 0);
        const FontMatch b = again.match("\"" + names[i] + "\"", weight, (i & 1) != 0);
        same = same && a.family == b.family && a.weight == b.weight && a.italic == b.italic &&
               a.isValid() == b.isValid() && (!a.isValid() || a.typeface->numGlyphs() == b.typeface->numGlyphs());
      }
    check(same, "two indexes of the machine's fonts are the same, face for face");
  }

  // What reads the font files: every job once, the calling thread working
  // through them too; a job that throws on a helper is done again on the
  // calling thread, where its exception reaches the caller.
  {
    const std::thread::id caller = std::this_thread::get_id();
    std::vector<int> runs(1000, 0);
    std::vector<std::thread::id> by(1000);
    sonore::gfx::detail::fontmatch::runShared(runs.size(), 8, [&](size_t i) {
      if (i % 97 == 5 && std::this_thread::get_id() != caller) throw std::runtime_error("on a helper");
      ++runs[i];
      by[i] = std::this_thread::get_id();
    });
    bool once = true, redone = true;
    for (size_t i = 0; i < runs.size(); ++i) {
      once = once && runs[i] == 1;
      if (i % 97 == 5) redone = redone && by[i] == caller;
    }
    check(once && redone, "1000 jobs on 8 threads: each runs once, and one that throws on a helper is redone here");
    bool reached = false;
    try {
      sonore::gfx::detail::fontmatch::runShared(40, 4, [](size_t i) {
        if (i == 17) throw std::runtime_error("always");
      });
    } catch (const std::runtime_error&) {
      reached = true;
    }
    check(reached, "a job that throws wherever it runs throws to the caller");
  }

#if defined(_WIN32)
  // On Windows the index reads a font file's header, directory and tables
  // where they are instead of mapping it. Each answer must be the one the
  // whole file gives, for any file: a real font cut short and flipped near
  // its directory, and collections and directories made up of random
  // numbers, each written out and asked the same questions both ways.
  {
    namespace fm = sonore::gfx::detail::fontmatch;
    std::vector<uint8_t> real;
    if (FILE* f = std::fopen("C:/Windows/Fonts/arial.ttf", "rb")) {
      real.resize(4096);
      real.resize(std::fread(real.data(), 1, real.size(), f));
      std::fclose(f);
    }
    uint32_t seed = 12345u;
    auto rnd = [&seed]() {
      seed = seed * 1664525u + 1013904223u;
      return seed >> 8;
    };
    const uint32_t tags[] = {0x6E616D65u, 0x4F532F32u, 0x68656164u, 0x66766172u, 0x636D6170u, 0x12345678u};
    auto put16 = [](std::vector<uint8_t>& b, size_t at, uint32_t v) {
      if (at + 2 > b.size()) return;
      b[at] = (uint8_t) (v >> 8);
      b[at + 1] = (uint8_t) v;
    };
    auto put32 = [&put16](std::vector<uint8_t>& b, size_t at, uint32_t v) {
      put16(b, at, v >> 16);
      put16(b, at + 2, v & 0xFFFFu);
    };
    // A table directory at `base`: records naming the index's tags (and
    // others), pointing inside the file, past it, and across its end.
    auto directory = [&](std::vector<uint8_t>& b, uint32_t base) {
      const uint32_t tables = 1 + rnd() % 12, size = (uint32_t) b.size();
      put16(b, base + 4, rnd() % 9 == 0 ? 60000u : tables);
      for (uint32_t i = 0; i < tables; ++i) {
        const size_t rec = (size_t) base + 12 + (size_t) i * 16;
        put32(b, rec, rnd() % 4 == 0 ? rnd() : tags[rnd() % 6]);
        const uint32_t off = rnd() % 5 == 0 ? rnd() : rnd() % (size + 1);
        put32(b, rec + 8, off);
        put32(b, rec + 12, rnd() % 6 == 0 ? rnd() : rnd() % (size / 2 + 2));
      }
    };
    const std::string path = sonoretest::tempPath("font-reads.ttf");
    int cases = 0, agreed = 0, found = 0;
    for (int c = 0; c < 600; ++c) {
      std::vector<uint8_t> bytes;
      const int kind = c % 4;
      if (kind == 0 && !real.empty()) {
        bytes = real;
        bytes.resize(1 + rnd() % bytes.size());
        for (int k = (int) (rnd() % 6); k > 0; --k) bytes[rnd() % bytes.size()] = (uint8_t) rnd();
      } else if (kind == 1 || kind == 2) {
        bytes.resize(32 + rnd() % 1500);
        for (uint8_t& b : bytes) b = (uint8_t) rnd();
        if (kind == 1) {
          directory(bytes, 0);
        } else {
          // A collection: its faces' directories wherever the header says.
          std::memcpy(bytes.data(), "ttcf", 4);
          const uint32_t faces = 1 + rnd() % 4;
          put32(bytes, 8, rnd() % 7 == 0 ? 100000u : faces);
          for (uint32_t i = 0; i < faces; ++i) {
            const uint32_t base = 28 + rnd() % (uint32_t) (bytes.size() - 28);
            put32(bytes, 12 + i * 4, base);
            directory(bytes, base);
          }
        }
        // Cut short, sometimes inside a directory.
        if (rnd() % 3 == 0) bytes.resize(1 + rnd() % bytes.size());
      } else {
        bytes.resize(rnd() % 600);
        for (uint8_t& b : bytes) b = (uint8_t) (rnd() % 7 == 0 ? rnd() : rnd() % 24);
        if (bytes.size() >= 12 && c % 3 == 0) std::memcpy(bytes.data(), "ttcf", 4);
      }
      if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
      }
      ++cases;
      fm::WholeFontFile whole{bytes.data(), bytes.size()};
      std::vector<uint32_t> wantFaces = bytes.empty() ? std::vector<uint32_t>() : whole.faces();
      fm::FontFileReads reads(path);
      bool same = reads.ok() == !bytes.empty();
      const std::vector<uint32_t> gotFaces = reads.ok() ? reads.faces() : std::vector<uint32_t>();
      same = same && gotFaces == wantFaces;
      std::vector<uint32_t> bases = wantFaces;
      bases.push_back((uint32_t) (rnd() % 700));
      for (uint32_t base : bases)
        for (uint32_t tag : tags) {
          const uint8_t *pa = nullptr, *pb = nullptr;
          uint32_t na = 0, nb = 0;
          const bool a = !bytes.empty() && whole.table(base, tag, &pa, &na);
          const bool b = reads.ok() && reads.table(base, tag, &pb, &nb);
          same = same && a == b && (!a || (na == nb && (na == 0 || std::memcmp(pa, pb, na) == 0)));
          found += a ? 1 : 0;
        }
      if (same) ++agreed;
    }
    char msg[200];
    std::snprintf(msg, sizeof(msg),
                  "a font file's faces and tables read in place are the whole file's (%d of %d files, %d tables "
                  "found)",
                  agreed, cases, found);
    check(agreed == cases && found > 100, msg);
  }
#endif
}

std::vector<std::vector<std::string>> readExpected(const std::string& path) {
  std::vector<std::vector<std::string>> out;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream words(line);
    std::vector<std::string> w;
    std::string word;
    while (words >> word) w.push_back(word);
    out.push_back(w);
  }
  return out;
}

} // namespace

int main() {
  const auto expected = readExpected(dataPath("images/images_expected.txt"));
  check(!expected.empty(), "images_expected.txt is there");
  testJpegFixtures(expected);
  testJpegRefusals();
  testJpegEncoder();
  testGifFixtures(expected);
  testGifRefusals();
  testFormats();
  testCache();
  testScaledAndTypes();
  testParallelogram();
  testSvgText();
  std::printf(g_failures == 0 ? "IMAGES TESTS PASSED\n" : "IMAGES TESTS FAILED (%d)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
