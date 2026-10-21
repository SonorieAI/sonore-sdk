// SPDX-License-Identifier: Apache-2.0
// The drawing API fed what a plugin's paint code computes when it goes wrong.
//
// A level meter takes the log of a silent block and draws to y = -inf. A
// knob's arc ends at an angle of NaN. A panel is laid out at zero width, a
// stroke is drawn at width 1e30, a transform scales by zero, a gradient's two
// points coincide, a font is asked for at size -3. The SVG fuzz reaches the
// rasteriser through the parser; this reaches it the way paint code does,
// through Graphics: random rectangles, rounded rectangles, ellipses, arcs,
// lines, paths of every verb, strokes of every join and cap, solid colours,
// linear and radial gradients with stops, tiled images, images drawn into
// rectangles and through transforms, nested save/restore with clips and
// transforms, text, and the colour arithmetic a LookAndFeel does -- fades,
// mixes, shades -- with every number drawn from a list that is mostly
// ordinary and sometimes NaN, an infinity, a huge value, a negative or zero.
// Properties: no crash, no read or write out of bounds (run it under ASan),
// and no hang -- a watchdog stops the run if one call takes ten seconds
// (two minutes under ASan or in a debug build).
//
// Every random number is drawn into a named local before the call that uses
// it. The order a compiler evaluates function arguments in is unspecified --
// MSVC changes it with the inlining around the call -- so Rect(num(), num(),
// ...) drew a different shape from the same seed in a different build, and a
// seed that failed could not be replayed. With --hash the run prints a hash
// of every frame's pixels: the same seed, the same calls, so two builds of
// the rasteriser can be compared pixel for pixel.
//
//   fuzz_graphics [--steps N] [--seed S] [--hash]
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
#include <thread>
#include <vector>

#include <sonore/gfx/bitmap.h>
#include <sonore/gfx/font.h>
#include <sonore/gfx/gradient.h>
#include <sonore/gfx/graphics.h>
#include <sonore/gfx/path.h>
#include <sonore/gfx/stroke.h>
#include <sonore/gfx/truetype.h>

using namespace sonore::gfx;

namespace {

std::shared_ptr<Typeface> loadAnyTypeface() {
  const char* candidates[] = {
#if defined(_WIN32)
      "C:/Windows/Fonts/arial.ttf", "C:/Windows/Fonts/segoeui.ttf",
#endif
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
      "/System/Library/Fonts/Helvetica.ttc",
  };
  for (const char* path : candidates) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) continue;
    std::vector<uint8_t> bytes;
    uint8_t chunk[65536];
    size_t got = 0;
    while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0) bytes.insert(bytes.end(), chunk, chunk + got);
    std::fclose(f);
    auto face = std::make_shared<Typeface>();
    if (face->load(std::move(bytes))) return face;
  }
  return nullptr;
}

// How long one call may take before it is called a hang. Some calls are
// slow and finish: a stroke 178 pixels wide with round joins, along arcs
// given in degrees where radians were meant -- fifty turns each, a circle at
// every flattened point -- takes over a second in a release build. Under ASan
// or in a debug build everything runs ten to twenty times slower, and the
// limit with it; a call that does not return at all is caught either way.
#if !defined(__has_feature)
#define SONORE_HAS_FEATURE(x) 0
#else
#define SONORE_HAS_FEATURE(x) __has_feature(x)
#endif
#if defined(__SANITIZE_ADDRESS__) || SONORE_HAS_FEATURE(address_sanitizer) || !defined(NDEBUG)
constexpr long long kHangMs = 120000;
#else
constexpr long long kHangMs = 10000;
#endif

std::atomic<long long> g_deadline{0};
std::atomic<size_t> g_step{0};
std::atomic<const char*> g_what{""};
std::atomic<bool> g_done{false};

