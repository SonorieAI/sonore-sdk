// SPDX-License-Identifier: Apache-2.0
// The component-tree infrastructure, checked with real numbers.
//
// Cached rendering against direct painting, pixel for pixel, at fractional
// scales; the four generations of look rendering a standard set of controls
// and differing from one another; a Drawable placed four ways; a pen's
// pressure and tilt arriving through the very WM_POINTER path the window runs;
// inactivity timing on an injected clock; text dragged between components and
// parsed off OLE data objects; relative-position expressions, cycles included;
// a component tree kept in step with a data tree; movement watching; spring
// easing against the damped-oscillator formula; momentum decay and page
// snapping; and the display refresh, measured on this machine.
#include <sonore/gfx/animated_position.h>
#include <sonore/gfx/animator.h>
#include <sonore/gfx/backing.h>
#include <sonore/gfx/component.h>
#include <sonore/gfx/component_builder.h>
#include <sonore/gfx/drawable_component.h>
#include <sonore/gfx/lookandfeel_versions.h>
#include <sonore/gfx/mouse_input.h>
#include <sonore/gfx/movement_watcher.h>
#include <sonore/gfx/paint_diagnostics.h>
#include <sonore/gfx/relative_position.h>
#include <sonore/gfx/text_drag.h>
#include <sonore/gfx/vblank.h>
#include <sonore/gfx/viewport.h>
#include <sonore/gfx/widgets.h>
#if defined(_WIN32)
#include <sonore/gfx/window_win32.h>
#include <shlobj.h> // DROPFILES
#endif

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace sonore;
using namespace sonore::gfx;

static int g_checks = 0, g_failures = 0;
static void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}

static void section(const char* name) { std::printf("\n-- %s\n", name); }

// ── helpers ─────────────────────────────────────────────────────────────────

static uint64_t hashBitmap(const Bitmap& b) {
  uint64_t h = 1469598103934665603ull;
  const uint8_t* p = b.data();
  for (size_t i = 0; i < b.sizeInBytes(); ++i) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h;
}

static int maxChannelDifference(const Bitmap& a, const Bitmap& b) {
  if (a.width() != b.width() || a.height() != b.height()) return 256;
  int worst = 0;
  for (size_t i = 0; i < a.sizeInBytes(); ++i) {
    const int d = std::abs((int) a.data()[i] - (int) b.data()[i]);
    if (d > worst) worst = d;
  }
  return worst;
}

static int pixelsDifferentFrom(const Bitmap& b, PremulColour c) {
  int n = 0;
  for (int y = 0; y < b.height(); ++y)
    for (int x = 0; x < b.width(); ++x) {
      const PremulColour p = b.pixelAt(x, y);
      if (p.r != c.r || p.g != c.g || p.b != c.b || p.a != c.a) ++n;
    }
  return n;
}

// ── 1. Cached rendering ─────────────────────────────────────────────────────

/** A component that draws something with antialiased edges and counts how
 *  often it was asked to. */
struct Art : Component {
  int paints = 0;
  bool opaque = true;
  Colour colour = Colour::fromRGB(0x3366CC);
  void paint(Graphics& g) override {
    ++paints;
    if (opaque) g.fillAll(Colour::fromRGB(0x202830));
    g.setColour(colour);
    g.fillEllipse(Rect(3.25f, 2.5f, bounds().w - 7.0f, bounds().h - 5.5f));
    g.setColour(Colour(255, 240, 200, opaque ? 255 : 160));
    g.drawLine(1.0f, 1.0f, bounds().w - 2.0f, bounds().h - 3.0f, 1.5f);
    g.setGradient(ColourGradient::vertical(Colour(255, 0, 0, 200), 0.0f, Colour(0, 0, 255, 90),
                                           bounds().h));
    g.fillRoundedRect(Rect(bounds().w * 0.5f, 4.0f, bounds().w * 0.4f, bounds().h * 0.3f), 3.0f);
  }
};

struct Scene {
  Component root;
  Art panel, child, sibling;
  Scene(bool opaque) {
    root.setBounds({0, 0, 160, 120});
    panel.opaque = opaque;
    panel.setBounds({10.25f, 8.5f, 96.0f, 80.0f});
    child.setBounds({20.5f, 30.25f, 40.0f, 30.0f});
    child.colour = Colour::fromRGB(0x44AA55);
    child.opaque = false;
    sibling.setBounds({70.0f, 50.0f, 60.0f, 50.0f});
    sibling.colour = Colour::fromRGB(0xDD8800);
    root.addChild(&panel);
    panel.addChild(&child);
    root.addChild(&sibling);
  }
};

static Bitmap renderScene(Component& root, float scale) {
  Bitmap b((int) std::ceil(160.0f * scale), (int) std::ceil(120.0f * scale));
  b.clear(Colour::fromRGB(0x101010));
  Graphics g(b);
  g.addTransform(Transform::scaling(scale, scale));
  root.paintTree(g);
  return b;
}

static void testCachedImage() {
  section("CachedComponentImage");
  for (float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
    Scene direct(true), cached(true);
    cached.panel.setBufferedToImage(true);
    const Bitmap a = renderScene(direct.root, scale);
    const Bitmap b = renderScene(cached.root, scale);
    char what[160];
    std::snprintf(what, sizeof(what), "opaque buffered panel at %.2fx is identical to direct paint",
                  scale);
    check(maxChannelDifference(a, b) == 0, what);

    const int before = cached.panel.paints;
    const Bitmap again = renderScene(cached.root, scale);
    std::snprintf(what, sizeof(what), "at %.2fx a second paint comes from the cache (paint() not called)",
                  scale);
    check(cached.panel.paints == before && maxChannelDifference(again, a) == 0, what);
  }

  {
    Scene direct(false), cached(false);
    cached.panel.setBufferedToImage(true);
    const int diff = maxChannelDifference(renderScene(direct.root, 1.5f), renderScene(cached.root, 1.5f));
    std::printf("       translucent panel, worst channel difference: %d\n", diff);
    check(diff <= 2, "translucent buffered panel within 2 levels of direct paint (8-bit 'over' rounding)");
  }

  {
    Scene direct(true), cached(true);
    cached.panel.setBufferedToImage(true);
    auto* cache = static_cast<StandardCachedComponentImage*>(cached.panel.cachedComponentImage());
    renderScene(cached.root, 1.5f);
    const int renders = cache->renderCount();
    // A child changes: repaint() on it must invalidate the PARENT's cache.
    direct.child.colour = cached.child.colour = Colour::fromRGB(0xFF00FF);
    cached.child.repaint();
    const int childPaints = cached.child.paints;
    const Bitmap b = renderScene(cached.root, 1.5f);
    check(cache->renderCount() > renders && cached.child.paints > childPaints,
          "child.repaint() invalidates the buffered parent");
    check(maxChannelDifference(renderScene(direct.root, 1.5f), b) == 0,
          "and the re-rendered cache matches direct paint of the new state");

    // Partial invalidation: only the child's rectangle is rendered again, and
    // the sibling (outside the panel) never enters the cache.
    const int panelPaints = cached.panel.paints;
    cached.child.repaint(Rect(0, 0, 5, 5));
    renderScene(cached.root, 1.5f);
    check(cached.panel.paints == panelPaints + 1, "a small repaint re-renders through the panel once");

    // A move by a whole number of DEVICE pixels keeps the cache: 2 logical
    // px at 1.5x is 3 device px, so the sub-pixel phase is unchanged.
    const int r0 = cache->renderCount();
    const Rect pb = cached.panel.bounds();
    cached.panel.setBounds({pb.x + 2.0f, pb.y + 2.0f, pb.w, pb.h});
    direct.panel.setBounds({pb.x + 2.0f, pb.y + 2.0f, pb.w, pb.h});
    const Bitmap moved = renderScene(cached.root, 1.5f);
    check(cache->renderCount() == r0, "moving by whole device pixels does not re-render the cache");
    check(maxChannelDifference(renderScene(direct.root, 1.5f), moved) == 0,
          "and the moved panel is still identical to direct paint");
    // A move by a fraction of a device pixel changes the antialiasing, so it
    // must re-render -- and still match.
    cached.panel.setBounds({pb.x + 2.25f, pb.y + 2.0f, pb.w, pb.h});
    direct.panel.setBounds({pb.x + 2.25f, pb.y + 2.0f, pb.w, pb.h});
    const Bitmap shifted = renderScene(cached.root, 1.5f);
    check(cache->renderCount() > r0, "a sub-pixel move re-renders the cache");
    check(maxChannelDifference(renderScene(direct.root, 1.5f), shifted) == 0,
          "and matches direct paint at the new phase");
  }

  {
    // Hidden ancestor: a repaint while not showing must still invalidate.
    Scene s(true);
    s.panel.setBufferedToImage(true);
    renderScene(s.root, 1.0f);
    s.root.setVisible(false);
    s.child.colour = Colour::fromRGB(0x00FFFF);
    s.child.repaint();
    s.root.setVisible(true);
    Scene ref(true);
    ref.child.colour = Colour::fromRGB(0x00FFFF);
    check(maxChannelDifference(renderScene(ref.root, 1.0f), renderScene(s.root, 1.0f)) == 0,
          "a change made while an ancestor was hidden is not served stale from the cache");
  }

  {
    // Through the window's own damage path at a fractional scale.
    Scene direct(true), cached(true);
    cached.panel.setBufferedToImage(true);
    Backing bd, bc;
    bd.setContent(&direct.root);
    bc.setContent(&cached.root);
    bd.setScale(1.25f);
    bc.setScale(1.25f);
    bd.setDeviceSize(200, 150);
    bc.setDeviceSize(200, 150);
    bd.invalidateAll();
    bc.invalidateAll();
    bd.render();
    bc.render();
    direct.child.colour = cached.child.colour = Colour::fromRGB(0x112233);
    direct.child.repaint();
    cached.child.repaint();
    const size_t changed = bc.render().size();
    bd.render();
    check(changed > 0 && maxChannelDifference(bd.bitmap(), bc.bitmap()) == 0,
          "partial (damage) repaint through Backing at 1.25x matches the unbuffered tree");
  }

  {
    Scene s(true);
    s.panel.setBufferedToImage(true);
    check(s.panel.isBufferedToImage(), "setBufferedToImage(true) installs a cache");
    s.panel.setBufferedToImage(false);
    check(!s.panel.isBufferedToImage(), "and false removes it");
  }
}

