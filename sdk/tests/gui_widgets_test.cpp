// SPDX-License-Identifier: Apache-2.0
// The application furniture: a colour picker, toolbars, menu bars, a drawer,
// documents, a key-mapping editor and the file chooser components.
//
// Every widget here is driven the way a window peer drives it -- through a
// MouseRouter, with presses, moves, releases and keys at root coordinates --
// and rendered into a Bitmap. Positions come from each widget's own geometry
// accessors, so the tests do not depend on which font the machine has.
//
//   gui_widgets_test
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <sonore/commands.h>
#include <sonore/gfx/bitmap.h>
#include <sonore/gfx/colour_selector.h>
#include <sonore/gfx/containers.h>
#include <sonore/gfx/file_chooser.h>
#include <sonore/gfx/graphics.h>
#include <sonore/gfx/key_mapping_editor.h>
#include <sonore/gfx/menu_bar.h>
#include <sonore/gfx/multi_document.h>
#include <sonore/gfx/side_panel.h>
#include <sonore/gfx/text_area.h>
#include <sonore/gfx/text_editor.h>
#include <sonore/gfx/toolbar.h>
#include <sonore/gfx/treeview.h>
#include <sonore/gfx/truetype.h>
#include <sonore/gfx/widgets2.h>

#include "temp_path.h"

using namespace sonore;
using namespace sonore::gfx;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what) {
  ++g_checks;
  if (ok) {
    std::printf("  ok   %s\n", what);
  } else {
    ++g_failures;
    std::printf("  FAIL %s\n", what);
  }
}

bool approx(float a, float b, float tolerance = 0.01f) { return std::fabs(a - b) <= tolerance; }

bool sameColour(Colour a, Colour b, int tolerance) {
  return std::abs((int) a.r - (int) b.r) <= tolerance && std::abs((int) a.g - (int) b.g) <= tolerance &&
         std::abs((int) a.b - (int) b.b) <= tolerance && std::abs((int) a.a - (int) b.a) <= tolerance;
}

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

Font g_font;

// ── Driving a tree the way a peer does ──────────────────────────────────────

Point centreOf(const Component& c, const Rect& local) { return c.localToRoot(local.centre()); }
Point centreOf(const Component& c) { return centreOf(c, c.localBounds()); }

void click(MouseRouter& router, Point at, int clicks = 1) {
  router.mouseMove(at);
  router.mouseDown(at, clicks);
  router.mouseUp(at);
}

void drag(MouseRouter& router, Point from, Point to, int steps = 6) {
  router.mouseMove(from);
  router.mouseDown(from);
  for (int i = 1; i <= steps; ++i) {
    const float t = (float) i / (float) steps;
    router.mouseMove({from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t});
  }
  router.mouseUp(to);
}

bool press(MouseRouter& router, int code, bool ctrl = false, bool shift = false, bool alt = false) {
  KeyPress k;
  k.keyCode = code;
  k.ctrlDown = ctrl;
  k.shiftDown = shift;
  k.altDown = alt;
  return router.keyPressed(k);
}

bool type(MouseRouter& router, uint32_t c, bool ctrl = false, bool shift = false, bool alt = false) {
  KeyPress k;
  k.character = c;
  k.ctrlDown = ctrl;
  k.shiftDown = shift;
  k.altDown = alt;
  return router.keyPressed(k);
}

void typeText(MouseRouter& router, const std::string& text) {
  for (unsigned char c : text) type(router, c);
}

Bitmap render(Component& root, Colour ground = Colour(40, 40, 40)) {
  Bitmap bmp((int) root.bounds().w, (int) root.bounds().h);
  Graphics g(bmp);
  g.fillAll(ground);
  root.paintTree(g);
  return bmp;
}

Colour pixel(const Bitmap& bmp, Point p) { return bmp.pixelAt((int) p.x, (int) p.y).toStraight(); }

/** How many pixels differ from the ground: a render that drew nothing is a
 *  render that tested nothing. */
int inkedPixels(const Bitmap& bmp, Colour ground) {
  int n = 0;
  for (int y = 0; y < bmp.height(); y += 2)
    for (int x = 0; x < bmp.width(); x += 2)
      if (!sameColour(pixel(bmp, {(float) x, (float) y}), ground, 2)) ++n;
  return n;
}

// ── A PNG, written by hand ──────────────────────────────────────────────────
//
// The SDK decodes PNG and has no encoder, so the preview test writes one: an
// RGBA image in a zlib stream of STORED blocks, with the CRCs and the Adler
// sum computed here.

uint32_t crc32Of(const uint8_t* data, size_t n, uint32_t crc = 0xFFFFFFFFu) {
  for (size_t i = 0; i < n; ++i) {
    crc ^= data[i];
    for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return crc;
}

void putBE32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back((uint8_t) (v >> 24));
  out.push_back((uint8_t) (v >> 16));
  out.push_back((uint8_t) (v >> 8));
  out.push_back((uint8_t) v);
}

void chunk(std::vector<uint8_t>& out, const char* type4, const std::vector<uint8_t>& body) {
  putBE32(out, (uint32_t) body.size());
  std::vector<uint8_t> typed(type4, type4 + 4);
  typed.insert(typed.end(), body.begin(), body.end());
  out.insert(out.end(), typed.begin(), typed.end());
  putBE32(out, crc32Of(typed.data(), typed.size()) ^ 0xFFFFFFFFu);
}

std::vector<uint8_t> makePng(int w, int h, uint32_t rgba) {
  std::vector<uint8_t> raw;
  for (int y = 0; y < h; ++y) {
    raw.push_back(0); // filter: none
    for (int x = 0; x < w; ++x) {
      raw.push_back((uint8_t) (rgba >> 24));
      raw.push_back((uint8_t) (rgba >> 16));
      raw.push_back((uint8_t) (rgba >> 8));
      raw.push_back((uint8_t) rgba);
    }
  }
  std::vector<uint8_t> z = {0x78, 0x01, 0x01}; // zlib header, final stored block
  z.push_back((uint8_t) (raw.size() & 0xFF));
  z.push_back((uint8_t) (raw.size() >> 8));
  z.push_back((uint8_t) (~raw.size() & 0xFF));
  z.push_back((uint8_t) ((~raw.size() >> 8) & 0xFF));
  z.insert(z.end(), raw.begin(), raw.end());
  uint32_t a = 1, b = 0;
  for (uint8_t v : raw) {
    a = (a + v) % 65521u;
    b = (b + a) % 65521u;
  }
  putBE32(z, (b << 16) | a);

  std::vector<uint8_t> png = {137, 80, 78, 71, 13, 10, 26, 10};
  std::vector<uint8_t> ihdr;
  putBE32(ihdr, (uint32_t) w);
  putBE32(ihdr, (uint32_t) h);
  ihdr.push_back(8); // bit depth
  ihdr.push_back(6); // RGBA
  ihdr.push_back(0);
  ihdr.push_back(0);
  ihdr.push_back(0);
  chunk(png, "IHDR", ihdr);
  chunk(png, "IDAT", z);
  chunk(png, "IEND", {});
  return png;
}

void writeFile(const std::string& path, const std::vector<uint8_t>& bytes) {
  std::FILE* f = pathio::open(path.c_str(), "wb");
  if (!f) return;
  if (!bytes.empty()) std::fwrite(bytes.data(), 1, bytes.size(), f);
  std::fclose(f);
}

const char* kIcon = "<svg viewBox=\"0 0 24 24\"><path d=\"M4 4 L20 12 L4 20 Z\"/></svg>";

// ═══════════════════════════════════════════════════════════════════════════
// ColourSelector
// ═══════════════════════════════════════════════════════════════════════════

