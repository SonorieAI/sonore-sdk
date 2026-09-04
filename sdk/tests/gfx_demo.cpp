// SPDX-License-Identifier: Apache-2.0
// Renders a plugin panel with the native graphics stack and writes a BMP.
//
// Not a test -- the tests assert pixels. This is so a HUMAN can look at what
// those pixels add up to, which is the one thing no assertion covers.
#include <sonore/gfx/font.h>
#include <sonore/gfx/graphics.h>
#include <sonore/gfx/widgets.h>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

using namespace sonore::gfx;

static bool writeBmp(const char* path, const Bitmap& b) {
  FILE* f = std::fopen(path, "wb");
  if (!f) return false;
  const int w = b.width(), h = b.height();
  const int rowBytes = w * 3;
  const int pad = (4 - (rowBytes % 4)) % 4;
  const int imageBytes = (rowBytes + pad) * h;
  const int fileBytes = 54 + imageBytes;
  unsigned char header[54] = {0};
  header[0] = 'B'; header[1] = 'M';
  auto put32 = [&](int at, int v) {
    header[at] = (unsigned char) (v & 0xFF);
    header[at + 1] = (unsigned char) ((v >> 8) & 0xFF);
    header[at + 2] = (unsigned char) ((v >> 16) & 0xFF);
    header[at + 3] = (unsigned char) ((v >> 24) & 0xFF);
  };
  put32(2, fileBytes);
  put32(10, 54);
  put32(14, 40);
  put32(18, w);
  put32(22, h);
  header[26] = 1;
  header[28] = 24;
  put32(34, imageBytes);
  std::fwrite(header, 1, 54, f);
  std::vector<unsigned char> row((size_t) (rowBytes + pad), 0);
  for (int y = h - 1; y >= 0; --y) {   // BMP rows run bottom-up
    for (int x = 0; x < w; ++x) {
      const Colour c = b.pixelAt(x, y).toStraight();
      row[(size_t) x * 3 + 0] = c.b;
      row[(size_t) x * 3 + 1] = c.g;
      row[(size_t) x * 3 + 2] = c.r;
    }
    std::fwrite(row.data(), 1, row.size(), f);
  }
  std::fclose(f);
  return true;
}

int main(int argc, char** argv) {
  const char* out = argc > 1 ? argv[1] : "gfx_demo.bmp";

  std::shared_ptr<Typeface> face;
  const char* fonts[] = {
      "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
  };
  for (const char* p : fonts) {
    FILE* f = std::fopen(p, "rb");
    if (!f) continue;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> bytes((size_t) (n > 0 ? n : 0));
    if (n > 0) { if (std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) bytes.clear(); }
    std::fclose(f);
    if (bytes.empty()) continue;
    auto t = std::make_shared<Typeface>();
    if (t->load(std::move(bytes))) { face = t; break; }
  }
  if (!face) { std::printf("no font found\n"); return 1; }

  Bitmap bmp(560, 280);
  Graphics g(bmp);
  const Font title(face, 20.0f), small(face, 12.0f), value(face, 15.0f);

  g.fillAll(palette::background());

  g.setColour(palette::panel());
  g.fillRoundedRect(Rect(12.0f, 12.0f, 536.0f, 256.0f), 10.0f);
  g.setColour(palette::outline());
  g.drawRoundedRect(Rect(12.0f, 12.0f, 536.0f, 256.0f), 10.0f, 1.0f);

  g.setColour(palette::text());
  title.draw(g, "Sonore Reverb", 30.0f, 48.0f);
  g.setColour(palette::dimText());
  small.draw(g, "native renderer \xe2\x80\x94 no webview", 30.0f, 68.0f);

  // Real widgets in a real component tree -- the same objects a plugin editor
  // holds, painted through paintTree().
  Component page;
  page.setBounds(Rect(0.0f, 0.0f, 560.0f, 280.0f));

  const char* names[] = {"Size", "Damp", "Width", "Mix"};
  const float values[] = {0.72f, 0.35f, 0.9f, 0.5f};
  Slider knobs[4];
  Label captions[4];
  for (int i = 0; i < 4; ++i) {
    knobs[i].setBounds(Rect(34.0f + (float) i * 92.0f, 86.0f, 80.0f, 80.0f));
    knobs[i].setValue(values[i], false);
    knobs[i].setFont(small);
    page.addChild(&knobs[i]);
    captions[i].setText(names[i]);
    captions[i].setJustify(Justify::Centred);
    captions[i].setBounds(Rect(34.0f + (float) i * 92.0f, 168.0f, 80.0f, 14.0f));
    captions[i].setFont(small);
    page.addChild(&captions[i]);
  }

  Slider preDelay(Slider::Style::LinearHorizontal);
  preDelay.setBounds(Rect(34.0f, 216.0f, 320.0f, 20.0f));
  preDelay.setValue(0.62f, false);
  page.addChild(&preDelay);

  Button bypass("Bypass");
  bypass.setBounds(Rect(400.0f, 88.0f, 78.0f, 26.0f));
  bypass.setToggleable(true);
  bypass.setToggled(true, false);
  bypass.setFont(small);
  page.addChild(&bypass);

  ComboBox mode;
  mode.setBounds(Rect(400.0f, 122.0f, 122.0f, 26.0f));
  mode.setItems({"Hall", "Plate", "Chamber"});
  mode.setSelectedIndex(1, false);
  mode.setFont(small);
  page.addChild(&mode);

  page.paintTree(g);

  // A horizontal slider and a level meter, the other two idioms a plugin uses.
  g.setColour(palette::dimText());
  small.draw(g, "Pre-delay", 34.0f, 210.0f);
  g.setColour(palette::text());
  value.drawIn(g, "62 ms", Rect(364.0f, 217.0f, 60.0f, 18.0f), Justify::Right);

  for (int i = 0; i < 12; ++i) {
    const Rect seg(440.0f + (float) i * 8.0f, 218.0f, 5.0f, 16.0f);
    g.setColour(i < 8 ? palette::accent() : (i < 10 ? palette::warning() : palette::outline()));
    g.fillRoundedRect(seg, 1.5f);
  }
  g.setColour(palette::dimText());
  small.draw(g, "OUT", 440.0f, 214.0f);

  if (!writeBmp(out, bmp)) { std::printf("could not write %s\n", out); return 1; }
  std::printf("wrote %s (%dx%d)\n", out, bmp.width(), bmp.height());
  return 0;
}