static void testPaintDiagnostics() {
  section("ComponentPaintDiagnostics");
  Scene s(true);
  s.panel.setAccessibleName("panel");
  ComponentPaintDiagnostics diag;
  double fake = 0.0;
  diag.setClock([&fake] {
    fake += 0.001; // every reading is a millisecond later
    return fake;
  });
  diag.install();
  check(diag.isInstalled(), "installs as the paint observer");
  renderScene(s.root, 1.0f);
  renderScene(s.root, 1.0f);
  const ComponentPaintDiagnostics::Stat* p = diag.statFor(&s.panel);
  check(p && p->paints == 2, "counts each paint of each component");
  check(p && std::fabs(p->totalSeconds - 0.002) < 1e-9 && p->name == "panel",
        "times each paint by the clock (two readings 1 ms apart, twice)");
  check(diag.stats().size() == 4, "one record per painted component (root, panel, child, sibling)");
  check(diag.report().find("panel") != std::string::npos, "the report names the component");

  // Flashing: through Backing, a repaint of the child is covered by an
  // overlay -- exactly the damaged region changes.
  diag.setTimePaints(false);
  Scene plain(true);
  Backing bf, bp;
  bf.setContent(&s.root);
  bp.setContent(&plain.root);
  for (Backing* b : {&bf, &bp}) {
    b->setScale(1.0f);
    b->setDeviceSize(160, 120);
    b->invalidateAll();
  }
  diag.uninstall();
  bf.render();
  bp.render();
  diag.install();
  diag.setFlashRepaints(true);
  s.child.repaint();
  bf.render();
  diag.uninstall();
  plain.child.repaint();
  bp.render();
  int inside = 0, outside = 0;
  const Point o = s.child.localToRoot({0, 0});
  const PixelRect damaged = PixelRect::enclosing(Rect(o.x, o.y, s.child.bounds().w, s.child.bounds().h));
  for (int y = 0; y < 120; ++y)
    for (int x = 0; x < 160; ++x) {
      const PremulColour a = bf.bitmap().pixelAt(x, y), b = bp.bitmap().pixelAt(x, y);
      const bool same = a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
      const bool in = x >= damaged.x && x < damaged.right() && y >= damaged.y && y < damaged.bottom();
      if (!same && in) ++inside;
      if (!same && !in) ++outside;
    }
  check(diag.framesFlashed() == 1 && inside > 0 && outside == 0,
        "flashing tints exactly the repainted region, nothing else");
}

// ── 2. The four looks ───────────────────────────────────────────────────────

static Bitmap renderStandardSet(LookAndFeel& look) {
  Bitmap b(420, 220);
  b.clear(look.background());
  Graphics g(b);
  Font font; // no face: text draws nothing, shapes are what is compared
  WidgetState normal, hover, pressed, toggled;
  hover.hovered = true;
  pressed.pressed = true;
  toggled.toggled = true;
  look.drawRotarySlider(g, Rect(10, 10, 60, 60), 0.65f, normal);
  look.drawRotarySlider(g, Rect(80, 10, 60, 60), 0.2f, hover);
  look.drawLinearSlider(g, Rect(150, 20, 120, 24), 0.4f, false, normal);
  look.drawLinearSlider(g, Rect(280, 10, 24, 90), 0.7f, true, pressed);
  look.drawLinearBar(g, Rect(320, 10, 90, 16), 0.5f, false, normal);
  look.drawButton(g, Rect(10, 90, 80, 26), "OK", font, normal);
  look.drawButton(g, Rect(100, 90, 80, 26), "On", font, toggled);
  look.drawButton(g, Rect(190, 90, 80, 26), "Down", font, pressed);
  look.drawComboBox(g, Rect(10, 130, 140, 24), "Choice", font, normal);
  look.drawScrollBar(g, Rect(400, 40, 10, 170), Rect(400, 60, 10, 50), true, false);
  look.drawProgressBar(g, Rect(160, 130, 150, 18), 0.6, "", font);
  look.drawTextEditorBackground(g, Rect(10, 165, 140, 24), true, true);
  look.drawTab(g, Rect(160, 160, 70, 24), "Tab", font, true, false);
  look.drawTab(g, Rect(232, 160, 70, 24), "Tab", font, false, true);
  look.drawPopupBackground(g, Rect(320, 120, 70, 90));
  PopupItem item;
  item.text = "Item";
  item.ticked = true;
  look.drawPopupItem(g, Rect(320, 125, 70, 20), item, font, true);
  look.drawTooltip(g, Rect(10, 195, 120, 20), "tip", font);
  look.drawRangeSlider(g, Rect(160, 192, 150, 24), 0.2f, 0.8f, normal);
  return b;
}

static void testLooks() {
  section("LookAndFeel_V1 / V2 / V3 / V4");
  LookAndFeel_V1 v1;
  LookAndFeel_V2 v2;
  LookAndFeel_V3 v3;
  LookAndFeel_V4 dark(LookAndFeel_V4::getDarkColourScheme());
  LookAndFeel_V4 midnight(LookAndFeel_V4::getMidnightColourScheme());
  LookAndFeel_V4 grey(LookAndFeel_V4::getGreyColourScheme());
  LookAndFeel_V4 light(LookAndFeel_V4::getLightColourScheme());
  LookAndFeel base;
  struct Named {
    const char* name;
    LookAndFeel* look;
  } looks[] = {{"V1", &v1},          {"V2", &v2},     {"V3", &v3},       {"V4 dark", &dark},
               {"V4 midnight", &midnight}, {"V4 grey", &grey}, {"V4 light", &light}, {"default", &base}};
  std::vector<uint64_t> hashes;
  for (const Named& n : looks) {
    const Bitmap b = renderStandardSet(*n.look);
    const int inked = pixelsDifferentFrom(b, PremulColour::from(n.look->background()));
    char what[120];
    std::snprintf(what, sizeof(what), "%s draws the standard set (%d pixels of ink)", n.name, inked);
    check(inked > 8000, what);
    hashes.push_back(hashBitmap(b));
  }
  bool distinct = true;
  for (size_t i = 0; i < hashes.size(); ++i)
    for (size_t j = i + 1; j < hashes.size(); ++j)
      if (hashes[i] == hashes[j]) distinct = false;
  check(distinct, "all eight looks render differently from one another");

  // An indeterminate V2 bar two pixels tall or less: its stripes stepped by
  // the inner height, zero or below there, and the paint never returned. On
  // a thread with a deadline, so a regression fails instead of hanging.
  {
    std::atomic<bool> done{false};
    std::thread painter([&done] {
      LookAndFeel_V2 look;
      Bitmap bmp(120, 40);
      Graphics g(bmp);
      for (float h : {2.0f, 1.0f, 0.0f, -4.0f}) look.drawProgressBar(g, Rect(4, 4, 100, h), -1.0, "", Font());
      done = true;
    });
    for (int i = 0; i < 1000 && !done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    check(done, "an indeterminate V2 progress bar two pixels tall or less is painted and returns");
    if (!done) {
      std::printf("GUI INFRA TESTS FAILED\n");
      std::fflush(stdout);
      std::_Exit(1);
    }
    painter.join();
  }

  // The V4 schemes are the reference's tables.
  check(dark.getCurrentColourScheme().getUIColour(LookAndFeel_V4::ColourScheme::windowBackground) ==
            Colour::fromARGB(0xff323e44),
        "V4 dark windowBackground is 0xff323e44");
  check(light.getCurrentColourScheme().getUIColour(LookAndFeel_V4::ColourScheme::highlightedFill) ==
            Colour::fromARGB(0xff42a2c8),
        "V4 light highlightedFill is 0xff42a2c8");
  check(grey.getCurrentColourScheme().getUIColour(LookAndFeel_V4::ColourScheme::defaultFill) ==
            Colour::fromARGB(0xff21ba90),
        "V4 grey defaultFill is 0xff21ba90");
  LookAndFeel_V4 changed;
  changed.setColourScheme(LookAndFeel_V4::getMidnightColourScheme());
  check(changed.getCurrentColourScheme() == LookAndFeel_V4::getMidnightColourScheme() &&
            changed.background() == Colour::fromARGB(0xff2f2f3a),
        "setColourScheme switches the palette");

  // Through real widgets: a look set on a parent reaches every control in it.
  Widget panel;
  panel.setBounds({0, 0, 200, 100});
  Slider knob(Slider::Style::Rotary);
  knob.setBounds({10, 10, 60, 60});
  knob.setValue(0.5f, false);
  Button button("Go");
  button.setBounds({90, 30, 80, 26});
  panel.addChild(&knob);
  panel.addChild(&button);
  std::vector<uint64_t> widgetHashes;
  for (LookAndFeel* l : {(LookAndFeel*) &v1, (LookAndFeel*) &v2, (LookAndFeel*) &v3, (LookAndFeel*) &dark}) {
    panel.setLookAndFeel(l);
    Bitmap b(200, 100);
    b.clear(Colour::fromRGB(0x808080));
    Graphics g(b);
    panel.paintTree(g);
    widgetHashes.push_back(hashBitmap(b));
  }
  check(widgetHashes[0] != widgetHashes[1] && widgetHashes[1] != widgetHashes[2] &&
            widgetHashes[2] != widgetHashes[3] && widgetHashes[0] != widgetHashes[3],
        "a Slider and a Button inherit each look from their parent and draw it");
}

// ── 3. DrawableComponent ────────────────────────────────────────────────────

static void testDrawable() {
  section("DrawableComponent / OwningDrawableComponent");
  const Drawable red = Drawable::parse(
      "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 50'>"
      "<rect x='0' y='0' width='100' height='50' fill='#ff0000'/></svg>");
  check(!red.isEmpty(), "the test drawable parses");

  auto paintOf = [](DrawableComponent& c) {
    Bitmap b(200, 200);
    Graphics g(b);
    c.paintTree(g);
    return b;
  };
  auto redAt = [](const Bitmap& b, int x, int y) {
    const PremulColour p = b.pixelAt(x, y);
    return p.r > 250 && p.a > 250 && p.g < 5;
  };
  auto clearAt = [](const Bitmap& b, int x, int y) { return b.pixelAt(x, y).a == 0; };

  DrawableComponent c(&red);
  c.setBounds({0, 0, 200, 200});
  RectanglePlacement contain; // default: Contain, centred
  c.setPlacement(contain);
  Bitmap b = paintOf(c);
  check(redAt(b, 100, 51) && redAt(b, 1, 148) && clearAt(b, 100, 48) && clearAt(b, 100, 151),
        "Contain: 200 wide, rows 50..150, letterboxed above and below");

  RectanglePlacement cover;
  cover.fit = RectanglePlacement::Fit::Cover;
  c.setPlacement(cover);
  b = paintOf(c);
  check(redAt(b, 0, 0) && redAt(b, 199, 199) && redAt(b, 100, 100), "Cover fills the bounds");

  RectanglePlacement stretch;
  stretch.fit = RectanglePlacement::Fit::Stretch;
  c.setPlacement(stretch);
  b = paintOf(c);
  check(redAt(b, 0, 0) && redAt(b, 199, 199), "Stretch fills the bounds, aspect ignored");

  RectanglePlacement none;
  none.fit = RectanglePlacement::Fit::None;
  c.setPlacement(none);
  b = paintOf(c);
  check(redAt(b, 51, 76) && redAt(b, 148, 123) && clearAt(b, 48, 100) && clearAt(b, 100, 72),
        "None: natural size (100x50), centred at 50..150 x 75..125");

  none.alignX = -1.0f;
  none.alignY = 1.0f;
  c.setPlacement(none);
  b = paintOf(c);
  check(redAt(b, 1, 198) && redAt(b, 98, 152) && clearAt(b, 101, 198),
        "None aligned left/bottom: 0..100 x 150..200");
  check(c.drawableArea(c.localBounds()).x == 0.0f && c.drawableArea(c.localBounds()).y == 150.0f,
        "drawableArea reports where it lands");

  c.setUsesDrawablePlacement(true);
  b = paintOf(c);
  check(redAt(b, 100, 51) && clearAt(b, 100, 48), "the SVG's own preserveAspectRatio (meet) also contains");

  OwningDrawableComponent owner;
  owner.setBounds({0, 0, 200, 200});
  std::string why;
  check(!owner.setSvg("<svg></svg>", &why), "an SVG with nothing in it is refused");
  check(owner.setSvg("<svg viewBox='0 0 10 10'><circle cx='5' cy='5' r='5' fill='#ff0000'/></svg>"),
        "OwningDrawableComponent parses and keeps its own");
  owner.setPlacement(contain);
  b = paintOf(owner);
  check(redAt(b, 100, 100) && clearAt(b, 2, 2), "and draws it (a circle filling the square)");
  check(!owner.interceptsMouse(), "a picture lets clicks through");
}

// ── 4. MouseInputSource / PenDetails ────────────────────────────────────────

struct Probe : Component {
  std::vector<MouseEvent> downs, drags, ups, moves;
  void mouseDown(const MouseEvent& e) override { downs.push_back(e); }
  void mouseDrag(const MouseEvent& e) override { drags.push_back(e); }
  void mouseUp(const MouseEvent& e) override { ups.push_back(e); }
  void mouseMove(const MouseEvent& e) override { moves.push_back(e); }
};

#if defined(_WIN32) && defined(SONORE_HAS_WIN32_POINTER)
// The fake pointer API: what GetPointerType / GetPointerPenInfo would say.
static POINTER_INPUT_TYPE g_fakeType = PT_PEN;
static POINTER_PEN_INFO g_fakePen{};
static POINTER_TOUCH_INFO g_fakeTouch[4]{};
static BOOL WINAPI fakeGetPointerType(UINT32 id, POINTER_INPUT_TYPE* type) {
  *type = id >= 100 ? PT_TOUCH : g_fakeType;
  return TRUE;
}
static BOOL WINAPI fakeGetPointerPenInfo(UINT32, POINTER_PEN_INFO* info) {
  *info = g_fakePen;
  return TRUE;
}
static BOOL WINAPI fakeGetPointerTouchInfo(UINT32 id, POINTER_TOUCH_INFO* info) {
  *info = g_fakeTouch[(id - 100) % 4];
  return TRUE;
}
static BOOL WINAPI fakeGetPointerInfo(UINT32, POINTER_INFO*) { return FALSE; }

#endif

static void testPointerInput() {
  section("MouseInputSource / PenDetails");
  {
    // Plain router: an event with no pointer input is a mouse with no pressure.
    Component root;
    root.setBounds({0, 0, 100, 100});
    Probe p;
    p.setBounds({0, 0, 100, 100});
    root.addChild(&p);
    MouseRouter router(root);
    router.mouseDown({10, 10});
    router.mouseUp({10, 10});
    check(p.downs.size() == 1 && p.downs[0].sourceType == MouseInputSourceType::Mouse &&
              !p.downs[0].isPressureValid() && p.downs[0].eventComponent == &p,
          "existing input is unchanged: a mouse, no pressure");
  }
#if defined(_WIN32)
  {
    // The MOUSE's source, kept by the window from the legacy messages it hands
    // its router. Only the pen and fingers updated a source, so the mouse's
    // sat at (0,0), never down and never pressed, whatever the mouse did.
    Component root;
    root.setBounds({0, 0, 300, 200});
    Probe p;
    p.setBounds({0, 0, 300, 200});
    root.addChild(&p);
    NativeWindow window;
    if (!window.open(nullptr, root, 300, 200)) {
      check(false, "a window opens for the mouse-source test");
    } else {
      HWND hwnd = (HWND) window.handle();
      const float scale = window.scale();
      auto at = [&](float lx, float ly) {
        return MAKELPARAM((int) std::lround(lx * scale), (int) std::lround(ly * scale));
      };
      MouseInputSource& mouse = MouseInputSources::instance().getMainMouseSource();
      char msg[200];
      SendMessageW(hwnd, WM_MOUSEMOVE, 0, at(60, 40));
      std::snprintf(msg, sizeof msg, "a WM_MOUSEMOVE moves the mouse's source to (%.0f, %.0f)",
                    mouse.getRootPosition().x, mouse.getRootPosition().y);
      check(std::fabs(mouse.getRootPosition().x - 60.0f) < 1.0f &&
                std::fabs(mouse.getRootPosition().y - 40.0f) < 1.0f && !mouse.isDragging(),
            msg);
      SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, at(70, 50));
      std::snprintf(msg, sizeof msg, "a WM_LBUTTONDOWN holds it down at (%.0f, %.0f), click %d",
                    mouse.getRootPosition().x, mouse.getRootPosition().y,
                    mouse.getNumberOfMultipleClicks());
      check(p.downs.size() == 1 && mouse.isDragging() && mouse.isMouse() &&
                std::fabs(mouse.getRootPosition().x - 70.0f) < 1.0f &&
                mouse.getNumberOfMultipleClicks() == 1 && mouse.getLastMouseDownTime() > -1.0 &&
                !mouse.isPressureValid(),
            msg);
      SendMessageW(hwnd, WM_LBUTTONUP, 0, at(70, 50));
      check(!mouse.isDragging(), "and the WM_LBUTTONUP lets it go");
      SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, at(70, 50));
      check(mouse.isDragging() && mouse.getNumberOfMultipleClicks() == 2,
            "a second press on the same spot straight after is click 2");
      SendMessageW(hwnd, WM_CAPTURECHANGED, 0, 0);
      check(!mouse.isDragging(), "and losing the capture mid-press ends it");
      window.close();
    }
  }