void testColourSelector() {
  std::printf("ColourSelector\n");

  // Known vectors for the conversion the square is drawn from.
  check(detail::selectorHsvToColour(0.0f, 1.0f, 1.0f) == Colour(255, 0, 0), "hsv(0,1,1) is red");
  check(detail::selectorHsvToColour(1.0f / 3.0f, 1.0f, 1.0f) == Colour(0, 255, 0), "hsv(120deg,1,1) is green");
  check(detail::selectorHsvToColour(2.0f / 3.0f, 1.0f, 1.0f) == Colour(0, 0, 255), "hsv(240deg,1,1) is blue");
  check(detail::selectorHsvToColour(0.5f, 0.5f, 1.0f) == Colour(128, 255, 255), "hsv(180deg,0.5,1) is (128,255,255)");
  check(detail::selectorHsvToColour(0.0f, 0.0f, 0.5f) == Colour(128, 128, 128), "zero saturation is grey");
  {
    float h = 0.9f, s = 0.0f, v = 0.0f;
    detail::selectorColourToHsv(Colour(255, 128, 0), h, s, v);
    check(approx(h * 360.0f, 30.1f, 0.2f) && approx(s, 1.0f) && approx(v, 1.0f), "orange reads back as 30 degrees, full");
    float h2 = 0.42f, s2 = 0.7f, v2 = 0.3f;
    detail::selectorColourToHsv(Colour(0, 0, 0), h2, s2, v2);
    check(h2 == 0.42f && s2 == 0.7f && v2 == 0.0f, "black keeps the hue and saturation it had");
  }
  Colour parsed;
  check(detail::selectorParseHex("#3366cc", parsed) && parsed == Colour(0x33, 0x66, 0xCC, 0xFF), "hex #3366cc parses opaque");
  check(detail::selectorParseHex("80FF0000", parsed) && parsed == Colour(255, 0, 0, 0x80), "eight digits are AARRGGBB");
  check(detail::selectorParseHex("0xf0a", parsed) && parsed == Colour(0xFF, 0x00, 0xAA), "three digits double up");
  Colour untouched(1, 2, 3);
  check(!detail::selectorParseHex("12G456", untouched) && untouched == Colour(1, 2, 3), "a bad digit leaves the colour alone");
  check(!detail::selectorParseHex("1234", untouched), "four digits is not a colour");

  Widget root;
  root.setFont(g_font);
  root.setBounds({0.0f, 0.0f, 500.0f, 520.0f});
  ColourSelector sel;
  root.addChild(&sel);
  sel.setBounds({10.0f, 10.0f, 300.0f, 380.0f});
  MouseRouter router(root);
  int changes = 0;
  Colour last;
  sel.onChange = [&](Colour c) {
    ++changes;
    last = c;
  };

  // Clicking the square picks saturation across and brightness down, at the
  // hue the strip holds.
  sel.setHSV(2.0f / 3.0f, 0.2f, 0.3f, false);
  check(changes == 0, "a quiet setHSV is not reported");
  click(router, sel.localToRoot(sel.colourSpacePoint(0.75f, 0.6f)));
  check(sameColour(sel.currentColour(), detail::selectorHsvToColour(2.0f / 3.0f, 0.75f, 0.6f), 1),
        "a click in the square picks the colour under it");
  check(approx(sel.saturation(), 0.75f, 0.005f) && approx(sel.brightness(), 0.6f, 0.005f), "saturation and brightness follow the click");
  check(approx(sel.hue(), 2.0f / 3.0f, 1e-4f), "the hue is not touched by the square");
  check(changes == 1 && last == sel.currentColour(), "onChange fired once with the new colour");

  // A drag carries on outside the square and clamps to its edges.
  drag(router, sel.localToRoot(sel.colourSpacePoint(0.5f, 0.5f)),
       sel.localToRoot(sel.colourSpacePoint(1.3f, -0.4f)));
  check(approx(sel.saturation(), 1.0f) && approx(sel.brightness(), 0.0f), "a drag past the corner clamps to s=1, v=0");

  // The square on screen IS the conversion: a pixel read back matches.
  sel.setHSV(0.1f, 0.75f, 0.6f, false);
  {
    Bitmap bmp = render(root);
    const Point probe = sel.localToRoot(sel.colourSpacePoint(0.25f, 0.9f));
    const Colour want = detail::selectorHsvToColour(0.1f, 0.25f, 0.9f);
    check(sameColour(pixel(bmp, probe), want, 10), "the drawn square at s=.25 v=.9 is that colour");
    const Point probe2 = sel.localToRoot(sel.colourSpacePoint(0.9f, 0.2f));
    check(sameColour(pixel(bmp, probe2), detail::selectorHsvToColour(0.1f, 0.9f, 0.2f), 10),
          "and at s=.9 v=.2");
    const Rect strip = sel.hueStripBounds();
    const Point mid = sel.localToRoot({strip.centre().x, strip.y + strip.h * (1.0f / 3.0f)});
    check(sameColour(pixel(bmp, mid), Colour(0, 255, 0), 12), "a third of the way down the hue strip is green");
    check(sameColour(pixel(bmp, sel.localToRoot(sel.previewBounds().centre())), sel.currentColour(), 2),
          "the preview shows the current colour");
  }

  // The hue strip.
  {
    const Rect strip = sel.hueStripBounds();
    click(router, sel.localToRoot({strip.centre().x, strip.y + strip.h * 0.5f}));
    check(approx(sel.hue(), 0.5f, 0.01f), "a click halfway down the strip is 180 degrees");
    check(sameColour(sel.currentColour(), detail::selectorHsvToColour(sel.hue(), 0.75f, 0.6f), 1),
          "and keeps the saturation and brightness");
    // The keyboard: the strip now has focus.
    const float before = sel.hue();
    press(router, KeyPress::Down);
    check(approx(sel.hue(), before + 1.0f / 360.0f, 1e-4f), "Down on the strip adds a degree");
    press(router, KeyPress::Up, false, true);
    check(approx(sel.hue(), before - 9.0f / 360.0f, 1e-4f), "Shift+Up takes ten");
  }

  // Typing a hex code.
  router.setFocus(&sel.hexEditor());
  type(router, 'a', true); // select all
  typeText(router, "#3366CC");
  check(sel.currentColour() != Colour(0x33, 0x66, 0xCC), "nothing applies before Return");
  press(router, KeyPress::Return);
  check(sel.currentColour() == Colour(0x33, 0x66, 0xCC, 0xFF), "typed #3366CC and Return sets that colour");
  check(sel.hexString() == "FF3366CC" && sel.hexEditor().getText() == "FF3366CC", "the field shows it back as AARRGGBB");
  type(router, 'a', true);
  typeText(router, "12");
  press(router, KeyPress::Return);
  check(sel.currentColour() == Colour(0x33, 0x66, 0xCC, 0xFF) && sel.hexEditor().getText() == "FF3366CC",
        "two digits are refused and the field goes back");
  type(router, 'a', true);
  typeText(router, "80ff0000");
  press(router, KeyPress::Return);
  check(sel.currentColour() == Colour(255, 0, 0, 0x80), "eight typed digits set the alpha too");

  // Black keeps the hue it was reached from.
  sel.setHSV(0.6f, 1.0f, 1.0f, false);
  sel.setCurrentColour(Colour(0, 0, 0));
  check(approx(sel.hue(), 0.6f) && approx(sel.saturation(), 1.0f), "setting black keeps hue and saturation");

  // A channel slider.
  changes = 0;
  sel.channelSlider(0).setValue(1.0f);
  check(sel.currentColour().r == 255 && changes == 1, "the red slider sets red, once");
  sel.channelSlider(3).setValue(0.5f);
  check(sel.currentColour().a == 128, "the alpha slider sets alpha");
  sel.setCurrentColour(Colour(10, 20, 30), false);
  check(approx(sel.channelSlider(2).value(), 30.0f / 255.0f, 1e-4f), "a colour set from outside moves the sliders");

  // Swatches: click to use, right-click to store.
  sel.setSwatches({Colour(255, 0, 0), Colour(0, 255, 0), Colour(0, 0, 255, 128)});
  int swatchChanged = -1;
  sel.onSwatchChanged = [&](int index, Colour) { swatchChanged = index; };
  click(router, sel.localToRoot(sel.swatchBounds(1).centre()));
  check(sel.currentColour() == Colour(0, 255, 0), "clicking a swatch uses it");
  sel.setCurrentColour(Colour(1, 2, 3));
  router.mouseDown(sel.localToRoot(sel.swatchBounds(0).centre()), 1, false, false, false, true);
  check(sel.swatchMenu().isOpen(), "right-clicking a swatch offers a menu");
  click(router, sel.swatchMenu().itemBounds(1).centre());
  check(!sel.swatchMenu().isOpen() && sel.swatchColour(0) == Colour(1, 2, 3) && swatchChanged == 0,
        "\"store the current colour\" overwrites the swatch");
  {
    Bitmap bmp = render(root);
    check(sameColour(pixel(bmp, sel.localToRoot(sel.swatchBounds(0).centre())), Colour(1, 2, 3), 2),
          "the stored swatch is drawn");
  }

  // The keyboard on the square.
  sel.setHSV(0.3f, 0.5f, 0.5f, false);
  click(router, sel.localToRoot(sel.colourSpacePoint(0.5f, 0.5f)));
  press(router, KeyPress::Right);
  press(router, KeyPress::Up, false, true);
  check(approx(sel.saturation(), 0.51f, 0.003f) && approx(sel.brightness(), 0.6f, 0.003f),
        "Right and Shift+Up move saturation by 1% and brightness by 10%");

  AccessibleInfo info = sel.accessibleInfo();
  check(info.role == AccessibleRole::Group && info.value == sel.hexString() && info.name == "Colour",
        "a reader hears the selector as a group reading its hex code");

  // No alpha channel: alpha is forced opaque and the slider is gone.
  ColourSelector opaque(ColourSelector::showColourspace | ColourSelector::showSliders);
  root.addChild(&opaque);
  opaque.setBounds({320.0f, 10.0f, 170.0f, 300.0f});
  opaque.setCurrentColour(Colour(9, 9, 9, 64));
  check(opaque.currentColour().a == 255 && !opaque.channelSlider(3).isVisible() && opaque.hexString() == "090909",
        "without the alpha channel colours are opaque and the code is six digits");
  check(inkedPixels(render(root), Colour(40, 40, 40)) > 2000, "both selectors render");
}

// ═══════════════════════════════════════════════════════════════════════════
// Toolbar
// ═══════════════════════════════════════════════════════════════════════════

struct TestFactory : ToolbarItemFactory {
  int clicks[8] = {};
  void getAllToolbarItemIds(std::vector<int>& ids) override {
    ids = {1, 2, 3, 4, 5, 6, 7, separatorBarId, spacerId, flexibleSpacerId};
  }
  void getDefaultItemSet(std::vector<int>& ids) override {
    ids = {1, 2, 3, separatorBarId, 4, 5, flexibleSpacerId, 6};
  }
  std::unique_ptr<ToolbarItemComponent> createItem(int id) override {
    static const char* names[] = {"", "New", "Open", "Save", "Undo", "Redo", "Help", "Mixer"};
    if (id < 1 || id > 7) return nullptr;
    std::unique_ptr<ToolbarButton> b(new ToolbarButton(id, names[id], kIcon));
    b->onClick = [this, id]() { ++clicks[id]; };
    return std::unique_ptr<ToolbarItemComponent>(b.release());
  }
};