long long nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

} // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  size_t steps = 20000;
  uint32_t seed = 0x61F7u;
  bool hashPixels = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--steps") == 0 && i + 1 < argc) steps = std::strtoull(argv[++i], nullptr, 10);
    if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) seed = (uint32_t) std::strtoul(argv[++i], nullptr, 10);
    if (std::strcmp(argv[i], "--hash") == 0) hashPixels = true;
  }

  // A call that has not returned in kHangMs is a hang, named and stopped.
  std::thread watchdog([] {
    while (!g_done.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      const long long d = g_deadline.load();
      if (d && nowMs() > d) {
        std::printf("  FAIL HANG at step %zu in %s\n", g_step.load(), g_what.load());
        std::fflush(stdout);
        std::_Exit(3);
      }
    }
  });

  auto face = loadAnyTypeface();
  std::mt19937 rng(seed);
  auto pick = [&](uint32_t n) { return n ? rng() % n : 0u; };
  auto chance = [&](int percent) { return (int) pick(100) < percent; };
  const float inf = std::numeric_limits<float>::infinity();
  const float nan = std::numeric_limits<float>::quiet_NaN();
  // Mostly a coordinate on or near a 256-pixel canvas; sometimes anything.
  auto num = [&]() -> float {
    if (chance(85)) {
      const float whole = (float) pick(400);
      return whole - 72.0f + (float) pick(100) / 100.0f;
    }
    const float wild[] = {nan, inf, -inf, 0.0f, -0.0f, 1e30f, -1e30f, 3e6f, -3e6f, 1e-30f, 0.5f, -1.0f};
    return wild[pick(sizeof(wild) / sizeof(wild[0]))];
  };
  auto size = [&]() -> float {
    if (chance(85)) return (float) pick(200) + 0.25f;
    const float wild[] = {nan, inf, 0.0f, -5.0f, 1e30f, 1e-20f};
    return wild[pick(sizeof(wild) / sizeof(wild[0]))];
  };
  // Each draw in its own statement: see the note at the top.
  auto point = [&]() {
    const float x = num();
    const float y = num();
    return Point{x, y};
  };
  auto rect = [&]() {
    const float x = num();
    const float y = num();
    const float w = size();
    const float h = size();
    return Rect(x, y, w, h);
  };
  auto colour = [&]() {
    const auto r = (uint8_t) pick(256);
    const auto g = (uint8_t) pick(256);
    const auto b = (uint8_t) pick(256);
    const auto a = (uint8_t) pick(256);
    return Colour(r, g, b, a);
  };
  auto factor = [&](float base, int tenths) { return chance(80) ? base + (float) pick((uint32_t) tenths) / 10.0f : num(); };

  auto randomPath = [&]() {
    Path p;
    const Point start = point();
    p.moveTo(start.x, start.y);
    for (int v = 1 + (int) pick(8); v > 0; --v) {
      switch (pick(6)) {
        case 0: {
          const Point to = point();
          p.lineTo(to.x, to.y);
          break;
        }
        case 1: {
          const Point c = point();
          const Point to = point();
          p.quadTo(c.x, c.y, to.x, to.y);
          break;
        }
        case 2: {
          const Point c1 = point();
          const Point c2 = point();
          const Point to = point();
          p.cubicTo(c1.x, c1.y, c2.x, c2.y, to.x, to.y);
          break;
        }
        case 3: {
          const Rect r = rect();
          const float from = num();
          const float to = num();
          p.addArc(r, from, to, chance(50));
          break;
        }
        case 4: p.close(); break;
        default: {
          const Rect r = rect();
          p.addRoundedRect(r, size());
          break;
        }
      }
    }
    return p;
  };
  auto amount = [&]() { return chance(80) ? (float) pick(101) / 100.0f : num(); };

  Bitmap image(37, 21);
  for (int y = 0; y < image.height(); ++y)
    for (int x = 0; x < image.width(); ++x) image.setPixel(x, y, PremulColour::from(Colour((uint8_t) (x * 7), (uint8_t) (y * 11), 99, (uint8_t) (128 + x))));
  const char* strings[] = {"Gain", "-6.0 dB", "", "\xC3\xA9t\xC3\xA9 \xE4\xB8\xAD", "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW",
                           "a\nb\tc", "\xF0\x9F\x8E\xB5", "\xFF\xFE broken utf-8 \xC3",
                           // Shaped: Arabic with its marks, Hebrew, a bidi mix with an override
                           u8"بِسْمِ اللَّهِ 12",
                           u8"שָׁלוֹם (Gain)",
                           u8"Preset الصوت 2.5k ‮abc‬ é"};

  size_t calls = 0;
  uint64_t hash = 1469598103934665603ull; // FNV-1a over every frame's pixels
  for (size_t step = 0; step < steps; ++step) {
    // A canvas of its own size now and then, the empty one included.
    static Bitmap canvas(256, 256);
    if (chance(2)) {
      const int w = (int) pick(300);
      const int h = (int) pick(300);
      canvas = Bitmap(w, h);
    }
    Graphics g(canvas);
    const int ops = 1 + (int) pick(12);
    for (int op = 0; op < ops; ++op) {
      g_step = step;
      g_deadline = nowMs() + kHangMs;
      ++calls;
      switch (pick(24)) {
        case 0: {
          g_what = "setColour";
          Colour c = colour();
          if (chance(30)) c = c.withMultipliedAlpha(amount());
          if (chance(30)) {
            const Colour other = colour();
            c = c.interpolatedTo(other, amount());
          }
          if (chance(30)) c = chance(50) ? c.brightened(amount()) : c.darkened(amount());
          g.setColour(c);
          break;
        }
        case 1: {
          g_what = "setGradient";
          const Colour c0 = colour();
          const Point p0 = point();
          const Colour c1 = colour();
          const Point p1 = point();
          ColourGradient grad(c0, p0, c1, p1, chance(40));
          for (int s = (int) pick(4); s > 0; --s) {
            const float at = chance(80) ? (float) pick(101) / 100.0f : num();
            grad.addStop(at, colour());
          }
          g.setGradient(grad);
          break;
        }
        case 2: {
          g_what = "setTiledImageFill";
          const Point origin = point();
          g.setTiledImageFill(image, origin, factor(0.5f, 40));
          break;
        }
        case 3: {
          g_what = "addTransform";
          const int kind = (int) pick(4);
          if (kind == 0) {
            const Point by = point();
            g.addTransform(Transform::translation(by.x, by.y));
          } else if (kind == 1) {
            const float sx = factor(0.25f, 30);
            const float sy = factor(0.25f, 30);
            g.addTransform(Transform::scaling(sx, sy));
          } else if (kind == 2) {
            g.addTransform(Transform::rotation(num()));
          } else {
            const float angle = num();
            g.addTransform(Transform::rotationAbout(angle, point()));
          }
          break;
        }
        case 4: g_what = "clipTo"; g.clipTo(rect()); break;
        case 5: g_what = "save/restore"; if (chance(50)) g.save(); else g.restore(); break;
        case 6: g_what = "fillRect"; g.fillRect(rect()); break;
        case 7: {
          g_what = "fillRoundedRect";
          const Rect r = rect();
          g.fillRoundedRect(r, size());
          break;
        }
        case 8: g_what = "fillEllipse"; g.fillEllipse(rect()); break;
        case 9: {
          g_what = "fill/stroke a path";
          const Path p = randomPath();
          if (chance(50)) {
            g.fillPath(p, chance(50) ? FillRule::NonZero : FillRule::EvenOdd);
          } else {
            StrokeStyle style;
            const float width = size();
            style.width = width * (chance(80) ? 0.1f : 1.0f);
            style.join = (LineJoin) pick(3);
            style.cap = (LineCap) pick(3);
            style.miterLimit = chance(80) ? 1.0f + (float) pick(10) : num();
            g.strokePath(p, style);
          }
          break;
        }
        case 10: {
          g_what = "drawLine";
          const Point a = point();
          const Point b = point();
          g.drawLine(a.x, a.y, b.x, b.y, size() * 0.1f);
          break;
        }
        case 11: {
          g_what = "drawRect";
          const Rect r = rect();
          g.drawRect(r, size() * 0.1f);
          break;
        }
        case 12: {
          g_what = "drawRoundedRect";
          const Rect r = rect();
          const float radius = size();
          g.drawRoundedRect(r, radius, size() * 0.1f);
          break;
        }
        case 13: {
          g_what = "drawEllipse";
          const Rect r = rect();
          g.drawEllipse(r, size() * 0.1f);
          break;
        }
        case 14: g_what = "drawImage"; g.drawImage(image, rect()); break;
        case 15: {
          g_what = "drawImageTransformed";
          const float angle = num();
          const float sx = factor(0.5f, 40);
          const float sy = factor(0.5f, 40);
          const Point by = point();
          const Transform t = Transform::rotation(angle).then(Transform::scaling(sx, sy));
          g.drawImageTransformed(image, t.then(Transform::translation(by.x, by.y)));
          break;
        }
        case 16: g_what = "drawImageAt"; g.drawImageAt(image, point()); break;
        case 17: g_what = "fillAll"; if (chance(10)) g.fillAll(colour()); break;
        case 18: {
          g_what = "clipToPath";
          const Path p = randomPath();
          g.clipToPath(p, chance(50) ? FillRule::NonZero : FillRule::EvenOdd);
          break;
        }
        case 19: {
          // Begun and ended in any order, nested, left open: the frame's
          // Graphics closes whatever is still open when it goes.
          const int which = (int) pick(4);
          if (which == 0) {
            g_what = "beginLayer";
            g.beginLayer(amount());
          } else if (which == 1) {
            g_what = "endLayer";
            g.endLayer();
          } else if (which == 2) {
            g_what = "beginMask";
            g.beginMask();
          } else {
            g_what = "endMask";
            g.endMask(chance(50) ? Graphics::MaskFrom::Alpha : Graphics::MaskFrom::Luminance);
          }
          break;
        }
        case 20: {
          g_what = "dashed stroke";
          const Path p = randomPath();
          StrokeStyle style;
          style.width = size() * 0.1f;
          style.join = (LineJoin) pick(3);
          style.cap = (LineCap) pick(3);
          for (int d = (int) pick(6); d > 0; --d) {
            const float length = chance(85) ? (float) pick(40) / 4.0f : num();
            style.dashes.push_back(length);
          }
          style.dashOffset = chance(70) ? (float) pick(20) : num();
          g.strokePath(p, style);
          break;
        }
        case 21: {
          g_what = "gradient: spread, focal, an affine map";
          const Colour c0 = colour();
          const Point p0 = point();
          const Colour c1 = colour();
          const Point p1 = point();
          ColourGradient grad(c0, p0, c1, p1, chance(60));
          grad.setSpread((GradientSpread) pick(3));
          if (chance(60)) grad.setFocalPoint(point());
          if (chance(60)) {
            const float a = factor(0.5f, 20);
            const float b = chance(70) ? 0.0f : num() * 0.01f;
            const float c = chance(70) ? 0.0f : num() * 0.01f;
            const float d = factor(0.5f, 20);
            const Point by = point();
            grad = grad.transformed(Transform(a, b, c, d, by.x, by.y));
          }
          if (chance(30)) grad.clearStops();
          for (int s = (int) pick(3); s > 0; --s) grad.addStop(amount(), colour());
          g.setGradient(grad);
          break;
        }
        default: {
          if (!face) break;
          g_what = "text";
          const Font font(face, chance(85) ? 6.0f + (float) pick(40) : num());
          const char* s = strings[pick(sizeof(strings) / sizeof(strings[0]))];
          const int how = (int) pick(3);
          if (how == 0) {
            const Point at = point();
            font.draw(g, s, at.x, at.y);
          } else {
            const Rect r = rect();
            const auto justify = (Justify) pick(3);
            if (how == 1) font.drawIn(g, s, r, justify);
            else font.drawWrapped(g, s, r, justify);
          }
          break;
        }
      }
      g_deadline = 0;
    }
    if (hashPixels)
      for (int y = 0; y < canvas.height(); ++y)
        for (int x = 0; x < canvas.width(); ++x) {
          const PremulColour c = canvas.pixelAt(x, y);
          hash = (hash ^ ((uint64_t) c.r | ((uint64_t) c.g << 8) | ((uint64_t) c.b << 16) | ((uint64_t) c.a << 24))) *
                 1099511628211ull;
        }
  }
  g_done = true;
  watchdog.join();
  if (hashPixels) std::printf("pixel hash %016llx\n", (unsigned long long) hash);
  std::printf("Sonore SDK graphics fuzz -- %zu frames, %zu calls, seed %u: nothing crashed or hung\n",
              steps, calls, seed);
  std::printf("SONORE GRAPHICS FUZZ PASSED\n");
  return 0;
}