#endif
#if defined(_WIN32) && defined(SONORE_HAS_WIN32_POINTER)
  Component root;
  root.setBounds({0, 0, 300, 200});
  Probe left, right;
  left.setBounds({0, 0, 150, 200});
  right.setBounds({150, 0, 150, 200});
  root.addChild(&left);
  root.addChild(&right);
  NativeWindow window;
  if (!window.open(nullptr, root, 300, 200)) {
    check(false, "a window opens for the pointer test");
    return;
  }
  HWND hwnd = (HWND) window.handle();
  detail::Win32PointerApi saved = detail::win32PointerApi();
  detail::Win32PointerApi& api = detail::win32PointerApi();
  api.getPointerType = &fakeGetPointerType;
  api.getPointerPenInfo = &fakeGetPointerPenInfo;
  api.getPointerTouchInfo = &fakeGetPointerTouchInfo;
  api.getPointerInfo = &fakeGetPointerInfo;

  const float scale = window.scale();
  auto screenOf = [&](float lx, float ly) {
    POINT p{(LONG) std::lround(lx * scale), (LONG) std::lround(ly * scale)};
    ClientToScreen(hwnd, &p);
    return p;
  };
  auto send = [&](UINT msg, UINT32 id, POINT at) {
    return SendMessageW(hwnd, msg, MAKEWPARAM(id, 0), MAKELPARAM(at.x, at.y));
  };

  // A pen: half pressure, tilted, rotated a quarter turn, eraser end down.
  g_fakeType = PT_PEN;
  g_fakePen = POINTER_PEN_INFO{};
  g_fakePen.pointerInfo.pointerType = PT_PEN;
  g_fakePen.pointerInfo.pointerId = 7;
  g_fakePen.pointerInfo.pointerFlags = POINTER_FLAG_INCONTACT | POINTER_FLAG_FIRSTBUTTON |
                                       POINTER_FLAG_PRIMARY | POINTER_FLAG_DOWN;
  g_fakePen.pointerInfo.ptPixelLocation = screenOf(40, 50);
  g_fakePen.pointerInfo.dwTime = 1000;
  g_fakePen.penMask = PEN_MASK_PRESSURE | PEN_MASK_ROTATION | PEN_MASK_TILT_X | PEN_MASK_TILT_Y;
  g_fakePen.pressure = 512;
  g_fakePen.rotation = 90;
  g_fakePen.tiltX = 45;
  g_fakePen.tiltY = -18;
  g_fakePen.penFlags = PEN_FLAG_ERASER;
  const LRESULT handled = send(WM_POINTERDOWN, 7, g_fakePen.pointerInfo.ptPixelLocation);
  check(handled == 0 && left.downs.size() == 1, "WM_POINTERDOWN from a pen reaches the component under it");
  if (!left.downs.empty()) {
    const MouseEvent& e = left.downs[0];
    check(e.sourceType == MouseInputSourceType::Pen && e.isFromPen(), "the event says it came from a pen");
    check(std::fabs(e.pressure - 0.5f) < 1e-6f, "pressure 512/1024 arrives as 0.5");
    check(std::fabs(e.pen.tiltX - 0.5f) < 1e-6f && std::fabs(e.pen.tiltY + 0.2f) < 1e-6f,
          "tilt 45 and -18 degrees arrive as 0.5 and -0.2");
    check(std::fabs(e.pen.rotation - 1.5707963f) < 1e-5f, "rotation 90 degrees arrives as pi/2");
    check(e.pen.eraser, "the eraser flag arrives");
    check(std::fabs(e.position.x - 40.0f) < 1.0f && std::fabs(e.position.y - 50.0f) < 1.0f,
          "at the position the pen touched, in logical units");
  }
  MouseInputSource& penSource = MouseInputSources::instance().sourceFor(MouseInputSourceType::Pen, 0);
  check(penSource.isDragging() && std::fabs(penSource.getCurrentPressure() - 0.5f) < 1e-6f &&
            penSource.isPen(),
        "MouseInputSources has the pen, down, at 0.5 pressure");

  g_fakePen.pressure = 1024;
  g_fakePen.pointerInfo.ptPixelLocation = screenOf(200, 60); // over the OTHER component
  g_fakePen.pointerInfo.pointerFlags = POINTER_FLAG_INCONTACT | POINTER_FLAG_FIRSTBUTTON |
                                       POINTER_FLAG_PRIMARY | POINTER_FLAG_UPDATE;
  send(WM_POINTERUPDATE, 7, g_fakePen.pointerInfo.ptPixelLocation);
  check(left.drags.size() == 1 && right.downs.empty() && right.drags.empty(),
        "a pen drag stays with the component it went down on (capture)");
  check(!left.drags.empty() && std::fabs(left.drags[0].pressure - 1.0f) < 1e-6f,
        "full pressure during the drag");
  g_fakePen.pointerInfo.pointerFlags = POINTER_FLAG_PRIMARY | POINTER_FLAG_UP;
  send(WM_POINTERUP, 7, g_fakePen.pointerInfo.ptPixelLocation);
  check(left.ups.size() == 1 && !penSource.isDragging(), "WM_POINTERUP releases it");
  // Hovering pen: a move with no contact is a hover, with the pen's data.
  g_fakePen.pointerInfo.pointerFlags = POINTER_FLAG_PRIMARY | POINTER_FLAG_INRANGE | POINTER_FLAG_UPDATE;
  g_fakePen.pointerInfo.ptPixelLocation = screenOf(220, 70);
  g_fakePen.pressure = 0;
  send(WM_POINTERUPDATE, 7, g_fakePen.pointerInfo.ptPixelLocation);
  check(!right.moves.empty() && right.moves.back().isFromPen(), "a hovering pen moves over components");

  // A mouse pointer is declined, so DefWindowProc makes legacy messages.
  g_fakeType = PT_MOUSE;
  const size_t downsBefore = left.downs.size();
  send(WM_POINTERDOWN, 3, screenOf(20, 20));
  check(left.downs.size() == downsBefore, "a PT_MOUSE pointer is left to the legacy mouse path");

  // Two fingers: the first (primary) on the left, the second on the right --
  // each captured by its own component, each with its own index.
  for (int i = 0; i < 2; ++i) {
    g_fakeTouch[i] = POINTER_TOUCH_INFO{};
    g_fakeTouch[i].pointerInfo.pointerType = PT_TOUCH;
    g_fakeTouch[i].pointerInfo.pointerId = 100u + (UINT32) i;
    g_fakeTouch[i].touchMask = TOUCH_MASK_PRESSURE | TOUCH_MASK_ORIENTATION;
    g_fakeTouch[i].pressure = 256;
    g_fakeTouch[i].orientation = 30;
    g_fakeTouch[i].pointerInfo.dwTime = 5000 + (DWORD) i * 1000;
  }
  left.downs.clear();
  right.downs.clear();
  left.drags.clear();
  right.drags.clear();
  g_fakeTouch[0].pointerInfo.pointerFlags = POINTER_FLAG_INCONTACT | POINTER_FLAG_PRIMARY | POINTER_FLAG_DOWN;
  g_fakeTouch[0].pointerInfo.ptPixelLocation = screenOf(30, 30);
  send(WM_POINTERDOWN, 100, g_fakeTouch[0].pointerInfo.ptPixelLocation);
  g_fakeTouch[1].pointerInfo.pointerFlags = POINTER_FLAG_INCONTACT | POINTER_FLAG_DOWN;
  g_fakeTouch[1].pointerInfo.ptPixelLocation = screenOf(250, 30);
  send(WM_POINTERDOWN, 101, g_fakeTouch[1].pointerInfo.ptPixelLocation);
  check(left.downs.size() == 1 && right.downs.size() == 1, "two fingers press two components at once");
  check(!left.downs.empty() && left.downs[0].isFromTouch() && left.downs[0].sourceIndex == 0 &&
            std::fabs(left.downs[0].pressure - 0.25f) < 1e-6f &&
            std::fabs(left.downs[0].orientation - 0.5235988f) < 1e-5f,
        "the first finger: touch, index 0, pressure 0.25, orientation 30 degrees");
  check(!right.downs.empty() && right.downs[0].sourceIndex == 1, "the second finger: index 1");
  // Both move -- and each drag goes to the component its finger is holding,
  // even when the fingers cross.
  g_fakeTouch[0].pointerInfo.pointerFlags = POINTER_FLAG_INCONTACT | POINTER_FLAG_PRIMARY | POINTER_FLAG_UPDATE;
  g_fakeTouch[0].pointerInfo.ptPixelLocation = screenOf(260, 40);
  send(WM_POINTERUPDATE, 100, g_fakeTouch[0].pointerInfo.ptPixelLocation);
  g_fakeTouch[1].pointerInfo.pointerFlags = POINTER_FLAG_INCONTACT | POINTER_FLAG_UPDATE;
  g_fakeTouch[1].pointerInfo.ptPixelLocation = screenOf(20, 40);
  send(WM_POINTERUPDATE, 101, g_fakeTouch[1].pointerInfo.ptPixelLocation);
  check(left.drags.size() == 1 && right.drags.size() == 1 && left.drags[0].sourceIndex == 0 &&
            right.drags[0].sourceIndex == 1,
        "crossed fingers keep their own captures");
  check(MouseInputSources::instance().getNumDraggingMouseSources() >= 2, "two sources are dragging");
  g_fakeTouch[1].pointerInfo.pointerFlags = POINTER_FLAG_UP;
  send(WM_POINTERUP, 101, g_fakeTouch[1].pointerInfo.ptPixelLocation);
  g_fakeTouch[0].pointerInfo.pointerFlags = POINTER_FLAG_PRIMARY | POINTER_FLAG_UP;
  send(WM_POINTERUP, 100, g_fakeTouch[0].pointerInfo.ptPixelLocation);
  check(left.ups.size() == 2 && right.ups.size() == 1, "and each release goes where its press did");

  // A secondary finger on a component that is destroyed mid-gesture.
  {
    auto doomed = std::make_unique<Probe>();
    doomed->setBounds({150, 100, 150, 100});
    root.addChild(doomed.get());
    g_fakeTouch[0].pointerInfo.pointerFlags = POINTER_FLAG_INCONTACT | POINTER_FLAG_PRIMARY | POINTER_FLAG_DOWN;
    g_fakeTouch[0].pointerInfo.ptPixelLocation = screenOf(30, 30);
    send(WM_POINTERDOWN, 100, g_fakeTouch[0].pointerInfo.ptPixelLocation);
    g_fakeTouch[1].pointerInfo.pointerFlags = POINTER_FLAG_INCONTACT | POINTER_FLAG_DOWN;
    g_fakeTouch[1].pointerInfo.ptPixelLocation = screenOf(250, 150);
    send(WM_POINTERDOWN, 101, g_fakeTouch[1].pointerInfo.ptPixelLocation);
    const bool got = doomed->downs.size() == 1;
    doomed.reset();
    g_fakeTouch[1].pointerInfo.pointerFlags = POINTER_FLAG_INCONTACT | POINTER_FLAG_UPDATE;
    send(WM_POINTERUPDATE, 101, g_fakeTouch[1].pointerInfo.ptPixelLocation);
    g_fakeTouch[1].pointerInfo.pointerFlags = POINTER_FLAG_UP;
    send(WM_POINTERUP, 101, g_fakeTouch[1].pointerInfo.ptPixelLocation);
    g_fakeTouch[0].pointerInfo.pointerFlags = POINTER_FLAG_PRIMARY | POINTER_FLAG_UP;
    send(WM_POINTERUP, 100, g_fakeTouch[0].pointerInfo.ptPixelLocation);
    check(got, "a finger held on a component that is then destroyed is forgotten, not called");
  }
  api = saved;
  window.close();