void testToolbar() {
  std::printf("Toolbar\n");
  Widget root;
  root.setFont(g_font);
  root.setBounds({0.0f, 0.0f, 800.0f, 600.0f});
  Toolbar tb;
  root.addChild(&tb);
  tb.setStyle(Toolbar::iconsOnly);
  tb.setBounds({0.0f, 0.0f, 800.0f, 40.0f});
  MouseRouter router(root);
  TestFactory factory;
  int changed = 0;
  tb.onItemsChanged = [&]() { ++changed; };

  tb.addDefaultItems(factory);
  check(tb.numItems() == 8 && tb.toString() == "TB:1 2 3 -1 4 5 -3 6", "the default set, saved as the reference writes it");
  check(changed == 8, "every addition is reported");
  check(approx(tb.itemComponent(0)->bounds().w, 40.0f) && approx(tb.itemComponent(3)->bounds().w, 8.0f),
        "icon buttons are as long as the toolbar is thick; a separator is a fifth");
  check(approx(tb.itemComponent(7)->bounds().right(), 800.0f), "the flexible spacer pushes the last item to the end");
  check(approx(tb.itemComponent(6)->bounds().w, 800.0f - 248.0f), "and takes exactly the room left over");
  check(!tb.hasOverflow(), "everything fits at 800");

  click(router, centreOf(*tb.itemComponent(2)));
  check(factory.clicks[3] == 1, "clicking Save fires it");

  // Overflow: 150 wide leaves room for three buttons and the separator.
  tb.setBounds({0.0f, 0.0f, 150.0f, 40.0f});
  check(tb.hasOverflow() && tb.firstOverflowIndex() == 4 && tb.numOverflowItems() == 4,
        "at 150 the last four items go behind the chevron");
  check(!tb.itemComponent(4)->isVisible() && tb.itemComponent(3)->isVisible(), "hidden, not squashed");
  check(approx(tb.overflowButtonBounds().x, 128.0f), "the chevron sits at the end");
  click(router, tb.localToRoot(tb.overflowButtonBounds().centre()));
  PopupMenuOverlay& more = tb.overflowMenu();
  check(more.isOpen() && more.menu().numItems() == 3, "the chevron opens a menu of Undo, Redo and Help");
  check(more.menu().itemAt(1).text == "Redo", "in toolbar order");
  click(router, more.itemBounds(1).centre());
  check(!more.isOpen() && factory.clicks[5] == 1, "choosing Redo from the menu clicks it");
  router.setFocus(nullptr);
  // From the keyboard: Return on the chevron opens it with the first lit.
  {
    Component* chevron = nullptr;
    for (Component* c : tb.children())
      if (c->accessibleName() == "More items") chevron = c;
    router.setFocus(chevron);
    press(router, KeyPress::Return);
    check(more.isOpen() && more.highlighted() == 0, "Return on the chevron opens it, first item lit");
    press(router, KeyPress::Return);
    check(factory.clicks[4] == 1 && router.focused() == chevron, "Return chooses Undo and the chevron has the keys back");
  }
  {
    Bitmap bmp = render(root);
    check(inkedPixels(bmp, Colour(40, 40, 40)) > 200, "an overflowing toolbar renders");
  }

  // ── Customising, by dragging ──
  tb.setBounds({0.0f, 0.0f, 800.0f, 40.0f});
  changed = 0;
  tb.showCustomisationDialog(factory);
  check(tb.isCustomisationDialogShowing() && tb.isEditingActive(), "the customisation dialog is up and items are editable");
  ToolbarItemPalette* palette = tb.customisationPalette();
  check(palette && palette->numItems() == 10, "the palette shows all ten items the factory makes");

  click(router, centreOf(*tb.itemComponent(2)));
  check(factory.clicks[3] == 1, "while customising, a press on Save does not save");

  // Mixer, from the palette, onto the gap between New and Open.
  ToolbarItemComponent* mixer = palette ? palette->itemComponent(6) : nullptr;
  check(mixer && mixer->itemId() == 7, "the palette's seventh item is Mixer");
  if (mixer) drag(router, centreOf(*mixer), {45.0f, 20.0f}, 10);
  check(tb.toString() == "TB:1 7 2 3 -1 4 5 -3 6", "dropping Mixer between New and Open inserts it there");
  check(tb.itemComponent(1)->editingMode() == ToolbarItemComponent::EditingMode::editableOnToolbar,
        "an item added while customising is editable straight away");

  // Open, off the toolbar and back onto the palette: removed.
  drag(router, centreOf(*tb.itemComponent(2)), centreOf(*palette), 10);
  check(tb.toString() == "TB:1 7 3 -1 4 5 -3 6", "dragging Open back onto the palette removes it");

  // New, along the toolbar to the far end: moved.
  drag(router, centreOf(*tb.itemComponent(0)), {798.0f, 20.0f}, 12);
  check(tb.toString() == "TB:7 3 -1 4 5 -3 6 1", "dragging New past the end moves it there");

  // Help from the palette is already on the toolbar: it moves, never twice.
  drag(router, centreOf(*palette->itemComponent(5)), {5.0f, 20.0f}, 10);
  check(tb.toString() == "TB:6 7 3 -1 4 5 -3 1", "Help dragged in again moves to the front, not doubled");
  // A separator can be added as often as wanted.
  drag(router, centreOf(*palette->itemComponent(7)), {5.0f, 20.0f}, 10);
  check(tb.toString() == "TB:-1 6 7 3 -1 4 5 -3 1", "a second separator is added");
  check(changed >= 5, "every rearrangement is reported");
  {
    Bitmap bmp = render(root);
    check(inkedPixels(bmp, Colour(40, 40, 40)) > 2000, "the dialog and palette render");
  }

  // Done.
  const std::string saved = tb.toString();
  press(router, KeyPress::Escape);
  check(!tb.isCustomisationDialogShowing() && !tb.isEditingActive(), "Escape closes the dialog and ends editing");
  click(router, centreOf(*tb.itemComponent(3)));
  check(factory.clicks[3] == 2, "after customising, Save saves again");

  // The round trip through a string.
  Toolbar other;
  root.addChild(&other);
  other.setBounds({0.0f, 100.0f, 800.0f, 40.0f});
  check(other.restoreFromString(factory, saved) && other.toString() == saved, "restoreFromString rebuilds the same toolbar");
  check(!other.restoreFromString(factory, "XX:1 2") && other.toString() == saved, "text that is not a toolbar is refused, untouched");
  check(!other.restoreFromString(factory, "TB:1 x"), "a token that is not a number is refused");
  check(other.restoreFromString(factory, "TB:1 99 2") && other.toString() == "TB:1 2", "an id the factory no longer makes is skipped");

  // Reset to defaults, through the dialog.
  tb.showCustomisationDialog(factory);
  Button* reset = nullptr;
  std::function<void(Component*)> find = [&](Component* c) {
    for (Component* child : c->children()) {
      if (child->accessibleName() == "" && child->accessibleInfo().role == AccessibleRole::Button &&
          child->accessibleInfo().name == "Reset to defaults")
        reset = static_cast<Button*>(child);
      find(child);
    }
  };
  find(&root);
  check(reset != nullptr, "the dialog has a Reset button");
  if (reset) click(router, centreOf(*reset));
  check(tb.toString() == "TB:1 2 3 -1 4 5 -3 6", "Reset puts the default set back");
  tb.closeCustomisationDialog();

  // Vertical.
  tb.setVertical(true);
  tb.setBounds({0.0f, 0.0f, 40.0f, 500.0f});
  check(approx(tb.itemComponent(1)->bounds().y, 40.0f) && approx(tb.itemComponent(1)->bounds().w, 40.0f) &&
            approx(tb.itemComponent(7)->bounds().bottom(), 500.0f),
        "a vertical toolbar lays out downwards");

  AccessibleInfo info = tb.itemComponent(0)->accessibleInfo();
  check(info.role == AccessibleRole::Button && info.name == "New", "a toolbar button is announced by its label");
  check(tb.accessibleInfo().role == AccessibleRole::Group && tb.accessibleInfo().name == "Toolbar", "the toolbar is a group");

  // Only the negative ids are separators and spacers; 0 is a regular item,
  // which the factory is asked to make. Dragged in again from the palette it
  // moves, as Help does -- the rule tested id > 0, and added it twice.
  {
    struct ZeroFactory : ToolbarItemFactory {
      void getAllToolbarItemIds(std::vector<int>& ids) override { ids = {0, 1}; }
      void getDefaultItemSet(std::vector<int>& ids) override { ids = {0, 1}; }
      std::unique_ptr<ToolbarItemComponent> createItem(int id) override {
        if (id < 0 || id > 1) return nullptr;
        return std::unique_ptr<ToolbarItemComponent>(new ToolbarButton(id, id == 0 ? "Zero" : "One", kIcon));
      }
    } zeroFactory;
    root.removeChild(&tb);
    Toolbar zero;
    root.addChild(&zero);
    zero.setStyle(Toolbar::iconsOnly);
    zero.setBounds({0.0f, 0.0f, 800.0f, 40.0f});
    zero.addDefaultItems(zeroFactory);
    zero.showCustomisationDialog(zeroFactory);
    ToolbarItemPalette* zp = zero.customisationPalette();
    if (zp && zp->numItems() == 2) drag(router, centreOf(*zp->itemComponent(0)), {798.0f, 20.0f}, 12);
    const std::string got = zero.toString();
    check(got == "TB:1 0", ("item 0 dragged in again from the palette moves to the end, not doubled (" + got + ")").c_str());
    zero.closeCustomisationDialog();
    root.removeChild(&zero);
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// Menu bar
// ═══════════════════════════════════════════════════════════════════════════

struct TestMenus : MenuBarModel {
  std::vector<std::string> names = {"&File", "Edit", "View"};
  std::vector<std::pair<int, int>> chosen;
  std::vector<bool> activations;
  std::vector<std::string> getMenuBarNames() override { return names; }
  PopupMenu getMenuForIndex(int index, const std::string&) override {
    PopupMenu m;
    if (index == 0) {
      m.addItem(1, "New");
      m.addItem(2, "Open");
      m.addSeparator();
      m.addItem(3, "Quit");
    } else if (index == 1) {
      m.addItem(11, "Undo");
      m.addItem(12, "Redo", false);
      m.addItem(13, "Cut");
    } else {
      m.addItem(21, "Zoom", true, true);
    }
    return m;
  }
  void menuItemSelected(int id, int top) override { chosen.push_back({id, top}); }
  void menuBarActivated(bool active) override { activations.push_back(active); }
};

void testMenuBar() {
  std::printf("MenuBarComponent / BurgerMenuComponent\n");
  Widget root;
  root.setFont(g_font);
  root.setBounds({0.0f, 0.0f, 600.0f, 400.0f});
  TestMenus model;
  MenuBarComponent bar(&model);
  TextEditor field;
  root.addChild(&bar);
  root.addChild(&field);
  bar.setBounds({0.0f, 0.0f, 600.0f, MenuBarComponent::kDefaultHeight});
  field.setBounds({10.0f, 200.0f, 200.0f, 24.0f});
  MouseRouter router(root);
  router.onUnhandledKey = [&](const KeyPress& k) { return bar.handleShortcutKey(k); };
  PopupMenuOverlay& menu = bar.menuOverlay();

  check(bar.numMenus() == 3, "three menus from the model");
  check(bar.itemBounds(1).x >= bar.itemBounds(0).right() - 0.01f, "names run left to right");

  // Mouse: click File, slide to Edit, choose Cut.
  click(router, centreOf(bar, bar.itemBounds(0)));
  check(bar.openMenuIndex() == 0 && menu.isOpen(), "clicking File opens it");
  check(menu.menuBounds().y >= MenuBarComponent::kDefaultHeight - 0.01f && approx(menu.menuBounds().x, bar.itemBounds(0).x),
        "the menu drops below its name");
  check(model.activations.size() == 1 && model.activations[0], "the model hears the bar go active");
  router.mouseMove(centreOf(bar, bar.itemBounds(1)));
  check(bar.openMenuIndex() == 1 && menu.menu().itemAt(0).text == "Undo", "moving along the bar switches to Edit");
  click(router, menu.itemBounds(2).centre());
  check(model.chosen.size() == 1 && model.chosen[0] == std::make_pair(13, 1), "clicking Cut chooses (13, Edit)");
  check(!menu.isOpen() && bar.openMenuIndex() == -1, "and the menu is gone");
  check(model.activations.size() == 2 && !model.activations[1], "and the bar went idle again");
  click(router, centreOf(bar, bar.itemBounds(1)));
  click(router, menu.itemBounds(1).centre());
  check(menu.isOpen() && model.chosen.size() == 1, "a disabled item cannot be chosen");
  click(router, {500.0f, 300.0f});
  check(!menu.isOpen() && model.chosen.size() == 1, "a click outside closes the menu choosing nothing");

  // Press on View, drag into its menu, let go over Zoom.
  {
    const Point viewAt = centreOf(bar, bar.itemBounds(2));
    router.mouseMove(viewAt);
    router.mouseDown(viewAt);
    check(menu.isOpen() && bar.openMenuIndex() == 2, "pressing View opens it");
    const Point zoom = menu.itemBounds(0).centre();
    router.mouseMove({viewAt.x, (viewAt.y + zoom.y) * 0.5f});
    router.mouseMove(zoom);
    check(menu.highlighted() == 0, "dragging down lights Zoom");
    router.mouseUp(zoom);
    check(!model.chosen.empty() && model.chosen.back() == std::make_pair(21, 2), "letting go over Zoom chooses it");
  }

  // The keyboard: F10, walk, open, walk inside, back out.
  router.setFocus(&field);
  model.activations.clear();
  press(router, KeyPress::F10);
  check(bar.isKeyboardActive() && bar.highlightedIndex() == 0 && router.focused() == &bar,
        "F10 enters the bar with File lit, and the bar has the keys");
  press(router, KeyPress::Right);
  check(bar.highlightedIndex() == 1 && !menu.isOpen(), "Right lights Edit without opening it");
  press(router, KeyPress::Down);
  check(menu.isOpen() && bar.openMenuIndex() == 1 && menu.highlighted() == 0, "Down opens Edit with Undo lit");
  press(router, KeyPress::Down);
  check(menu.highlighted() == 2, "Down skips the disabled Redo to Cut");
  press(router, KeyPress::Right);
  check(bar.openMenuIndex() == 2 && menu.highlighted() == 0, "Right with a menu open moves to View");
  press(router, KeyPress::Left);
  check(bar.openMenuIndex() == 1, "Left goes back to Edit");
  press(router, KeyPress::Escape);
  check(!menu.isOpen() && bar.isKeyboardActive() && bar.highlightedIndex() == 1 && router.focused() == &bar,
        "Escape closes the menu but stays in the bar");
  press(router, KeyPress::Escape);
  check(!bar.isKeyboardActive() && router.focused() == &field, "a second Escape leaves, and the field has the keys back");
  check(model.activations.size() == 2 && model.activations[0] && !model.activations[1], "one activation for the whole visit");

  // Alt+F opens File by its '&'; Return chooses New.
  const size_t before = model.chosen.size();
  type(router, 'f', false, false, true);
  check(menu.isOpen() && bar.openMenuIndex() == 0 && menu.highlighted() == 0, "Alt+F opens File with New lit");
  press(router, KeyPress::Return);
  check(model.chosen.size() == before + 1 && model.chosen.back() == std::make_pair(1, 0), "Return chooses New");
  check(!bar.isKeyboardActive() && router.focused() == &field, "choosing ends menu mode and gives the field its keys back");

  // Alt+E then a letter: type-ahead inside the menu.
  type(router, 'e', false, false, true);
  type(router, 'c');
  check(menu.highlighted() == 2, "typing c inside Edit lights Cut");
  press(router, KeyPress::Return);
  check(model.chosen.back() == std::make_pair(13, 1), "and Return chooses it");
  check(!bar.handleShortcutKey([] {
          KeyPress k;
          k.character = 'q';
          k.altDown = true;
          return k;
        }()),
        "Alt with a letter no menu has is declined");

  {
    click(router, centreOf(bar, bar.itemBounds(0)));
    Bitmap bmp = render(root);
    check(inkedPixels(bmp, Colour(40, 40, 40)) > 300, "the bar and an open menu render");
    AccessibleInfo info = bar.accessibleInfo();
    check(info.role == AccessibleRole::Group && info.value == "File", "a reader hears the open menu's name, without the &");
    press(router, KeyPress::Escape);
  }

  // The model changes its names.
  model.names = {"File", "Edit", "View", "Help"};
  model.menuItemsChanged();
  check(bar.numMenus() == 4, "menuItemsChanged re-reads the names");

  // ── The burger ──
  Widget side;
  side.setFont(g_font);
  side.setBounds({0.0f, 0.0f, 220.0f, 600.0f});
  BurgerMenuComponent burger(&model);
  side.addChild(&burger);
  burger.setBounds({0.0f, 0.0f, 220.0f, 600.0f});
  MouseRouter sideRouter(side);
  int invoked = 0;
  burger.onItemInvoked = [&](int, int) { ++invoked; };
  // File: New Open - Quit | Edit: Undo Redo Cut | View: Zoom | Help: Zoom
  check(burger.numRows() == 13 && burger.isHeading(0) && burger.rowText(5) == "Edit" && burger.isHeading(5),
        "every menu as a heading with its items under it");
  click(sideRouter, centreOf(burger, burger.rowBounds(6)));
  check(model.chosen.back() == std::make_pair(11, 1) && invoked == 1, "clicking Undo chooses (11, Edit)");
  sideRouter.setFocus(&burger);
  press(sideRouter, KeyPress::Home);
  check(burger.selectedRow() == 1, "Home lands on the first item, not a heading");
  press(sideRouter, KeyPress::Down);
  press(sideRouter, KeyPress::Down);
  check(burger.selectedRow() == 4, "Down steps over the separator");
  press(sideRouter, KeyPress::Down);
  check(burger.selectedRow() == 6, "and over the next heading");
  press(sideRouter, KeyPress::Down);
  check(burger.selectedRow() == 8, "and over the disabled Redo");
  press(sideRouter, KeyPress::Return);
  check(model.chosen.back() == std::make_pair(13, 1), "Return chooses Cut");
  check(burger.accessibleInfo().role == AccessibleRole::List && burger.accessibleInfo().value == "Cut",
        "a reader hears the burger as a list reading the selected item");
  check(inkedPixels(render(side), Colour(40, 40, 40)) > 500, "the burger renders");

  // The model goes away first: both components let go of it.
  {
    std::unique_ptr<TestMenus> doomed(new TestMenus());
    MenuBarComponent bar2(doomed.get());
    BurgerMenuComponent burger2(doomed.get());
    root.addChild(&bar2);
    bar2.setBounds({0.0f, 40.0f, 600.0f, 24.0f});
    bar2.showMenu(0);
    doomed.reset();
    check(bar2.numMenus() == 0 && burger2.numRows() == 0 && bar2.model() == nullptr, "a destroyed model is forgotten");
    check(!bar2.menuOverlay().isOpen(), "and its open menu closed");
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// SidePanel
// ═══════════════════════════════════════════════════════════════════════════

void testSidePanel() {
  std::printf("SidePanel\n");
  Widget root;
  root.setFont(g_font);
  root.setBounds({0.0f, 0.0f, 600.0f, 400.0f});
  TextEditor field;
  root.addChild(&field);
  field.setBounds({300.0f, 50.0f, 200.0f, 24.0f});
  Button inner("Inner");
  SidePanel side("Settings", 200.0f, true, &inner);
  root.addChild(&side);
  MouseRouter router(root);
  int shown = 0, hidden = 0;
  side.onPanelShowHide = [&](bool s) { (s ? shown : hidden)++; };

  check(!side.isVisible() && !side.isPanelShowing(), "a drawer starts away");
  router.setFocus(&field);
  side.showOrHide(true, 10.0);
  check(side.isVisible() && side.isPanelShowing() && shown == 1, "showOrHide(true) brings it");
  check(approx(side.panelBounds().x, -200.0f) && approx(side.openFraction(), 0.0f), "starting off the left edge");
  check(router.focused() == &inner, "the keyboard moves into the drawer's content");
  side.tick(10.1);
  check(approx(side.panelBounds().x, -50.0f, 0.05f) && approx(side.openFraction(), 0.75f, 0.01f),
        "halfway through, ease-out has it three quarters in");
  {
    Bitmap bmp = render(root, Colour(200, 200, 200));
    const Colour dim = pixel(bmp, {450.0f, 300.0f});
    check(dim.r < 200 && dim.r > 110, "the editor behind is dimmed in proportion");
  }
  side.tick(10.25);
  check(approx(side.panelBounds().x, 0.0f) && approx(side.openFraction(), 1.0f) && !side.isAnimating(),
        "after 0.2 s it is fully in, and still");
  {
    Bitmap bmp = render(root, Colour(200, 200, 200));
    const Colour dim = pixel(bmp, {450.0f, 300.0f});
    check(std::abs((int) dim.r - (200 * 155 / 255)) <= 3, "fully open, the editor is dimmed by 100/255");
    check(sameColour(pixel(bmp, {100.0f, 15.0f}), palette::panel(), 6), "the title bar is drawn");
  }
  check(!side.tick(10.3), "a still drawer reports no movement");

  // A press on the dimmed editor closes it, and does not reach the field.
  click(router, {450.0f, 60.0f});
  check(!side.isPanelShowing() && hidden == 1 && side.isVisible(), "a click outside starts it closing");
  check(router.focused() == &field, "the keyboard goes back to the field it came from");
  side.tick(10.4);
  check(side.isVisible() && side.openFraction() > 0.0f && side.openFraction() < 1.0f, "mid-slide it is still there");
  side.tick(10.55);
  check(!side.isVisible(), "and once out of sight it is gone");
  click(router, {450.0f, 60.0f});
  check(router.focused() == &field, "a closed drawer takes no clicks");

  // Escape, from inside.
  side.showOrHide(true, 20.0);
  side.tick(20.3);
  check(router.focused() == &inner, "opened again, the content has the keys");
  press(router, KeyPress::Escape);
  check(!side.isPanelShowing(), "Escape closes it");
  side.tick(20.6);

  // The close button.
  side.setInputClock(30.0);
  side.showOrHide(true, 30.0);
  side.tick(30.3);
  click(router, side.localToRoot(side.closeButtonBounds().centre()));
  check(!side.isPanelShowing(), "the close button closes it");
  side.tick(30.6);

  // Dragging the title bar back towards the edge throws it away; a little
  // springs back.
  side.showOrHide(true, 40.0);
  side.tick(40.3);
  drag(router, {100.0f, 12.0f}, {70.0f, 12.0f});
  check(side.isPanelShowing() && approx(side.panelBounds().x, -30.0f), "a short drag pulls it out by that much");
  side.tick(40.5);
  check(approx(side.panelBounds().x, 0.0f), "and it springs back");
  drag(router, {100.0f, 12.0f}, {20.0f, 12.0f});
  check(!side.isPanelShowing(), "a drag past a third of its width closes it");
  side.tick(41.0);

  // From the right.
  side.setPositionOnLeft(false);
  side.showOrHide(true, 50.0);
  check(approx(side.panelBounds().x, 600.0f), "a right-hand drawer starts off the right edge");
  side.tick(50.3);
  check(approx(side.panelBounds().x, 400.0f), "and ends against it");
  check(side.accessibleInfo().name == "Settings" && side.accessibleInfo().value == "open", "a reader hears its title and state");
  side.showOrHide(false, 51.0);
  side.tick(52.0);
  check(!side.isVisible() && shown == 5 && hidden == 5, "every open and close was reported");

  // The control that had the keyboard, deleted while the drawer was open.
  // Closing asked the dead control for its parent before looking for it in
  // the tree (AddressSanitizer: heap-use-after-free in stillInTree).
  side.setPositionOnLeft(true);
  auto* doomed = new TextEditor();
  root.addChild(doomed);
  doomed->setBounds({300.0f, 100.0f, 200.0f, 24.0f});
  router.setFocus(doomed);
  side.showOrHide(true, 60.0);
  side.tick(60.3);
  check(router.focused() == &inner, "opened over a control that has the keys");
  delete doomed;
  side.showOrHide(false, 61.0);
  side.tick(62.0);
  check(router.focused() == nullptr && !side.isVisible(),
        "closed after that control was deleted, the keys go to nobody");
}

// ═══════════════════════════════════════════════════════════════════════════
// MultiDocumentPanel
// ═══════════════════════════════════════════════════════════════════════════

struct Document : Widget {
  Button button{"Press"};
  int presses = 0;
  Document() {
    addChild(&button);
    button.onClick = [this]() { ++presses; };
  }
  void resized() override { button.setBounds({20.0f, 20.0f, 100.0f, 30.0f}); }
};

void testMultiDocument() {
  std::printf("MultiDocumentPanel\n");
  Widget root;
  root.setFont(g_font);
  root.setBounds({0.0f, 0.0f, 800.0f, 600.0f});
  MultiDocumentPanel panel;
  root.addChild(&panel);
  panel.setBounds({0.0f, 0.0f, 800.0f, 600.0f});
  MouseRouter router(root);
  int activeChanges = 0;
  panel.onActiveDocumentChanged = [&](Component*) { ++activeChanges; };

  std::unique_ptr<Document> ownedA(new Document()), ownedB(new Document());
  Document* a = ownedA.get();
  Document* b = ownedB.get();
  Document c;
  check(panel.addDocument(std::unique_ptr<Component>(ownedA.release()), "Alpha"), "a document the panel owns");
  check(panel.addDocument(std::unique_ptr<Component>(ownedB.release()), "Beta"), "another");
  check(panel.addDocument(&c, "Gamma", Colour(30, 60, 90)), "and one the caller owns");
  check(!panel.addDocument(&c, "Gamma again"), "the same document twice is refused");
  check(panel.numDocuments() == 3 && panel.activeDocument() == &c && activeChanges == 3, "the newest is active");
  check(panel.isShowingTabs() && c.isVisible() && !a->isVisible() && approx(c.bounds().y, MultiDocumentPanel::kTabHeight),
        "as tabs, only the active document shows, under the strip");

  click(router, panel.localToRoot(panel.tabBounds(0).centre()));
  check(panel.activeDocument() == a && a->isVisible() && !c.isVisible(), "clicking the first tab brings Alpha forward");

  // Closing asks first.
  int asked = 0;
  panel.tryToCloseDocument = [&](Component* d, std::function<void(bool)> answer) {
    ++asked;
    answer(d != b);
  };
  click(router, panel.localToRoot(panel.tabCloseBounds(1).centre()));
  check(asked == 1 && panel.numDocuments() == 3, "Beta says no, and stays");
  click(router, panel.localToRoot(panel.tabCloseBounds(0).centre()));
  check(asked == 2 && panel.numDocuments() == 2 && panel.indexOf(a) < 0, "Alpha says yes, and goes");
  check(panel.activeDocument() == &c, "the one used before it comes forward");
  {
    Bitmap bmp = render(root);
    check(inkedPixels(bmp, Colour(40, 40, 40)) > 300, "tabs render (and the closed one is freed at paint)");
  }

  // Ctrl+Tab from inside a document.
  router.setFocus(&c.button);
  press(router, KeyPress::Tab, true);
  check(panel.activeDocument() == b && router.focused() == &b->button, "Ctrl+Tab moves to Beta and takes the keys there");
  press(router, KeyPress::Tab, true, true);
  check(panel.activeDocument() == &c, "Ctrl+Shift+Tab goes back");

  // ── Floating windows ──
  panel.setLayoutMode(MultiDocumentPanel::LayoutMode::FloatingWindows);
  MultiDocumentPanelWindow* wb = panel.windowFor(b);
  MultiDocumentPanelWindow* wc = panel.windowFor(&c);
  check(wb && wc && b->parent() == wb && c.parent() == wc, "each document now sits in a window");
  check(!panel.windowOrder().empty() && panel.windowOrder().back() == &c, "the active one is on top");
  panel.setActiveDocument(b);
  check(panel.windowOrder().back() == b, "activating Beta raises its window");

  // Gamma's window is under Beta's. Its uncovered corner takes the first click
  // to come forward -- and that click presses nothing.
  const Rect gb = wc->bounds();
  const Point corner = panel.localToRoot({gb.right() - 20.0f, gb.bottom() - 20.0f});
  click(router, corner);
  check(panel.activeDocument() == &c && panel.windowOrder().back() == &c, "a click on an inactive window raises it");
  const Point gammaButton = centreOf(c.button);
  click(router, gammaButton);
  check(c.presses == 1, "once in front, its button takes clicks");

  // Moving and resizing.
  const Rect before = wc->bounds();
  drag(router, panel.localToRoot({before.x + 60.0f, before.y + 10.0f}), panel.localToRoot({before.x + 110.0f, before.y + 50.0f}));
  check(approx(wc->bounds().x, before.x + 50.0f) && approx(wc->bounds().y, before.y + 40.0f), "the title bar drags the window");
  const Rect moved = wc->bounds();
  drag(router, wc->localToRoot(wc->resizeGripBounds().centre()),
       {wc->localToRoot(wc->resizeGripBounds().centre()).x + 40.0f, wc->localToRoot(wc->resizeGripBounds().centre()).y + 30.0f});
  check(approx(wc->bounds().w, moved.w + 40.0f) && approx(wc->bounds().h, moved.h + 30.0f), "the corner resizes it");
  drag(router, panel.localToRoot({moved.x + 60.0f, moved.y + 10.0f}), {-900.0f, -900.0f});
  check(wc->bounds().right() >= 40.0f - 0.01f && wc->bounds().y >= 0.0f, "a window cannot be dragged out of reach");
  {
    Bitmap bmp = render(root);
    check(inkedPixels(bmp, Colour(40, 40, 40)) > 2000, "floating windows render");
  }

  // An answer that comes later.
  std::function<void(bool)> pending;
  panel.tryToCloseDocument = [&](Component*, std::function<void(bool)> answer) { pending = answer; };
  click(router, wc->localToRoot(wc->closeButtonBounds().centre()));
  check(pending && panel.numDocuments() == 2, "the close button asks, and waits");
  pending(true);
  check(panel.numDocuments() == 1 && c.parent() == nullptr, "the later yes closes it, handing Gamma back untouched");

  // Limits and fullscreen.
  panel.setMaximumNumDocuments(2);
  Document d, e;
  check(panel.addDocument(&d, "Delta") && !panel.addDocument(&e, "Epsilon"), "the maximum is enforced on adding");
  panel.closeDocument(&d, false);
  panel.useFullscreenWhenOneDocument(true);
  check(b->parent() == &panel && approx(b->bounds().w, 800.0f) && approx(b->bounds().h, 600.0f),
        "one document, fullscreen: no frame, the whole panel");

  // Close all, with one refusing.
  panel.useFullscreenWhenOneDocument(false);
  panel.addDocument(&d, "Delta");
  panel.tryToCloseDocument = [&](Component* doc, std::function<void(bool)> answer) { answer(doc != b); };
  bool allClosed = true;
  panel.closeAllDocuments(true, [&](bool all) { allClosed = all; });
  check(!allClosed && panel.numDocuments() == 1 && panel.document(0) == b, "close-all stops at the document that says no");
  check(panel.accessibleInfo().value == "Beta", "a reader hears the active document's name");
  panel.tryToCloseDocument = nullptr;
  panel.closeAllDocuments(true);
  check(panel.numDocuments() == 0 && panel.activeDocument() == nullptr, "and with nobody objecting, all go");
  render(root);
}

// ═══════════════════════════════════════════════════════════════════════════
// KeyMappingEditorComponent
// ═══════════════════════════════════════════════════════════════════════════

void testKeyMappingEditor() {
  std::printf("KeyMappingEditorComponent\n");
  check(Shortcut::fromKeyCode(KeyPress::F10).describe() == "F10" &&
            Shortcut::fromKeyCode(KeyPress::F1, true).describe() == "Ctrl+F1",
        "function keys have names");

  CommandManager cm;
  cm.registerCommand({1, "Save", "File", ""}, {Shortcut::fromCharacter('s')});
  cm.registerCommand({2, "Open", "File", ""}, {Shortcut::fromCharacter('o')});
  cm.registerCommand({3, "Undo", "Edit", ""}, {Shortcut::fromCharacter('z')});
  cm.registerCommand({4, "Redo", "Edit", ""}, {Shortcut::fromCharacter('z', true, true)});
  cm.registerCommand({5, "Play", "Transport", ""}, {Shortcut::fromCharacter(' ', false)});
  cm.registerCommand({6, "Find", "Edit", ""}, {Shortcut::fromKeyCode(KeyPress::F3)});
  int invoked = 0;
  cm.onInvoke = [&](int id) { invoked = id; };

  Widget root;
  root.setFont(g_font);
  root.setBounds({0.0f, 0.0f, 600.0f, 500.0f});
  KeyMappingEditorComponent editor(cm);
  editor.isCommandReadOnly = [](int id) { return id == 5; };
  root.addChild(&editor);
  editor.setBounds({0.0f, 0.0f, 600.0f, 500.0f});
  MouseRouter router(root);
  int changes = 0;
  editor.onMappingsChanged = [&]() { ++changes; };

  check(editor.numRows() == 9 && editor.isHeading(0) && editor.isHeading(3) && editor.isHeading(7),
        "File, Edit and Transport as headings, in the order first seen");
  check(editor.commandAtRow(1) == 1 && editor.commandAtRow(6) == 6 && editor.commandAtRow(8) == 5,
        "commands under their own category");
  check(editor.rowText(1) == "Save: Ctrl+S" && editor.rowText(6) == "Find: F3", "a row reads as its command and keys");

  // "+" on Open, then Ctrl+P.
  const int openRow = editor.rowForCommand(2);
  click(router, centreOf(editor, editor.chipBounds(openRow, KeyMappingEditorComponent::kAddChip)));
  check(editor.isCapturingKey() && editor.capturingCommand() == 2 && router.focused() == &editor,
        "clicking + waits for a key, with the keyboard");
  type(router, 'p', true);
  check(!editor.isCapturingKey() && cm.shortcutsFor(2).size() == 2 && cm.shortcutsFor(2)[1] == Shortcut::fromCharacter('p'),
        "Ctrl+P is added to Open");
  check(changes == 1, "and reported");

  // A key another command has: asked, and reassigned on Return.
  editor.setSelectedRow(openRow);
  press(router, KeyPress::Return);
  check(editor.isCapturingKey(), "Return on a row waits for a key");
  type(router, 's', true);
  check(editor.isShowingQuestion() && cm.shortcutsFor(1).size() == 1, "Ctrl+S belongs to Save: the user is asked, nothing moved yet");
  press(router, KeyPress::Return);
  check(!editor.isShowingQuestion() && cm.commandForShortcut(Shortcut::fromCharacter('s')) == 2 && cm.shortcutsFor(1).empty(),
        "Reassign moves Ctrl+S to Open");
  check(router.focused() == &editor, "and the editor has the keys back");
  KeyPress ctrlS;
  ctrlS.character = 's';
  ctrlS.ctrlDown = true;
  cm.keyPressed(ctrlS);
  check(invoked == 2, "Ctrl+S now runs Open");

  // A conflict refused.
  editor.startKeyCapture(1);
  type(router, 'z', true);
  check(editor.isShowingQuestion(), "Ctrl+Z belongs to Undo: asked");
  press(router, KeyPress::Escape);
  check(!editor.isShowingQuestion() && cm.commandForShortcut(Shortcut::fromCharacter('z')) == 3 && cm.shortcutsFor(1).empty(),
        "Cancel changes nothing");

  // A keycap: remove one, change another.
  const int openKeys = (int) cm.shortcutsFor(2).size();
  check(openKeys == 3, "Open has Ctrl+O, Ctrl+P and Ctrl+S");
  click(router, centreOf(editor, editor.chipBounds(openRow, 1)));
  check(editor.keyMenu().isOpen(), "clicking a keycap offers to change or remove it");
  click(router, editor.keyMenu().itemBounds(2).centre());
  check(cm.shortcutsFor(2).size() == 2 && cm.commandForShortcut(Shortcut::fromCharacter('p')) == 0, "Remove takes Ctrl+P off");
  click(router, centreOf(editor, editor.chipBounds(openRow, 0)));
  click(router, editor.keyMenu().itemBounds(1).centre());
  check(editor.isCapturingKey(), "Change waits for the replacement");
  press(router, KeyPress::F5);
  check(cm.commandForShortcut(Shortcut::fromCharacter('o')) == 0 && cm.commandForShortcut(Shortcut::fromKeyCode(KeyPress::F5)) == 2,
        "F5 replaces Ctrl+O");
  check(editor.rowText(openRow) == "Open: Ctrl+S, F5", "and the row says so");

  // Delete takes the last key of the selected row.
  editor.setSelectedRow(openRow);
  router.setFocus(&editor);
  press(router, KeyPress::Delete);
  check(cm.shortcutsFor(2).size() == 1, "Delete removes the row's last key");

  // Escape during capture changes nothing; Tab is a key like any other.
  editor.startKeyCapture(3);
  press(router, KeyPress::Escape);
  check(!editor.isCapturingKey() && cm.shortcutsFor(3).size() == 1, "Escape cancels a capture");
  editor.startKeyCapture(6);
  press(router, KeyPress::Tab);
  check(cm.commandForShortcut(Shortcut::fromKeyCode(KeyPress::Tab)) == 6 && router.focused() == &editor,
        "Tab is captured as a key, not used to move focus");

  // Read-only.
  const int playRow = editor.rowForCommand(5);
  check(editor.chipBounds(playRow, KeyMappingEditorComponent::kAddChip).isEmpty(), "a read-only command has no +");
  editor.startKeyCapture(5);
  check(!editor.isCapturingKey(), "and cannot be captured for");

  // Navigation steps over headings.
  editor.setSelectedRow(-1);
  press(router, KeyPress::Down);
  check(editor.selectedRow() == 1, "Down from nothing lands on Save");
  press(router, KeyPress::Down);
  press(router, KeyPress::Down);
  check(editor.selectedRow() == 4, "and steps over the Edit heading");
  press(router, KeyPress::End);
  press(router, KeyPress::Up);
  check(editor.selectedRow() == 6, "End then Up skips the Transport heading");
  check(editor.accessibleInfo().value == editor.rowText(6), "a reader hears the selected row");

  // Settings round trip, then reset to defaults through the button.
  const std::string saved = cm.saveMappings();
  click(router, centreOf(editor.resetButton()));
  check(editor.isShowingQuestion(), "Reset asks first");
  press(router, KeyPress::Return);
  check(cm.commandForShortcut(Shortcut::fromCharacter('s')) == 1 && cm.commandForShortcut(Shortcut::fromCharacter('o')) == 2 &&
            cm.commandForShortcut(Shortcut::fromKeyCode(KeyPress::Tab)) == 0,
        "Reset puts every default back");
  check(cm.loadMappings(saved) && cm.commandForShortcut(Shortcut::fromKeyCode(KeyPress::Tab)) == 6,
        "the saved mappings load back");
  {
    editor.startKeyCapture(4);
    Bitmap bmp = render(root);
    check(inkedPixels(bmp, Colour(40, 40, 40)) > 2000, "the editor renders, prompt and all");
    editor.cancelKeyCapture();
  }
}

// ═══════════════════════════════════════════════════════════════════════════
// The file chooser components
// ═══════════════════════════════════════════════════════════════════════════

void testFileComponents() {
  std::printf("File chooser components\n");
  namespace fs = std::filesystem;
  const std::string base = sonoretest::tempPath("gui_widgets_files");
  std::error_code ec;
  fs::create_directories(fs::path(base) / "Drums", ec);
  fs::create_directories(fs::path(base) / "Bass", ec);
  writeFile(joinPath(joinPath(base, "Drums"), "kick.wav"), std::vector<uint8_t>(100, 1));
  writeFile(joinPath(joinPath(base, "Drums"), "snare.wav"), std::vector<uint8_t>(3000, 2));
  writeFile(joinPath(base, "notes.txt"), std::vector<uint8_t>(10, 'x'));
  writeFile(joinPath(base, "song.WAV"), std::vector<uint8_t>(2048, 3));
  writeFile(joinPath(base, "a.png"), makePng(3, 2, 0xFF2040FFu));
  const std::string drums = joinPath(base, "Drums");

  Widget root;
  root.setFont(g_font);
  root.setBounds({0.0f, 0.0f, 900.0f, 700.0f});
  MouseRouter router(root);

  // ── FileListComponent ──
  FileListComponent list;
  root.addChild(&list);
  list.setBounds({0.0f, 0.0f, 400.0f, 300.0f});
  std::string chosenFile;
  int dirChanges = 0;
  list.onFileChosen = [&](const std::string& p) { chosenFile = p; };
  list.onDirectoryChanged = [&](const std::string&) { ++dirChanges; };
  check(list.setDirectory(base) && list.numEntries() == 5, "five entries in the folder");
  check(list.entry(0).name == "Bass" && list.entry(1).name == "Drums" && list.entry(2).name == "a.png" &&
            list.entry(3).name == "notes.txt" && list.entry(4).name == "song.WAV",
        "folders first, then names regardless of case");
  check(!list.setDirectory(joinPath(base, "nowhere")) && list.directory() == base, "a folder that is not there is refused");
  auto rowCentre = [&](int row) {
    return list.list().localToRoot({60.0f, (float) row * FileListComponent::kRowHeight + FileListComponent::kRowHeight * 0.5f});
  };
  router.mouseDown(rowCentre(1), 2);
  router.mouseUp(rowCentre(1));
  check(list.directory() == drums && list.numEntries() == 2, "double-clicking Drums walks into it");
  router.setFocus(&list.list());
  press(router, KeyPress::Backspace);
  check(list.directory() == base && list.selectedFile() == drums, "Backspace walks out, with Drums still selected");
  type(router, 'n');
  check(fileName(list.selectedFile()) == "notes.txt", "typing n jumps to notes.txt");
  type(router, 'S');
  check(fileName(list.selectedFile()) == "song.WAV", "and S to song.WAV");
  press(router, KeyPress::Return);
  check(chosenFile == joinPath(base, "song.WAV"), "Return on a file chooses it");
  list.setFileFilter({".wav"});
  check(list.numEntries() == 3 && list.entry(2).name == "song.WAV", "a .wav filter keeps folders and song.WAV");
  list.setFileFilter({});
  ImagePreviewComponent preview;
  root.addChild(&preview);
  preview.setBounds({410.0f, 0.0f, 200.0f, 200.0f});
  list.setPreview(&preview);
  list.selectFile("a.png");
  check(preview.hasImage() && preview.image().width() == 3 && preview.caption() == "3 x 2",
        "selecting a PNG shows it in the preview");
  {
    Bitmap bmp = render(root);
    check(sameColour(pixel(bmp, preview.localToRoot({100.0f, 70.0f})), Colour(0xFF, 0x20, 0x40), 6),
          "the preview draws the picture");
  }
  list.selectFile("notes.txt");
  check(!preview.hasImage() && preview.currentFile() == joinPath(base, "notes.txt"), "a text file has no picture");
  check(list.list().accessibleInfo().value == "notes.txt", "a reader hears the selected name");
  // Folders past MAX_PATH, walked into one Return at a time. Made through
  // pathio, which takes long paths; the list read the 258-character level as
  // empty, so nothing under it could be reached while the files opened fine.
  {
    const std::string deepBase = sonoretest::tempPath("gui_widgets_deep");
    bool made = pathio::makeDirectory(deepBase.c_str());
    std::string deep = deepBase;
    for (int i = 0; i < 6 && made; ++i) {
      deep = joinPath(deep, std::string(48, (char) ('a' + i)));
      made = pathio::makeDirectory(deep.c_str());
    }
    writeFile(joinPath(deep, "inside.wav"), std::vector<uint8_t>(5, 1));
    bool walked = made && list.setDirectory(deepBase);
    std::string at = deepBase;
    router.setFocus(&list.list());
    for (int i = 0; i < 6 && walked; ++i) {
      at = joinPath(at, std::string(48, (char) ('a' + i)));
      walked = list.selectFile(at) && press(router, KeyPress::Return) && list.directory() == at;
    }
    char msg[128];
    std::snprintf(msg, sizeof(msg), "a folder %d characters deep is listed and walked into", (int) deep.size());
    check(walked && list.numEntries() == 1 && list.entry(0).name == "inside.wav" &&
              fileExists(list.entry(0).path),
          msg);
    list.setDirectory(base);
  }
  root.removeChild(&list);
  root.removeChild(&preview);

  // ── FileTreeComponent ──
  FileTreeComponent tree;
  root.addChild(&tree);
  tree.setBounds({0.0f, 0.0f, 300.0f, 400.0f});
  tree.setDirectory(base);
  check(tree.tree().numVisibleRows() == 6, "the folder, open, with its five entries");
  tree.setPreview(&preview);
  check(tree.selectPath(joinPath(base, "a.png")) && tree.selectedFile() == joinPath(base, "a.png") && preview.hasImage(),
        "selecting a.png in the tree previews it");
  check(tree.selectPath(joinPath(drums, "kick.wav")) && !preview.hasImage(), "selecting a deeper file opens its folder");
  root.removeChild(&tree);

  // ── FilenameComponent ──
  FilenameComponent name("Sample", "", true, false, false, "*.wav;*.aif", ".wav", "No file");
  root.addChild(&name);
  name.setBounds({0.0f, 0.0f, 400.0f, 26.0f});
  int fileChanges = 0;
  name.onFileChanged = [&](const std::string&) { ++fileChanges; };
  router.setFocus(&name.editor());
  typeText(router, "loop");
  press(router, KeyPress::Return);
  check(name.currentFile() == "loop.wav" && fileChanges == 1, "typed loop and Return: the suffix is enforced");
  check(name.recentlyUsedFilenames().size() == 1 && name.recentlyUsedFilenames()[0] == "loop.wav", "and it is recent");
  name.setRecentlyUsedFilenames({"a.wav", "b.wav", "c.wav"});
  name.addRecentlyUsedFile("b.wav");
  check(name.recentlyUsedFilenames() == std::vector<std::string>({"b.wav", "a.wav", "c.wav"}), "a re-used file moves to the top");
  name.setMaxNumberOfRecentFiles(2);
  check(name.recentlyUsedFilenames().size() == 2, "the list is trimmed to its maximum");
  {
    Component* recentButton = nullptr;
    for (Component* c : name.children())
      if (c->accessibleName() == "Recent files") recentButton = c;
    click(router, centreOf(*recentButton));
    check(name.recentFilesMenu().isOpen() && name.recentFilesMenu().menu().numItems() == 2, "the drop-down lists the recent files");
    click(router, name.recentFilesMenu().itemBounds(1).centre());
    check(name.currentFile() == "a.wav" && name.recentlyUsedFilenames()[0] == "a.wav", "choosing one sets it and moves it up");
  }
  // Browse, through the dialog.
  name.setCurrentFile(joinPath(drums, "kick.wav"), false);
  click(router, centreOf(name.browseButton()));
  FileChooserDialogBox* dialog = name.browserDialog();
  check(dialog && dialog->isShowing() && dialog->currentDirectory() == drums, "browse opens the dialog in the file's folder");
  if (dialog) {
    check(dialog->filenameEditor().getText() == "kick.wav", "with its name in the field");
    FileListComponent& dl = dialog->fileList();
    const Point snare = dl.list().localToRoot({60.0f, FileListComponent::kRowHeight * 1.5f});
    click(router, snare);
    check(dialog->filenameEditor().getText() == "snare.wav", "selecting snare puts its name in the field");
    {
      Bitmap bmp = render(root);
      check(inkedPixels(bmp, Colour(40, 40, 40)) > 3000, "the dialog renders");
    }
    click(router, centreOf(dialog->okButton()));
    check(!dialog->isShowing() && name.currentFile() == joinPath(drums, "snare.wav"), "Open sets the field");
  }
  // A file dropped from outside.
  router.filesDropped({joinPath(base, "song.WAV")}, centreOf(name.editor()));
  check(name.currentFile() == joinPath(base, "song.WAV"), "a dropped file is taken");
  check(!router.fileDragMove({drums}, centreOf(name)), "a folder dragged over a file field is refused");
  root.removeChild(&name);

  // ── FileChooserDialogBox: save, open, folder ──
  FileChooserDialogBox save("Save", "Pick a name", FileChooserDialogBox::Mode::saveFile);
  root.addChild(&save);
  std::string result = "unset";
  check(save.show(base, [&](const std::string& r) { result = r; }), "a dialog with a parent shows");
  check(router.focused() == &save.filenameEditor(), "saving starts in the name field");
  typeText(router, "notes.txt");
  press(router, KeyPress::Return);
  check(save.isAskingAQuestion() && result == "unset", "saving over notes.txt asks first");
  press(router, KeyPress::Return);
  check(result == joinPath(base, "notes.txt") && !save.isShowing(), "Replace answers with the path");
  result = "unset";
  save.show(base, [&](const std::string& r) { result = r; });
  typeText(router, "fresh.txt");
  click(router, centreOf(save.okButton()));
  check(result == joinPath(base, "fresh.txt"), "a new name is saved without a question");
  save.show(base, [&](const std::string& r) { result = r; });
  router.setFocus(&save.fileList().list());
  press(router, KeyPress::Tab);
  check(router.focused() == &save.filenameEditor(), "Tab moves within the dialog");
  press(router, KeyPress::Tab, false, true);
  check(router.focused() == &save.fileList().list(), "Shift+Tab goes back, and never out of it");
  press(router, KeyPress::Escape);
  check(result.empty() && !save.isShowing(), "Escape cancels with an empty answer");

  FileChooserDialogBox open("Open", "", FileChooserDialogBox::Mode::openFile);
  root.addChild(&open);
  open.show(joinPath(base, "missing.txt"), [&](const std::string& r) { result = r; });
  result = "unset";
  click(router, centreOf(open.okButton()));
  check(open.isAskingAQuestion() && result == "unset", "a file that does not exist is not opened");
  press(router, KeyPress::Escape);
  check(!open.isAskingAQuestion() && open.isShowing(), "the message goes, the dialog stays");
  click(router, centreOf(open.cancelButton()));
  check(result.empty(), "Cancel answers empty");

  FileChooserDialogBox folder("Folder", "", FileChooserDialogBox::Mode::chooseDirectory);
  root.addChild(&folder);
  folder.show(base, [&](const std::string& r) { result = r; });
  check(folder.fileList().numEntries() == 2, "choosing a folder lists only folders");
  click(router, folder.fileList().list().localToRoot({60.0f, FileListComponent::kRowHeight * 0.5f}));
  click(router, centreOf(folder.okButton()));
  check(result == joinPath(base, "Bass"), "the selected folder is the answer");
  check(folder.accessibleInfo().role == AccessibleRole::Dialog, "a reader hears a dialog");

  // ── FileSearchPathListComponent ──
  FileSearchPathListComponent paths;
  root.addChild(&paths);
  paths.setBounds({0.0f, 0.0f, 400.0f, 200.0f});
  int pathChanges = 0;
  paths.onChange = [&]() { ++pathChanges; };
  const std::string bass = joinPath(base, "Bass");
  check(paths.addDirectory(bass) && paths.addDirectory(drums) && !paths.addDirectory(bass), "two folders, no duplicates");
  check(paths.pathString() == bass + ";" + drums && pathChanges == 2, "joined with ;");
  paths.list().setSelectedRow(0);
  click(router, centreOf(paths.moveDownButton()));
  check(paths.path()[0] == drums && paths.path()[1] == bass && paths.list().selectedRow() == 1, "Down moves the selected folder down");
  paths.onBrowseForDirectory = [&](std::function<void(const std::string&)> answer) { answer(base); };
  click(router, centreOf(paths.addButton()));
  check(paths.path().size() == 3 && paths.path()[2] == base, "+ adds what the chooser answers");
  click(router, centreOf(paths.removeButton()));
  check(paths.path().size() == 2, "- removes the selected folder");
  paths.setPathString("x;y;;x");
  check(paths.path() == std::vector<std::string>({"x", "y"}), "setPathString drops empties and repeats");
  router.filesDropped({drums, joinPath(base, "notes.txt")}, centreOf(paths.list()));
  check(paths.path().size() == 3 && paths.path()[2] == drums, "a dropped folder is added, a dropped file ignored");
  check(inkedPixels(render(root), Colour(40, 40, 40)) > 500, "the search path list renders");
}

// ── Handlers that pull the widget away, and menus that outlive their list ──
//
// Each of these ran on freed memory (the sanitize build sees it) or acted on
// the wrong thing (any build sees it).

struct ClosingMenuModel : MenuBarModel {
  std::function<void()> onSelect;
  std::vector<std::string> getMenuBarNames() override { return {"File"}; }
  PopupMenu getMenuForIndex(int, const std::string&) override {
    PopupMenu m;
    m.addItem(1, "Close the panel");
    return m;
  }
  void menuItemSelected(int, int) override {
    if (onSelect) onSelect();
  }
};

struct CountingFactory : ToolbarItemFactory {
  std::vector<std::string> log;
  void getAllToolbarItemIds(std::vector<int>& ids) override { ids = {1, 2, 3, 4, 5, 6}; }
  void getDefaultItemSet(std::vector<int>& ids) override { ids = {1, 2, 3, 4, 5, 6}; }
  std::unique_ptr<ToolbarItemComponent> createItem(int id) override {
    std::unique_ptr<ToolbarButton> b(new ToolbarButton(id, "Item " + std::to_string(id)));
    b->onClick = [this, id]() { log.push_back("Item " + std::to_string(id)); };
    return std::unique_ptr<ToolbarItemComponent>(b.release());
  }
};

/** A `W` in a window-sized tree whose handler, armed by `arm`, deletes it;
 *  `act` drives the router to fire it. True when the handler ran once and
 *  the control was gone after -- what it touched of itself after the handler
 *  is what ASan reads as heap-use-after-free. */
template <typename W, typename Arm, typename Act>
bool deletedByOwnHandler(Arm arm, Act act) {
  Widget root;
  root.setFont(g_font);
  root.setBounds({0, 0, 400, 300});
  Button elsewhere("Elsewhere");
  elsewhere.setBounds({300, 250, 90, 30});
  root.addChild(&elsewhere);
  W* w = new W();
  w->setBounds({20, 20, 240, 120});
  root.addChild(w);
  MouseRouter router(root);
  int fired = 0;
  // Nothing captured is touched after the delete: the lambda lives in the
  // control it deletes.
  arm(*w, std::function<void()>([&]() {
        ++fired;
        W* gone = w;
        w = nullptr;
        delete gone;
      }));
  act(router, *w, elsewhere);
  render(root);
  const bool ok = fired == 1 && w == nullptr;
  delete w;
  return ok;
}

void testHiddenFocus() {
  std::printf("\n-- the keyboard leaves what is hidden --\n");
  // A plugin's own shortcut switches pages while a field on the first one
  // has the keyboard. The field kept it: what was typed next went into a
  // field nobody could see, and Return committed it.
  Widget root;
  root.setFont(g_font);
  root.setBounds({0, 0, 400, 300});
  TabbedComponent tabs;
  tabs.setBounds({0, 0, 400, 300});
  root.addChild(&tabs);
  Component mainPage, settings;
  TextEditor name;
  name.setBounds({10, 10, 200, 26});
  mainPage.addChild(&name);
  Slider knob(Slider::Style::LinearHorizontal);
  knob.setBounds({10, 10, 200, 26});
  settings.addChild(&knob);
  tabs.addTab("Main", &mainPage);
  tabs.addTab("Settings", &settings);
  MouseRouter router(root);
  router.onUnhandledKey = [&](const KeyPress& k) {
    if (!k.ctrlDown || k.character != '2') return false;
    tabs.setCurrentTab(1);
    return true;
  };
  std::vector<std::string> committed;
  int returns = 0;
  name.onFocusLost = [&](const std::string& t) { committed.push_back(t); };
  name.onReturn = [&](const std::string&) { ++returns; };
  router.setFocus(&name);
  typeText(router, "Pad");
  type(router, '2', /*ctrl=*/true);
  check(tabs.currentTab() == 1 && router.focused() == nullptr && !name.hasKeyboardFocus(),
        "a page switched by a shortcut takes the keyboard from the field on the page it hid");
  check(committed.size() == 1 && committed[0] == "Pad",
        "and the field commits what was typed, as a click elsewhere would have made it");
  const bool taken = type(router, 'x');
  press(router, KeyPress::Return);
  char msg[200];
  std::snprintf(msg, sizeof(msg), "the next keys reach nothing hidden (field \"%s\", %d Return)",
                name.getText().c_str(), returns);
  check(!taken && name.getText() == "Pad" && returns == 0, msg);
  press(router, KeyPress::Tab);
  check(router.focused() == &knob, "and Tab goes on from the page that is showing");
}

void testHandlersThatDelete() {
  std::printf("\n-- handlers that delete, menus that go stale --\n");
  {
    // The basic controls. Each touched itself after a handler that its owner
    // may answer by deleting it: a switch that swaps the panel it sits in, a
    // value that rebuilds the editor around it, a drag that ends in a
    // re-layout, notes committed as the focus leaves, a tree in a picker
    // that closes on a choice.
    using Kill = std::function<void()>;
    auto toggle = [](Button& b, Kill k) {
      b.setToggleable(true);
      b.onToggle = [k](bool) { k(); };
      b.onClick = [] {};
    };
    check(deletedByOwnHandler<Button>(toggle, [](MouseRouter& r, Button& b, Component&) { click(r, centreOf(b)); }),
          "a toggle deleted by its onToggle, clicked");
    check(deletedByOwnHandler<Button>(toggle,
                                      [](MouseRouter& r, Button& b, Component&) {
                                        r.setFocus(&b);
                                        press(r, KeyPress::Return);
                                      }),
          "a toggle deleted by its onToggle, pressed with Return");
    auto onValue = [](Slider& s, Kill k) {
      s.setStyle(Slider::Style::LinearHorizontal);
      s.onDragStart = [] {};
      s.onDragEnd = [] {};
      s.onValueChange = [k](float) { k(); };
    };
    check(deletedByOwnHandler<Slider>(onValue, [](MouseRouter& r, Slider& s, Component&) {
            click(r, centreOf(s, Rect(150.0f, 0.0f, 10.0f, 10.0f)));
          }), "a slider deleted by its onValueChange, clicked on the track");
    check(deletedByOwnHandler<Slider>(onValue, [](MouseRouter& r, Slider& s, Component&) {
            r.setFocus(&s);
            press(r, KeyPress::Up);
          }), "a slider deleted by its onValueChange, stepped with a key");
    check(deletedByOwnHandler<Slider>(onValue, [](MouseRouter& r, Slider& s, Component&) {
            r.mouseWheel(centreOf(s), 1.0f);
          }), "a slider deleted by its onValueChange, by the wheel");
    check(deletedByOwnHandler<Slider>(onValue, [](MouseRouter&, Slider& s, Component&) {
            s.setAccessibleValue(0.7);
          }), "a slider deleted by its onValueChange, set by a reader");
    check(deletedByOwnHandler<Slider>([](Slider& s, Kill k) { s.onDragEnd = [k] { k(); }; },
                                      [](MouseRouter& r, Slider& s, Component&) { click(r, centreOf(s)); }),
          "a slider deleted by its onDragEnd");
    check(deletedByOwnHandler<RangeSlider>([](RangeSlider& s, Kill k) { s.onDragEnd = [k] { k(); }; },
                                           [](MouseRouter& r, RangeSlider& s, Component&) {
                                             click(r, centreOf(s));
                                           }),
          "a range slider deleted by its onDragEnd");
    check(deletedByOwnHandler<TextArea>([](TextArea& t, Kill k) {
            t.setText("notes");
            t.onFocusLost = [k](const std::string&) { k(); };
          },
                                        [](MouseRouter& r, TextArea& t, Component& elsewhere) {
                                          r.setFocus(&t);
                                          click(r, centreOf(elsewhere));
                                        }),
          "a text area deleted by its onFocusLost, as a click takes the focus");
    TreeNode tree;
    tree.addChild("Kick");
    tree.addChild("Snare");
    check(deletedByOwnHandler<TreeView>([&tree](TreeView& v, Kill k) {
            v.setRoot(&tree);
            v.onSelectionChange = [k](TreeNode*) { k(); };
          },
                                        [](MouseRouter& r, TreeView& v, Component&) {
                                          click(r, v.localToRoot({120.0f, 5.0f}));
                                        }),
          "a tree deleted by its onSelectionChange, clicked");
  }
  {
    // The model's handler closes the panel the burger list is in.
    ClosingMenuModel model;
    Widget root;
    root.setFont(g_font);
    root.setBounds({0, 0, 300, 300});
    BurgerMenuComponent* burger = new BurgerMenuComponent(&model);
    int invoked = 0;
    burger->onItemInvoked = [&invoked](int, int) { ++invoked; };
    root.addChild(burger);
    burger->setBounds({0, 0, 300, 300});
    MouseRouter router(root);
    model.onSelect = [&]() {
      delete burger;
      burger = nullptr;
    };
    click(router, centreOf(*burger, burger->rowBounds(1)));
    check(burger == nullptr && invoked == 0, "a burger list deleted by its model's handler is not touched after");
    delete burger;
  }
  for (int viaFocus = 0; viaFocus < 2; ++viaFocus) {
    // A picker that closes once a colour is typed: Return, or a click away.
    Widget root;
    root.setFont(g_font);
    root.setBounds({0, 0, 400, 400});
    Button elsewhere("Elsewhere");
    root.addChild(&elsewhere);
    elsewhere.setBounds({300, 350, 90, 30});
    ColourSelector* selector = new ColourSelector();
    root.addChild(selector);
    selector->setBounds({0, 0, 280, 340});
    MouseRouter router(root);
    Colour got;
    // Everything the lambda captures is touched BEFORE the delete: the
    // lambda lives in the selector it deletes.
    selector->onChange = [&](Colour c) {
      got = c;
      ColourSelector* doomed = selector;
      selector = nullptr;
      delete doomed;
    };
    router.setFocus(&selector->hexEditor());
    type(router, 'a', true);
    typeText(router, "FF0000");
    if (viaFocus) click(router, centreOf(elsewhere));
    else press(router, KeyPress::Return);
    check(selector == nullptr && got == Colour(255, 0, 0),
          viaFocus ? "a selector deleted by onChange from a hex code committed by clicking away"
                   : "a selector deleted by onChange from a hex code committed with Return");
    delete selector;
  }
  {
    // The key editor's owner rebuilds it when the mappings change.
    CommandManager cm;
    cm.registerCommand({1, "Save", "File", ""}, {Shortcut::fromCharacter('s', true)});
    cm.registerCommand({2, "Open", "File", ""}, {Shortcut::fromCharacter('o', true)});
    Widget root;
    root.setFont(g_font);
    root.setBounds({0, 0, 600, 400});
    KeyMappingEditorComponent* editor = new KeyMappingEditorComponent(cm);
    root.addChild(editor);
    editor->setBounds({0, 0, 600, 400});
    MouseRouter router(root);
    editor->onMappingsChanged = [&]() {
      KeyMappingEditorComponent* doomed = editor;
      editor = nullptr;
      delete doomed;
    };
    editor->startKeyCapture(1);
    type(router, 'o', true);
    const bool asked = editor && editor->isShowingQuestion();
    press(router, KeyPress::Return); // Reassign
    check(asked && editor == nullptr && cm.commandForShortcut(Shortcut::fromCharacter('o', true)) == 1,
          "a key editor deleted by onMappingsChanged after Reassign");
    delete editor;
  }
  {
    // Reassigning would take the key FROM a read-only command.
    CommandManager cm;
    cm.registerCommand({1, "Save", "File", ""}, {Shortcut::fromCharacter('s', true)});
    cm.registerCommand({2, "Play", "Transport", ""}, {Shortcut::fromCharacter('p')});
    Widget root;
    root.setFont(g_font);
    root.setBounds({0, 0, 600, 400});
    KeyMappingEditorComponent editor(cm);
    editor.isCommandReadOnly = [](int id) { return id == 2; };
    root.addChild(&editor);
    editor.setBounds({0, 0, 600, 400});
    MouseRouter router(root);
    editor.startKeyCapture(1);
    type(router, 'p', true);
    const bool told = editor.isShowingQuestion();
    press(router, KeyPress::Return);
    check(told && cm.shortcutsFor(2).size() == 1 && cm.commandForShortcut(Shortcut::fromCharacter('p')) == 2,
          "a read-only command's key is not taken by another command");
  }
  {
    // "Change this key" after the command's keys changed under the open menu.
    CommandManager cm;
    cm.registerCommand({1, "Save", "File", ""}, {Shortcut::fromCharacter('a', true), Shortcut::fromCharacter('b', true),
                                                 Shortcut::fromCharacter('c', true)});
    Widget root;
    root.setFont(g_font);
    root.setBounds({0, 0, 700, 400});
    KeyMappingEditorComponent editor(cm);
    root.addChild(&editor);
    editor.setBounds({0, 0, 700, 400});
    MouseRouter router(root);
    const int row = editor.rowForCommand(1);
    click(router, centreOf(editor, editor.chipBounds(row, 1))); // Ctrl+B
    const bool open = editor.keyMenu().isOpen();
    cm.removeShortcut(1, Shortcut::fromCharacter('a', true));
    click(router, editor.keyMenu().itemBounds(1).centre()); // Change this key...
    press(router, KeyPress::F7);
    check(open && cm.commandForShortcut(Shortcut::fromCharacter('b', true)) == 0 &&
              cm.commandForShortcut(Shortcut::fromCharacter('c', true)) == 1,
          "Change replaces the key its menu was opened on, not the one now at its index");
  }
  {
    // An item removed while the overflow menu is up.
    Widget root;
    root.setFont(g_font);
    root.setBounds({0, 0, 600, 300});
    CountingFactory factory;
    Toolbar tb;
    tb.addDefaultItems(factory);
    root.addChild(&tb);
    tb.setBounds({0, 0, 200, 32});
    MouseRouter router(root);
    tb.showOverflowMenu(true);
    int firstIndex = -1;
    for (int i = 0; i < tb.overflowMenu().menu().numItems() && firstIndex < 0; ++i)
      if (tb.overflowMenu().menu().itemAt(i).id > 0) firstIndex = i;
    const std::string listed = firstIndex >= 0 ? tb.overflowMenu().menu().itemAt(firstIndex).text : std::string();
    tb.removeToolbarItem(0);
    if (firstIndex >= 0) click(router, tb.overflowMenu().itemBounds(firstIndex).centre());
    check(!listed.empty() && factory.log.size() == 1 && factory.log.back() == listed,
          "the overflow item chosen is the one that runs, after the toolbar changed under the menu");
  }
}

} // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  auto face = loadAnyTypeface();
  if (face) g_font = Font(face, 13.0f);
  std::printf("Sonore SDK GUI widget tests (%s font)\n", face ? "with a" : "without a");
  testColourSelector();
  testToolbar();
  testMenuBar();
  testSidePanel();
  testMultiDocument();
  testKeyMappingEditor();
  testFileComponents();
  testHandlersThatDelete();
  testHiddenFocus();
  std::printf("%d checks, %d failed\n", g_checks, g_failures);
  std::printf(g_failures == 0 ? "GUI WIDGETS TESTS PASSED\n" : "GUI WIDGETS TESTS FAILED\n");
  return g_failures == 0 ? 0 : 1;
}