#else
  std::printf("       (WM_POINTER not available in this build: the pen path is compiled out)\n");
#endif
}

// ── 5. MouseInactivityDetector ──────────────────────────────────────────────

static void testInactivity() {
  section("MouseInactivityDetector");
  Component root;
  root.setBounds({0, 0, 200, 200});
  Probe child;
  child.setBounds({50, 50, 100, 100});
  root.addChild(&child);
  MouseRouter router(root);
  double now = 0.0;
  MouseInactivityDetector detector(root);
  detector.setClock([&now] { return now; });
  detector.setDelay(1000);
  detector.setMouseMoveTolerance(10);
  int active = 0, inactive = 0;
  detector.onBecameActive = [&] { ++active; };
  detector.onBecameInactive = [&] { ++inactive; };

  router.mouseMove({60, 60}); // over the CHILD: nested events count
  now = 0.5;
  detector.tick();
  check(detector.isActive() && inactive == 0, "half a second still: still active");
  now = 0.99;
  detector.tick();
  check(inactive == 0, "not before the delay");
  now = 1.0;
  detector.tick();
  check(!detector.isActive() && inactive == 1, "inactive exactly when the delay has passed");
  now = 2.0;
  detector.tick();
  check(inactive == 1, "and told once, not every tick");
  router.mouseMove({65, 63}); // inside the tolerance
  check(!detector.isActive() && active == 0, "a drift within the tolerance does not wake it");
  router.mouseMove({90, 90});
  check(detector.isActive() && active == 1, "moving past the tolerance wakes it, once");
  now = 2.5;
  router.mouseDown({90, 90}); // a click is activity and resets the clock
  router.mouseUp({90, 90});
  now = 3.2;
  detector.tick();
  check(detector.isActive(), "a click restarts the wait");
  now = 3.6;
  detector.tick();
  check(!detector.isActive() && inactive == 2, "and it goes inactive a delay after the click");
  router.mouseWheel({90, 90}, 1.0f);
  check(detector.isActive() && active == 2, "the wheel wakes it");
}

// ── 6. Text drag ────────────────────────────────────────────────────────────

struct TextField : Component, TextDragAndDropTarget {
  std::string received, entered;
  Point enteredAt, droppedAt;
  int moves = 0, exits = 0;
  bool wantsDigitsOnly = false;
  TextDragAndDropTarget* asTextDragAndDropTarget() override { return this; }
  bool isInterestedInTextDrag(const std::string& text) override {
    if (!wantsDigitsOnly) return true;
    for (char c : text)
      if (c < '0' || c > '9') return false;
    return true;
  }
  void textDragEnter(const std::string& text, Point p) override {
    entered = text;
    enteredAt = p;
  }
  void textDragMove(const std::string&, Point) override { ++moves; }
  void textDragExit(const std::string&) override { ++exits; }
  void textDropped(const std::string& text, Point p) override {
    received = text;
    droppedAt = p;
  }
};

struct TextSource : Component {
  std::string selection = "dragged words";
  void mouseDrag(const MouseEvent& e) override {
    MouseRouter* r = routerFor(this);
    if (r && !r->isDragging() && std::fabs(e.dragDistanceX()) > 4.0f)
      r->startTextDrag(selection, this, e.rootPosition);
  }
};

struct ItemTarget : Component {
  int offered = 0;
  bool isInterestedInDrag(const DragSource&) override {
    ++offered;
    return false;
  }
};

#if defined(_WIN32)
/** A data object carrying files, as the shell's would: CF_HDROP. */
class FileDataObject : public IDataObject {
public:
  explicit FileDataObject(const std::vector<std::wstring>& paths) : paths_(paths) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDataObject)) {
      *out = this;
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG n = --refs_;
    if (n == 0) delete this;
    return n;
  }
  HRESULT STDMETHODCALLTYPE GetData(FORMATETC* f, STGMEDIUM* m) override {
    if (f->cfFormat != CF_HDROP) return DV_E_FORMATETC;
    size_t chars = 1;
    for (const auto& p : paths_) chars += p.size() + 1;
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + chars * sizeof(wchar_t));
    auto* drop = (DROPFILES*) GlobalLock(g);
    drop->pFiles = sizeof(DROPFILES);
    drop->fWide = TRUE;
    wchar_t* w = (wchar_t*) ((char*) drop + sizeof(DROPFILES));
    for (const auto& p : paths_) {
      std::memcpy(w, p.c_str(), p.size() * sizeof(wchar_t));
      w += p.size() + 1;
    }
    GlobalUnlock(g);
    m->tymed = TYMED_HGLOBAL;
    m->hGlobal = g;
    m->pUnkForRelease = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* f) override {
    return f->cfFormat == CF_HDROP ? S_OK : DV_E_FORMATETC;
  }
  HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD, IEnumFORMATETC**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return E_NOTIMPL; }

private:
  virtual ~FileDataObject() = default;
  std::vector<std::wstring> paths_;
  ULONG refs_ = 1;
};

struct FileTarget : Component {
  std::vector<std::string> got;
  bool isInterestedInFileDrag(const std::vector<std::string>&) override { return true; }
  void filesDropped(const std::vector<std::string>& files, Point) override { got = files; }
};
#endif

static void testTextDrag() {
  section("TextDragAndDropTarget");
  Component root;
  root.setBounds({0, 0, 300, 100});
  TextSource source;
  source.setBounds({0, 0, 100, 100});
  TextField field, digits;
  field.setBounds({100, 0, 100, 100});
  digits.setBounds({200, 0, 100, 100});
  digits.wantsDigitsOnly = true;
  ItemTarget plain;
  plain.setBounds({0, 0, 1, 1});
  root.addChild(&source);
  root.addChild(&field);
  root.addChild(&digits);
  MouseRouter router(root);

  router.mouseDown({20, 50});
  router.mouseMove({40, 50}); // starts the text drag
  check(router.isDragging() && router.dragSource().isText, "a text drag starts through the router");
  router.mouseMove({130, 40});
  check(field.entered == "dragged words" && std::fabs(field.enteredAt.x - 30.0f) < 0.01f &&
            std::fabs(field.enteredAt.y - 40.0f) < 0.01f,
        "the target is told the text and WHERE it entered, in its own coordinates");
  router.mouseMove({150, 45});
  check(field.moves >= 2, "and every move over it");
  router.mouseMove({250, 45}); // over the digits-only field: refused
  check(field.exits == 1 && digits.entered.empty(), "it leaves; a target that refuses the text is not entered");
  router.mouseMove({160, 70});
  router.mouseUp({160, 70});
  check(field.received == "dragged words" && std::fabs(field.droppedAt.x - 60.0f) < 0.01f,
        "the drop delivers the text at the drop point");

  // An ordinary item drag is NOT offered to a text target.
  TextField onlyText;
  onlyText.setBounds({0, 0, 300, 100});
  Component root2;
  root2.setBounds({0, 0, 300, 100});
  root2.addChild(&onlyText);
  MouseRouter router2(root2);
  router2.mouseDown({10, 10});
  router2.startDrag("modulator 3", nullptr, {10, 10});
  router2.mouseMove({100, 50});
  router2.mouseUp({100, 50});
  check(onlyText.entered.empty() && onlyText.received.empty(),
        "a non-text item drag is not mistaken for text");

  // From outside: the dispatcher, with a target destroyed mid-drag.
  {
    Component outside;
    outside.setBounds({0, 0, 200, 100});
    auto doomed = std::make_unique<TextField>();
    doomed->setBounds({0, 0, 100, 100});
    TextField survivor;
    survivor.setBounds({100, 0, 100, 100});
    outside.addChild(doomed.get());
    outside.addChild(&survivor);
    TextDragDispatcher dispatcher(outside);
    check(dispatcher.dragMove("ext", {10, 10}) && doomed->entered == "ext",
          "TextDragDispatcher finds the target under an external drag");
    doomed.reset();
    check(dispatcher.currentTarget() == nullptr, "a target destroyed mid-drag is forgotten");
    check(dispatcher.dragMove("ext", {150, 10}) && survivor.entered == "ext",
          "and the drag carries on to the next one");
    check(dispatcher.drop("ext", {160, 20}) && survivor.received == "ext" && survivor.exits == 1,
          "the drop closes the enter/exit pair first, then delivers");
    check(!dispatcher.drop("ext", {50, 50}), "a drop over nothing interested is refused");
  }

#if defined(_WIN32)
  // OLE: the data objects, parsed.
  {
    const std::string unicode = "h\xC3\xA9llo w\xC3\xB6rld \xE2\x9C\x93";
    auto* data = new detail::OleTextDataObject(unicode);
    std::string read;
    check(detail::oleReadText(data, &read) && read == unicode,
          "CF_UNICODETEXT round-trips UTF-8 through an OLE data object");
    FORMATETC ansi{CF_TEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    check(data->QueryGetData(&ansi) == S_OK, "CF_TEXT is offered too");
    IEnumFORMATETC* formats = nullptr;
    int count = 0;
    if (SUCCEEDED(data->EnumFormatEtc(DATADIR_GET, &formats)) && formats) {
      FORMATETC f[4];
      ULONG got = 0;
      formats->Next(4, f, &got);
      count = (int) got;
      formats->Release();
    }
    check(count == 2, "EnumFormatEtc lists the two formats");
    std::vector<std::string> noFiles;
    check(!detail::oleReadFiles(data, &noFiles), "and no files");
    data->Release();

    auto* files = new FileDataObject({L"C:\\samples\\kick.wav", L"C:\\samples\\caf\u00e9.wav"});
    std::vector<std::string> paths;
    check(detail::oleReadFiles(files, &paths) && paths.size() == 2 &&
              paths[1] == "C:\\samples\\caf\xC3\xA9.wav",
          "CF_HDROP files are read as UTF-8 paths");

    // The drop target itself, driven as OLE drives it.
    Component tree;
    tree.setBounds({0, 0, 200, 100});
    TextField t;
    t.setBounds({0, 0, 100, 100});
    FileTarget ft;
    ft.setBounds({100, 0, 100, 100});
    tree.addChild(&t);
    tree.addChild(&ft);
    MouseRouter r(tree);
    auto* target = new detail::OleDropTarget(r, tree, [](LONG x, LONG y) {
      return Point((float) x, (float) y); // screen = logical in this test
    });
    auto* text = new detail::OleTextDataObject("from notepad");
    DWORD effect = 0;
    target->DragEnter(text, MK_LBUTTON, POINTL{20, 20}, &effect);
    check(effect == DROPEFFECT_COPY && t.entered == "from notepad",
          "IDropTarget::DragEnter reads the text and offers it to the target (copy cursor)");
    target->DragOver(MK_LBUTTON, POINTL{150, 20}, &effect);
    check(effect == DROPEFFECT_NONE && t.exits == 1, "over something that does not take text: no entry");
    target->Drop(text, 0, POINTL{30, 40}, &effect);
    check(t.received == "from notepad" && effect == DROPEFFECT_COPY, "IDropTarget::Drop delivers it");
    target->DragEnter(files, MK_LBUTTON, POINTL{150, 50}, &effect);
    check(effect == DROPEFFECT_COPY && target->draggedFiles().size() == 2,
          "files arrive through the same target, with hover feedback");
    target->Drop(files, 0, POINTL{150, 50}, &effect);
    check(ft.got.size() == 2, "and are dropped on the file target");
    text->Release();
    files->Release();
    target->Release();
  }

  // A thread the host never put in an apartment is left exactly as it was:
  // opening a window must not make it an STA behind the host's back.
  {
    const detail::Ole32Api& api = detail::ole32();
    APTTYPE before = APTTYPE_CURRENT, after = APTTYPE_CURRENT;
    APTTYPEQUALIFIER q = APTTYPEQUALIFIER_NONE;
    const HRESULT hb = api.coGetApartmentType(&before, &q);
    Component tree;
    tree.setBounds({0, 0, 100, 100});
    NativeWindow w;
    w.open(nullptr, tree, 100, 100);
    const HRESULT ha = api.coGetApartmentType(&after, &q);
    check(hb == ha && before == after, "opening a window leaves the thread's COM apartment alone");
    w.close();
  }

  // Registered on a real window, on a thread the "host" made an STA with OLE
  // -- as every host that does drag and drop has: OLE then says the window
  // already has a target.
  detail::ole32().oleInitialize(nullptr);
  {
    Component tree;
    tree.setBounds({0, 0, 100, 100});
    NativeWindow w;
    if (w.open(nullptr, tree, 100, 100)) {
      auto* other = new detail::OleDropTarget(*w.router(), tree, nullptr);
      const HRESULT again = detail::ole32().registerDragDrop((HWND) w.handle(), other);
      check(again == DRAGDROP_E_ALREADYREGISTERED,
            "the window registers its own IDropTarget (a second registration is refused)");
      if (SUCCEEDED(again)) detail::ole32().revokeDragDrop((HWND) w.handle());
      other->Release();
      w.close();
    }
  }

  // OLE keeps a reference of its own to the target while a drag is over the
  // window, and calls it again after the window closes under the drag. The
  // peer's close() revokes and releases it and deletes the router: the next
  // DragLeave read the freed router (AddressSanitizer: heap-use-after-free in
  // fileDragExit). The router lives on here, so a delivery is SEEN instead.
  {
    Component tree;
    tree.setBounds({0, 0, 100, 100});
    TextField t;
    t.setBounds({0, 0, 100, 100});
    tree.addChild(&t);
    MouseRouter r(tree);
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"late drop", WS_POPUP, 0, 0, 100, 100, nullptr,
                                nullptr, GetModuleHandleW(nullptr), nullptr);
    bool initialised = false;
    detail::OleDropTarget* target = detail::registerOleDropTarget(
        hwnd, r, tree, [](LONG x, LONG y) { return Point((float) x, (float) y); }, &initialised);
    check(target != nullptr, "a target registers on a plain window of an STA thread");
    if (target) {
      target->AddRef(); // what DoDragDrop holds for the length of a drag
      IDropTarget* held = target;
      auto* text = new detail::OleTextDataObject("too late");
      DWORD effect = 0;
      held->DragEnter(text, MK_LBUTTON, POINTL{10, 10}, &effect);
      const bool hovered = effect == DROPEFFECT_COPY && t.entered == "too late";
      detail::revokeOleDropTarget(hwnd, target, initialised); // the window closes
      t.entered.clear();
      held->DragOver(MK_LBUTTON, POINTL{20, 20}, &effect);
      const DWORD overEffect = effect;
      held->DragLeave();
      held->DragEnter(text, MK_LBUTTON, POINTL{10, 10}, &effect);
      held->Drop(text, 0, POINTL{30, 30}, &effect);
      check(hovered && overEffect == DROPEFFECT_NONE && effect == DROPEFFECT_NONE &&
                t.entered.empty() && t.received.empty(),
            "a target OLE still holds after its window closed answers no-drop and delivers nothing");
      held->Release();
      text->Release();
    }
    DestroyWindow(hwnd);
  }
  detail::ole32().oleUninitialize();
#endif
}

// ── 7. Relative positioning ─────────────────────────────────────────────────

static bool approx(float a, float b) { return std::fabs(a - b) < 1e-3f; }
static bool rectIs(const Rect& r, float x, float y, float w, float h) {
  return approx(r.x, x) && approx(r.y, y) && approx(r.w, w) && approx(r.h, h);
}

static void testRelative() {
  section("Relative positioning");
  std::string why;
  RelativeScope empty;
  check(approx((float) RelativeCoordinate("1 + 2 * 3 - (4 - 6) / 2").resolve(empty), 8.0f),
        "precedence and parentheses: 1 + 2*3 - (4-6)/2 = 8");
  check(approx((float) RelativeCoordinate("-.5e1 + 1E2").resolve(empty), 95.0f), "-.5e1 + 1E2 = 95");
  check(approx((float) RelativeCoordinate("min(3, max(-1, abs(-7)))").resolve(empty), 3.0f),
        "min, max and abs");
  for (const char* bad : {"10 +", "(1", "foo(1)", "a..b", "1 2", "min(1)", "", ")", "3 $ 4"}) {
    RelativeCoordinate c(bad);
    char what[96];
    std::snprintf(what, sizeof(what), "\"%s\" is refused with a reason", bad);
    check(!c.isValid() && !c.error().empty(), what);
  }
  RelativeCoordinate divide("1 / (2 - 2)");
  divide.resolve(empty, &why);
  check(why == "division by zero", "division by zero is an error, not an infinity");
  // A chain nests nothing and is as deep as it is long. 300 ones evaluated to
  // 256 with no error, and 2048 of them overflowed a debug build's stack.
  {
    std::string ones = "1";
    for (int i = 1; i < 256; ++i) ones += "+1";
    check(approx((float) RelativeCoordinate(ones).resolve(empty, &why), 256.0f) && why.empty(),
          "256 ones in a chain add up to 256");
    for (int i = 256; i < 2048; ++i) ones += "+1";
    const RelativeCoordinate tooLong(ones);
    check(!tooLong.isValid() && tooLong.resolve(empty, &why) == 0.0 && !why.empty(),
          "2048 ones in a chain are refused, not cut short to a wrong sum");
  }
  const RelativeCoordinate printed("parent.width - (a.right + 5) * 2");
  check(RelativeCoordinate(printed.toString()).resolve(empty, &why) == 0.0 &&
            RelativeCoordinate(printed.toString()).toString() == printed.toString(),
        "toString parses back to the same expression");

  Component parent;
  parent.setBounds({0, 0, 400, 300});
  Component a, b, c, x, y;
  a.setComponentID("a");
  b.setComponentID("b");
  for (Component* k : {&a, &b, &c, &x, &y}) parent.addChild(k);

  RelativeRectangle("10, 10, parent.width - 10, 40").applyToComponent(a);
  check(rectIs(a.bounds(), 10, 10, 380, 30), "\"10, 10, parent.width - 10, 40\" in a 400-wide parent");
  RelativeRectangle("a.right - 100, a.bottom + 5, a.right, a.bottom + 25").applyToComponent(b);
  check(rectIs(b.bounds(), 290, 45, 100, 20), "a sibling positioned against a's edges");
  parent.setSize(600, 300);
  check(rectIs(a.bounds(), 10, 10, 580, 30) && rectIs(b.bounds(), 490, 45, 100, 20),
        "resizing the parent re-applies both -- b follows a, which followed the parent");

  MarkerList markers;
  markers.setMarker("gutter", RelativeCoordinate("parent.width * 0.1"));
  markers.setMarker("inner", RelativeCoordinate("gutter * 2"));
  RelativeRectangle("gutter, inner, parent.width - gutter, inner + 20").applyToComponent(c, {&markers});
  check(rectIs(c.bounds(), 60, 120, 480, 20), "markers, including one defined by another");
  markers.setMarker("gutter", RelativeCoordinate(25.0));
  check(rectIs(c.bounds(), 25, 50, 550, 20), "changing a marker re-applies");

  MarkerList loop;
  loop.setMarker("m1", RelativeCoordinate("m2 + 1"));
  loop.setMarker("m2", RelativeCoordinate("m3 + 1"));
  loop.setMarker("m3", RelativeCoordinate("m1 + 1"));
  RelativeScope loopScope;
  loopScope.parent = &parent;
  loopScope.markers = {&loop};
  RelativeCoordinate("m1").resolve(loopScope, &why);
  check(why.find("circular") != std::string::npos && why.find("m1 -> m2 -> m3 -> m1") != std::string::npos,
        "a marker cycle is detected and named");

  // Two components each positioned against the other's x: a real cycle.
  x.setComponentID("x");
  y.setComponentID("y");
  RelativeRectangle("y.right + 5, 200, y.right + 25, 220").applyToComponent(x);
  RelativeRectangle("x.right + 5, 250, x.right + 25, 270").applyToComponent(y);
  RelativeCoordinatePositionerBase* px = RelativeCoordinatePositionerBase::positionerFor(x);
  RelativeCoordinatePositionerBase* py = RelativeCoordinatePositionerBase::positionerFor(y);
  check(px && py && (px->hasCycle() || py->hasCycle()),
        "a component cycle is detected (and the program is still running)");
  check((px && px->hasCycle() && py && py->hasCycle()), "and both members of the loop report it");

  // A mutual dependency on DIFFERENT axes converges and is not a cycle.
  Component p2;
  p2.setBounds({0, 0, 300, 300});
  Component u, v;
  u.setComponentID("u");
  v.setComponentID("v");
  p2.addChild(&u);
  p2.addChild(&v);
  RelativeRectangle("v.right + 10, 10, v.right + 50, 30").applyToComponent(u);
  RelativeRectangle("10, u.bottom + 10, 60, u.bottom + 30").applyToComponent(v);
  RelativeCoordinatePositionerBase* pu = RelativeCoordinatePositionerBase::positionerFor(u);
  RelativeCoordinatePositionerBase* pv = RelativeCoordinatePositionerBase::positionerFor(v);
  check(pu && pv && !pu->hasCycle() && !pv->hasCycle() && rectIs(u.bounds(), 70, 10, 40, 20) &&
            rectIs(v.bounds(), 10, 40, 50, 20),
        "x from one, y from the other: converges, no cycle reported");

  // A name that does not exist yet starts working when it is added.
  Component p3;
  p3.setBounds({0, 0, 100, 100});
  Component later, follower;
  p3.addChild(&follower);
  RelativeRectangle("later.x, 0, later.right, 10").applyToComponent(follower);
  RelativeCoordinatePositionerBase* pf = RelativeCoordinatePositionerBase::positionerFor(follower);
  check(pf && pf->lastError().find("later") != std::string::npos, "an unknown sibling is reported");
  later.setComponentID("later");
  later.setBounds({20, 0, 30, 30});
  p3.addChild(&later);
  check(rectIs(follower.bounds(), 20, 0, 30, 10) && pf && pf->lastError().empty(),
        "and resolved once it is added");
  later.setBounds({40, 0, 10, 30});
  check(rectIs(follower.bounds(), 40, 0, 10, 10), "then followed as it moves");

  // A positioned component destroyed: its positioner goes with it.
  {
    Component temp;
    p3.addChild(&temp);
    RelativeRectangle("0, 0, parent.width, 5").applyToComponent(temp);
    check(RelativeCoordinatePositionerBase::positionerFor(temp) != nullptr, "a positioner is installed");
  }
  p3.setSize(120, 100); // would re-apply a dangling positioner if it survived

  // Point, parallelogram, path.
  RelativeScope s;
  s.parent = &parent;
  const Point pt = RelativePoint("parent.width / 2, parent.height - 10").resolve(s);
  check(approx(pt.x, 300.0f) && approx(pt.y, 290.0f), "RelativePoint \"parent.width / 2, parent.height - 10\"");
  RelativeParallelogram para(RelativePoint("10, 10"), RelativePoint("110, 20"), RelativePoint("0, 60"));
  const Transform t = para.transformFrom(Rect(0, 0, 100, 50), s);
  const Point corner = t.apply({100, 50});
  check(approx(corner.x, 100.0f) && approx(corner.y, 70.0f),
        "RelativeParallelogram maps a rectangle's far corner to tr + bl - tl");
  RelativePointPath path;
  path.startNewSubPath(RelativePoint("0, 0"));
  path.lineTo(RelativePoint("parent.width, 0"));
  path.lineTo(RelativePoint("parent.width, parent.height"));
  path.closeSubPath();
  check(path.containsAnyDynamicPoints() && !path.createPath(s, &why).isEmpty() && why.empty(),
        "RelativePointPath builds a Path from its points");
}

// ── 8. ComponentBuilder ─────────────────────────────────────────────────────

static int g_liveLabels = 0;
struct DataLabel : Component {
  std::string text;
  int updates = 0;
  DataLabel() { ++g_liveLabels; }
  ~DataLabel() override { --g_liveLabels; }
};

struct LabelHandler : ComponentBuilder::TypeHandler {
  LabelHandler() : TypeHandler("Label") {}
  std::unique_ptr<Component> addNewComponentFromState(const JsonValue&, Component*) override {
    return std::make_unique<DataLabel>();
  }
  void updateComponentFromState(Component& c, const JsonValue& state) override {
    auto& l = static_cast<DataLabel&>(c);
    l.text = state["text"].asString();
    ++l.updates;
  }
};

struct PanelHandler : ComponentBuilder::TypeHandler {
  PanelHandler() : TypeHandler("Panel") {}
  std::unique_ptr<Component> addNewComponentFromState(const JsonValue&, Component*) override {
    return std::make_unique<Component>();
  }
  void updateComponentFromState(Component&, const JsonValue&) override {}
};

static void testBuilder() {
  section("ComponentBuilder");
  const char* first = R"({"type":"Panel","id":"root","bounds":[0,0,400,300],"children":[
      {"type":"Label","id":"title","text":"Hello","bounds":[10,10,200,20]},
      {"type":"Label","id":"status","text":"Ready","bounds":"10, parent.height - 30, parent.width - 10, parent.height - 10"},
      {"type":"Panel","id":"box","bounds":[10,40,100,100],"children":[
         {"type":"Label","id":"inner","text":"In"}]}]})";
  std::string err;
  const JsonValue state = JsonValue::parse(first, &err);
  check(err.empty(), "the test tree parses");
  ComponentBuilder builder(state);
  builder.registerTypeHandler(std::make_unique<LabelHandler>());
  builder.registerTypeHandler(std::make_unique<PanelHandler>());
  std::vector<std::string> errors;
  builder.onError = [&](const std::string& e) { errors.push_back(e); };

  Component* root = builder.getManagedComponent();
  check(root && root->numChildren() == 3 && root->componentID() == "root", "builds the root and its children");
  auto* title = static_cast<DataLabel*>(builder.findComponentByID("title"));
  auto* status = static_cast<DataLabel*>(builder.findComponentByID("status"));
  auto* inner = static_cast<DataLabel*>(builder.findComponentByID("inner"));
  check(title && title->text == "Hello" && rectIs(title->bounds(), 10, 10, 200, 20),
        "a handler made and filled each node; bounds applied");
  check(status && rectIs(status->bounds(), 10, 270, 380, 20),
        "a RelativeRectangle bounds string is applied live");
  check(inner && inner->parent() == builder.findComponentByID("box"), "nested children nest");
  check(builder.componentsCreated() == 5 && g_liveLabels == 3, "five components, three of them labels");

  // Change one label's text, move one child, add one, remove one, reorder.
  const char* second = R"({"type":"Panel","id":"root","bounds":[0,0,400,300],"children":[
      {"type":"Panel","id":"box","bounds":[10,40,100,100],"children":[
         {"type":"Label","id":"inner","text":"In"}]},
      {"type":"Label","id":"title","text":"Goodbye","bounds":[10,10,200,20]},
      {"type":"Label","id":"extra","text":"New"}]})";
  const int updatesBefore = builder.componentsUpdated();
  const int titleUpdates = title->updates, innerUpdates = inner->updates;
  builder.setState(JsonValue::parse(second));
  check(builder.findComponentByID("title") == title && title->text == "Goodbye" &&
            title->updates == titleUpdates + 1,
        "a changed node keeps its component and is updated");
  check(builder.findComponentByID("inner") == inner && inner->updates == innerUpdates,
        "an unchanged node is not touched");
  check(builder.findComponentByID("status") == nullptr && g_liveLabels == 3,
        "a removed node's component is destroyed (status gone, extra added)");
  check(builder.componentsUpdated() == updatesBefore + 2, "exactly two updates: title and the new extra");
  check(root->child(0) == builder.findComponentByID("box") && root->child(1) == title &&
            root->child(2) == builder.findComponentByID("extra"),
        "children reordered to match the data's z-order");
  root->setSize(800, 300); // the destroyed status's relative positioner must be gone

  // Type change recreates; an unknown type is reported and skipped.
  const char* third = R"({"type":"Panel","id":"root","children":[
      {"type":"Panel","id":"title"},
      {"type":"Gizmo","id":"what"}]})";
  const int createdBefore = builder.componentsCreated();
  builder.setState(JsonValue::parse(third));
  Component* retyped = builder.findComponentByID("title");
  // Not compared by address: the new component may well be allocated where
  // the destroyed label was.
  check(retyped && g_liveLabels == 0 && builder.componentsCreated() == createdBefore + 1,
        "a node whose type changed is rebuilt (the label destroyed, one new component)");
  check(!errors.empty() && errors.back().find("Gizmo") != std::string::npos,
        "an unknown type is reported, not fatal");
}

// ── 9. ComponentMovementWatcher ─────────────────────────────────────────────

static void testMovementWatcher() {
  section("ComponentMovementWatcher");
  Component root, panel, other, child;
  root.setBounds({0, 0, 400, 400});
  panel.setBounds({10, 10, 200, 200});
  other.setBounds({250, 250, 100, 100});
  child.setBounds({5, 5, 50, 50});
  root.addChild(&panel);
  root.addChild(&other);
  panel.addChild(&child);
  ComponentMovementWatcher watcher(&child);
  int moves = 0, resizes = 0, peers = 0, visibility = 0;
  watcher.onMovedOrResized = [&](bool moved, bool resized) {
    moves += moved ? 1 : 0;
    resizes += resized ? 1 : 0;
  };
  watcher.onPeerChanged = [&] { ++peers; };
  watcher.onVisibilityChanged = [&] { ++visibility; };

  panel.setBounds({20, 10, 200, 200});
  check(moves == 1 && resizes == 0, "a PARENT moving moves the component on screen");
  panel.setSize(300, 300);
  check(moves == 1 && resizes == 0, "a parent resized without moving the child: nothing");
  child.setSize(60, 60);
  check(moves == 1 && resizes == 1, "the component resized");
  root.setBounds({0, 5, 400, 400});
  check(moves == 1, "the ROOT moving is not a move within the window");
  panel.setVisible(false);
  check(visibility == 1, "an ancestor hidden");
  panel.setVisible(true);
  check(visibility == 2, "and shown");
  {
    MouseRouter router(root);
    check(peers == 1, "the tree put into a window (a router appears): peer changed");
  }
  check(peers == 2, "and taken out: peer changed again");
  Point reported;
  watcher.onMovedOrResized = [&](bool moved, bool resized) {
    moves += moved ? 1 : 0;
    resizes += resized ? 1 : 0;
    reported = child.localToRoot({0, 0});
  };
  // Re-parented to (250, 250). addChild takes it out of the old parent first,
  // and the watcher sees that intermediate state too -- what matters is that
  // it moved, and that the last report is where it ended up.
  const int movesBefore = moves;
  other.addChild(&child);
  check(moves > movesBefore && approx(reported.x, 255.0f) && approx(reported.y, 255.0f),
        "re-parenting to somewhere else is reported as a move, ending at the new place");
  const int afterReparent = moves;
  panel.setBounds({0, 0, 50, 50});
  check(moves == afterReparent, "and the old parent is no longer watched");
  other.setBounds({200, 250, 100, 100});
  check(moves == afterReparent + 1, "the new one is");
  {
    auto temp = std::make_unique<Component>();
    temp->setBounds({0, 0, 10, 10});
    ComponentMovementWatcher dying(temp.get());
    temp.reset();
    check(dying.getComponent() == nullptr, "a watched component destroyed first is forgotten");
  }
}

// ── 10. Springs ─────────────────────────────────────────────────────────────

static void testSpring() {
  section("SpringEasingOptions");
  const SpringEasingOptions defaults;
  check(defaults.getFrequency() == 3.0f && defaults.getAttenuation() == 3.0f &&
            defaults.getExtraAttenuationRange() == 0.25f,
        "the reference's defaults: frequency 3, attenuation 3, range 0.25");
  check(defaults(0.0) == 0.0 && defaults(1.0) == 1.0, "starts at 0 and ends exactly at 1");
  // f=3, a=3: omega_d = 6 pi, decay rate 3: first peak at t = 1/6, value 1 + e^-0.5.
  check(std::fabs(defaults.peakTime() - 1.0 / 6.0) < 1e-12, "peak at t = 1/(2f) = 1/6");
  check(std::fabs(defaults(1.0 / 6.0) - (1.0 + std::exp(-0.5))) < 1e-9,
        "peak value 1 + e^(-a/2f) = 1.6065");

  // A physical spring: m 1, k 100, c 4 -> zeta 0.2, omega0 10.
  const SpringEasingOptions phys = SpringEasingOptions::fromPhysical(1.0, 100.0, 4.0, 2.0);
  check(std::fabs(phys.dampingRatio() - 0.2) < 1e-12, "fromPhysical: zeta = c / 2 sqrt(km) = 0.2");
  const double expected = std::exp(-3.141592653589793 * 0.2 / std::sqrt(1.0 - 0.04));
  check(std::fabs(phys.overshoot() - expected) < 1e-12, "textbook overshoot exp(-pi z / sqrt(1-z^2)) = 0.5266");
  double peak = 0.0, peakAt = 0.0;
  for (int i = 0; i <= 20000; ++i) {
    const double t = 0.7 * i / 20000.0;
    if (phys(t) > peak) {
      peak = phys(t);
      peakAt = t;
    }
  }
  const double tPeak = 3.141592653589793 / (10.0 * std::sqrt(0.96)) / 2.0; // seconds / duration
  std::printf("       sampled peak %.6f at t=%.5f (expected %.6f at %.5f)\n", peak, peakAt,
              1.0 + expected, tPeak);
  check(std::fabs(peak - (1.0 + expected)) < 1e-6 && std::fabs(peakAt - tPeak) < 1e-3,
        "the sampled curve overshoots by exactly that, when it should");
  check(std::fabs(SpringEasingOptions::settlingTime(1.0, 100.0, 4.0) - std::log(1000.0) / 2.0) < 1e-12,
        "settlingTime: ln(1/0.001) / (zeta omega0) = 3.454 s");

  const SpringEasingOptions critical = SpringEasingOptions::fromPhysical(1.0, 100.0, 20.0, 1.0);
  bool monotonic = true;
  double last = 0.0;
  for (int i = 1; i <= 1000; ++i) {
    const double v = critical(i / 1000.0);
    if (v < last - 1e-12 || v > 1.0 + 1e-12) monotonic = false;
    last = v;
  }
  check(monotonic && critical.overshoot() == 0.0, "critically damped: no overshoot, monotonic");
  const SpringEasingOptions over = SpringEasingOptions::fromPhysical(1.0, 100.0, 60.0, 1.0);
  check(over(0.5) > 0.0 && over(0.5) < critical(0.5) && over(1.0) == 1.0,
        "overdamped: slower than critical, and still lands");
  const SpringEasingOptions bouncy = SpringEasingOptions::fromResponse(0.5, 0.3, 1.0);
  check(std::fabs(bouncy.dampingRatio() - 0.7) < 1e-12, "fromResponse: bounce 0.3 is zeta 0.7");

  // Through ValueAnimator: overshoots, lands exactly.
  ValueAnimator animator;
  std::vector<double> values;
  animator.animateWithSpring([&](double v) { values.push_back(v); }, 100.0, 200.0, 2.0, 0.0, phys);
  for (int frame = 1; frame <= 130; ++frame) animator.tick(frame / 60.0);
  double highest = 0.0;
  for (double v : values) highest = v > highest ? v : highest;
  check(!values.empty() && values.back() == 200.0 && !animator.isAnimating(), "lands exactly on 200");
  check(highest > 150.0 + 100.0 * 0.5 && highest < 100.0 + 100.0 * (1.0 + expected) + 1e-6,
        "and overshot past 250 on the way, but no further than the formula allows");

  Component k;
  k.setBounds({0, 0, 10, 10});
  Animator bounds;
  bounds.animateBounds(&k, Rect(100, 0, 10, 10), 1.0, 0.0, defaults);
  float furthest = 0.0f;
  for (int frame = 1; frame <= 70; ++frame) {
    bounds.tick(frame / 60.0);
    furthest = k.bounds().x > furthest ? k.bounds().x : furthest;
  }
  check(furthest > 150.0f && k.bounds().x == 100.0f, "Animator moves bounds with a spring and lands");

  // A component deleted mid-flight -- a notice closed while it slides in --
  // without anybody calling cancel(). The next tick set the bounds of freed
  // memory and called its resized().
  {
    Component screen;
    screen.setBounds({0, 0, 400, 300});
    Animator slide;
    Component* notice = new Component();
    notice->setBounds({0, 300, 200, 40});
    screen.addChild(notice);
    slide.animateBounds(notice, Rect(0, 260, 200, 40), 0.3, 0.0);
    slide.tick(0.1);
    delete notice;
    const bool moved = slide.tick(0.2);
    check(!moved && slide.count() == 0 && !slide.isAnimating(),
          "a component deleted mid-flight is dropped at the next tick, not moved");
  }
}

// ── 11. AnimatedPosition and kinetic scrolling ──────────────────────────────

static void testMomentum() {
  section("AnimatedPosition (momentum scrolling)");
  double now = 0.0;
  AnimatedPosition<AnimatedPositionBehaviours::ContinuousWithMomentum> pos;
  pos.setClock([&now] { return now; });
  pos.beginDrag();
  for (int i = 1; i <= 10; ++i) {
    now = i * 0.01;
    pos.drag(10.0 * i); // 10 units per 10 ms: 1000 units/s
  }
  const double v0 = pos.releaseVelocity();
  check(std::fabs(v0 - 1000.0) < 0.5, "release velocity measured as 1000/s (smoothed over the drag)");
  pos.endDrag();
  const double start = pos.getPosition();
  // Friction 0.08 per 60th of a second is exp(-lambda t), lambda = -60 ln 0.92.
  const double lambda = -60.0 * std::log(0.92);
  now += 1.0 / 60.0;
  pos.update(now);
  check(std::fabs((pos.getPosition() - start) - v0 * (1.0 - 0.92) / lambda) < 1e-9,
        "first frame: v0 (1 - 0.92) / lambda, the exact integral of the friction");
  int frames = 1;
  while (pos.update(now += 1.0 / 60.0)) ++frames;
  // The whole coast is v0 / lambda, less the tail cut off below the minimum
  // velocity (0.05 / lambda, a hundredth of a unit).
  std::printf("       coasted %.3f units in %d frames (v0 / lambda = %.3f)\n",
              pos.getPosition() - start, frames, v0 / lambda);
  check(std::fabs((pos.getPosition() - start) - v0 / lambda) < 0.05 / lambda + 1e-9 &&
            !pos.isAnimating(),
        "coasts v0 / lambda and stops below the minimum velocity");

  // Frame-rate independence: the same fling at 30 and 144 Hz.
  auto coastAt = [](double hz) {
    double t = 0.0;
    AnimatedPosition<AnimatedPositionBehaviours::ContinuousWithMomentum> p;
    p.setClock([&t] { return t; });
    p.beginDrag();
    for (int i = 1; i <= 10; ++i) {
      t = i * 0.01;
      p.drag(10.0 * i);
    }
    p.endDrag();
    const double s = p.getPosition();
    for (int i = 0; i < 2000 && p.update(t += 1.0 / hz); ++i) {
    }
    return p.getPosition() - s;
  };
  const double at30 = coastAt(30.0), at144 = coastAt(144.0);
  std::printf("       coast at 30 Hz %.2f, at 144 Hz %.2f\n", at30, at144);
  check(std::fabs(at30 - at144) < 0.02, "the coast is the same length at 30 and 144 Hz");

  // Limits stop it.
  AnimatedPosition<AnimatedPositionBehaviours::ContinuousWithMomentum> limited;
  limited.setClock([&now] { return now; });
  limited.setLimits(0.0, 50.0);
  limited.beginDrag();
  for (int i = 1; i <= 10; ++i) {
    now += 0.01;
    limited.drag(4.0 * i);
  }
  limited.endDrag();
  while (limited.update(now += 1.0 / 60.0)) {
  }
  check(limited.getPosition() == 50.0, "a coast stops at the limit");

  // Snapping.
  AnimatedPosition<AnimatedPositionBehaviours::SnapToPageBoundaries> page;
  page.setClock([&now] { return now; });
  page.setPosition(2.3);
  page.beginDrag();
  page.endDrag(); // no velocity
  while (page.update(now += 1.0 / 60.0)) {
  }
  check(page.getPosition() == 2.0, "released at 2.3 with no speed: snaps back to page 2");
  page.behaviour.releasedWithVelocity(2.3, 5.0);
  check(page.behaviour.targetSnapPosition == 3.0, "flicked forward at 2.3: on to page 3");
  page.behaviour.releasedWithVelocity(2.7, -5.0);
  check(page.behaviour.targetSnapPosition == 2.0, "flicked back at 2.7: back to page 2");
  page.behaviour.releasedWithVelocity(2.7, 0.5);
  check(page.behaviour.targetSnapPosition == 3.0, "slow at 2.7: the nearest, page 3");

  // A Viewport scrolled by a finger, with momentum.
  Component root;
  root.setBounds({0, 0, 200, 200});
  Viewport viewport;
  viewport.setBounds({0, 0, 200, 200});
  Component content;
  content.setBounds({0, 0, 190, 2000});
  Probe row;
  row.setBounds({0, 0, 190, 40});
  content.addChild(&row);
  viewport.setViewedComponent(&content);
  root.addChild(&viewport);
  MouseRouter router(root);
  ViewportDragScroller scroller(viewport);
  scroller.setTicksFromVBlank(false);
  scroller.setClock([&now] { return now; });
  PointerInput finger;
  finger.type = MouseInputSourceType::Touch;
  router.setPointerInput(finger);
  router.mouseDown({100, 20}); // on the row: the drag still scrolls
  for (int i = 1; i <= 10; ++i) {
    now += 0.01;
    router.mouseMove({100, 20.0f - 10.0f * (float) i}); // finger moves UP 100 px
  }
  check(scroller.isScrolling() && std::fabs(viewport.viewPositionY() - 100.0f) < 0.5f,
        "dragging the content up 100 px scrolls the view down 100 px");
  router.mouseUp({100, -80});
  router.resetPointerInput();
  const float released = viewport.viewPositionY();
  int ticks = 0;
  while (scroller.tick(now += 1.0 / 60.0) && ticks < 2000) ++ticks;
  std::printf("       released at %.1f, coasted to %.1f in %d frames\n", released,
              viewport.viewPositionY(), ticks);
  check(viewport.viewPositionY() > released + 100.0f && !scroller.isCoasting(),
        "after release it keeps scrolling, decelerates and stops");
  // A mouse drag in nonHover mode means what it always meant.
  const float before = viewport.viewPositionY();
  router.mouseDown({100, 100});
  router.mouseMove({100, 20});
  router.mouseUp({100, 20});
  check(viewport.viewPositionY() == before, "a MOUSE drag does not scroll in nonHover mode");
  scroller.setScrollOnDragMode(ViewportDragScroller::ScrollOnDragMode::all);
  router.mouseDown({100, 100});
  router.mouseMove({100, 50});
  router.mouseUp({100, 50});
  check(viewport.viewPositionY() > before, "but does in 'all' mode");
}

// ── 12. VBlankAttachment ────────────────────────────────────────────────────

#if defined(_WIN32)
/** A message loop as a real UI thread runs one: asleep until a message
 *  arrives, then straight back to it -- not a poll, which would itself limit
 *  how often a refresh could be delivered. */
static void pumpUntil(std::chrono::steady_clock::time_point until) {
  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= until) return;
    const DWORD wait = (DWORD) std::chrono::duration_cast<std::chrono::milliseconds>(until - now).count();
    MsgWaitForMultipleObjects(0, nullptr, FALSE, wait + 1, QS_ALLINPUT);
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
  }
}
#endif

static void testVBlank() {
  section("VBlankAttachment");
  const double rate = VBlankAttachment::displayRefreshRate();
  std::printf("       display refresh rate reported: %.2f Hz\n", rate);
  check(rate >= 20.0 && rate <= 500.0, "a plausible refresh rate");
#if defined(_WIN32)
  Component root;
  root.setBounds({0, 0, 10, 10});
  Component inWindow;
  root.addChild(&inWindow);
  Component nowhere;
  int fired = 0, firedNowhere = 0;
  double firstStamp = -1.0, lastStamp = -1.0;
  {
    MouseRouter router(root); // puts the tree "in a window" for the attachment
    VBlankAttachment attachment(&inWindow, [&](double stamp) {
      if (firstStamp < 0.0) firstStamp = stamp;
      lastStamp = stamp;
      ++fired;
    });
    VBlankAttachment orphan(&nowhere, [&](double) { ++firedNowhere; });
    // Let the first few frames settle, then measure for a second.
    const auto settle = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
    pumpUntil(settle);
    // Up to six one-second windows. Too MANY callbacks is a bug in any
    // window; too few is a message thread kept from its loop -- a run beside
    // a full build measured 43 Hz on a 60 Hz display, and one beside four
    // missed it three windows running -- so one window at the display's rate
    // is enough.
    bool atRate = false, tooFast = false;
    for (int window = 0; window < 6 && !atRate && !tooFast; ++window) {
      fired = 0;
      firstStamp = -1.0;
      const auto start = std::chrono::steady_clock::now();
      const auto until = start + std::chrono::milliseconds(1000);
      pumpUntil(until);
      const double elapsed =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
      const double measured = fired / elapsed;
      const double stampRate = fired > 1 ? (fired - 1) / (lastStamp - firstStamp) : 0.0;
      std::printf("       backend: %s; %d callbacks in %.3f s = %.2f Hz (by timestamps %.2f Hz)\n",
                  VBlankAttachment::backendName(), fired, elapsed, measured, stampRate);
      tooFast = measured >= rate * 1.4;
      atRate = measured > rate * 0.6 && !tooFast;
    }
    check(atRate && !tooFast, "callbacks arrive at the display's rate (within 40 %), never faster");
    check(firedNowhere == 0, "an attachment whose component is in no window is not called");

    // A callback that destroys its own attachment.
    std::unique_ptr<VBlankAttachment> once;
    int onceFired = 0;
    once = std::make_unique<VBlankAttachment>(&inWindow, [&](double) {
      ++onceFired;
      once.reset();
    });
    const auto brief = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    pumpUntil(brief);
    check(onceFired == 1 && !once, "an attachment may destroy itself from its own callback");
  }
  check(std::string(VBlankAttachment::backendName()) == "none", "the refresh thread stops with the last attachment");
  // And its window with it. Kept, it outlived a plugin: after the host
  // unloaded the DLL the window's procedure was in freed code, and the next
  // message to it crashed the host (a DLL probe: SendMessage to the orphan
  // faulted). Counted among this process's message-only windows.
  {
    auto vblankWindows = []() {
      int n = 0;
      HWND w = nullptr;
      while ((w = FindWindowExW(HWND_MESSAGE, w, nullptr, nullptr)) != nullptr) {
        wchar_t cls[64] = {0};
        GetClassNameW(w, cls, 64);
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid == GetCurrentProcessId() && std::wcsncmp(cls, L"SonoreVBlank_", 13) == 0) ++n;
      }
      return n;
    };
    check(vblankWindows() == 0, "no refresh window is left once the last attachment has gone");
    MouseRouter router(root);
    int again = 0;
    {
      VBlankAttachment next(&inWindow, [&](double) { ++again; });
      pumpUntil(std::chrono::steady_clock::now() + std::chrono::milliseconds(300));
      check(vblankWindows() == 1, "a new attachment makes one again");
    }
    check(again > 0 && vblankWindows() == 0, "which delivers, and goes with it");
  }
#else
  std::printf("       (refresh delivery measured on Windows only in this test)\n");
#endif
}

int main() {
  // Unbuffered, so a crash shows the last check that passed.
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("gui infrastructure\n");
  testCachedImage();
  testPaintDiagnostics();
  testLooks();
  testDrawable();
  testPointerInput();
  testInactivity();
  testTextDrag();
  testRelative();
  testBuilder();
  testMovementWatcher();
  testSpring();
  testMomentum();
  testVBlank();
  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  std::printf("%s\n", g_failures == 0 ? "GUI INFRA TESTS PASSED" : "GUI INFRA TESTS FAILED");
  return g_failures == 0 ? 0 : 1;
}
