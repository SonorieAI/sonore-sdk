// SPDX-License-Identifier: Apache-2.0
// The whole native toolkit under random hands.
//
// fuzz_ui drives the generic editor. A plugin with an interface of its own
// is built from the rest of the toolkit -- text fields, lists, tables, trees,
// tabs, scrolling viewports, collapsible panels, range sliders, a keyboard --
// and each of those has its own state machine of hover, press, drag, focus,
// selection, scrolling and editing. This lays every one of them out in one
// window-sized tree and feeds it random mouse, wheel, keyboard, focus, resize
// and file-drop events through the MouseRouter a window peer uses, painting
// the tree every few events. Properties:
//   * nothing crashes or reads out of bounds (run it under ASan for that);
//   * what the widgets report stays inside what they were given: a selected
//     row exists, a sort column exists, a range has low <= high inside 0..1,
//     a note is a MIDI note, a text field never holds more than its limit.
//
//   fuzz_widgets [--steps N] [--seed S]
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <sonore/gfx/bitmap.h>
#include <sonore/gfx/concertina.h>
#include <sonore/gfx/containers.h>
#include <sonore/gfx/graphics.h>
#include <sonore/gfx/group.h>
#include <sonore/gfx/midi_keyboard.h>
#include <sonore/gfx/table.h>
#include <sonore/gfx/text_area.h>
#include <sonore/gfx/text_editor.h>
#include <sonore/gfx/treeview.h>
#include <sonore/gfx/truetype.h>
#include <sonore/gfx/value_box.h>
#include <sonore/gfx/viewport.h>
#include <sonore/gfx/widgets.h>
#include <sonore/gfx/widgets2.h>

using namespace sonore::gfx;

namespace {

std::shared_ptr<Typeface> loadAnyTypeface() {
  const char* candidates[] = {
#if defined(_WIN32)
      "C:/Windows/Fonts/arial.ttf", "C:/Windows/Fonts/segoeui.ttf",
#endif
      "/system/fonts/RobotoStatic-Regular.ttf", // Android
      "/system/fonts/Roboto-Regular.ttf",
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

int g_failures = 0;
// What the run reached: a callback that never fired is a widget the events
// never got to, and a pass that touched nothing is no pass.
enum Reached {
  kText, kList, kTableSelect, kTableSort, kTree, kTab, kRange, kNote, kSection, kCombo, kNotes, kReachedCount
};
const char* const kReachedNames[kReachedCount] = {"text", "list", "table row", "table sort", "tree", "tab",
                                                  "range", "note", "section", "combo", "notes"};
long g_reached[kReachedCount] = {};
void fail(const char* what, double a, double b = 0.0) {
  ++g_failures;
  if (g_failures <= 10) std::printf("  FAIL %s (%g, %g)\n", what, a, b);
}

} // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  size_t steps = 4000;
  uint32_t seed = 0x71D6u;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--steps") == 0 && i + 1 < argc) steps = std::strtoull(argv[++i], nullptr, 10);
    if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) seed = (uint32_t) std::strtoul(argv[++i], nullptr, 10);
  }
  auto face = loadAnyTypeface();
  const Font font = face ? Font(face, 13.0f) : Font();

  Widget root;
  root.setFont(font);
  float width = 900.0f, height = 700.0f;
  root.setBounds({0.0f, 0.0f, width, height});

  // ── Every widget ──
  TextEditor text;
  text.setMaxLength(24);
  text.setPlaceholder("name");
  text.onTextChange = [&](const std::string& t) {
    ++g_reached[kText];
    // Characters, not bytes: count UTF-8 lead bytes.
    int chars = 0;
    for (unsigned char c : t) chars += (c & 0xC0) != 0x80;
    if (chars > 24) fail("a text field holds more than its limit", chars, 24);
  };
  TextEditor digits;
  digits.setAllowedCharacters("0123456789.-");
  digits.setText("12.5");
  // Several lines: wrapping, up and down, a selection across lines.
  TextArea notes;
  notes.setMaxLength(300);
  notes.setText("A pad for slow tunes.\nTry the second macro\r\nwith the mod wheel.");
  notes.onTextChange = [&](const std::string& t) {
    ++g_reached[kNotes];
    int chars = 0;
    for (unsigned char c : t) chars += (c & 0xC0) != 0x80;
    if (chars > 300) fail("a text area holds more than its limit", chars, 300);
    if (t.find('\r') != std::string::npos) fail("a text area kept a CR", 0);
    if (notes.caretPosition() < 0 || notes.caretPosition() > chars) fail("the caret left the text", notes.caretPosition(), chars);
  };

  ListBox list;
  std::vector<std::string> items;
  for (int i = 0; i < 200; ++i) items.push_back("Item " + std::to_string(i));
  list.setItems(items);
  list.onSelectionChange = [&](int row) {
    ++g_reached[kList];
    if (row < -1 || row >= 200) fail("a list selected a row that does not exist", row);
  };

  TableListBox table;
  table.setColumns({{"Name", 160.0f}, {"Size", 80.0f}, {"Preview", 60.0f, 40.0f, false}});
  int tableRows = 500;
  table.setNumRows(tableRows);
  table.getCellText = [&](int row, int column) {
    if (row < 0 || row >= tableRows || column < 0 || column > 2) {
      fail("a table asked for a cell that does not exist", row, column);
      return std::string();
    }
    return "r" + std::to_string(row) + "c" + std::to_string(column);
  };
  table.onSelectionChange = [&](int row) {
    ++g_reached[kTableSelect];
    if (row < -1 || row >= tableRows) fail("a table selected a row that does not exist", row, tableRows);
  };
  table.onSortChanged = [&](int column, bool) {
    ++g_reached[kTableSort];
    if (column < 0 || column > 1) fail("a table sorted by a column that cannot sort", column);
  };

  TreeView tree;
  TreeNode treeRoot;
  treeRoot.text = "Library";
  treeRoot.mightHaveChildren = true;
  int depthBuilt = 0;
  tree.onNeedChildren = [&](TreeNode& node) {
    // Lazily, as a file tree does, and not forever.
    if (++depthBuilt > 400) return;
    for (int i = 0; i < 5; ++i) node.addChild(node.text + "/" + std::to_string(i), {}, i % 2 == 0);
  };
  tree.setRoot(&treeRoot);
  tree.onSelectionChange = [&](TreeNode*) { ++g_reached[kTree]; };

  TabbedComponent tabs;
  Label pageA("First page");
  Slider pageB(Slider::Style::Rotary);
  ValueBox pageC;
  tabs.addTab("One", &pageA);
  tabs.addTab("Two", &pageB);
  tabs.addTab("Three", &pageC);
  tabs.onTabChanged = [&](int index) {
    ++g_reached[kTab];
    if (index < 0 || index > 2) fail("a tab that does not exist was chosen", index);
  };

  Viewport viewport;
  GroupComponent big("Large content");
  big.setBounds({0.0f, 0.0f, 1600.0f, 1600.0f});
  std::vector<std::unique_ptr<Slider>> inBig;
  for (int i = 0; i < 12; ++i) {
    inBig.push_back(std::make_unique<Slider>(i % 3 == 0 ? Slider::Style::LinearVertical : Slider::Style::LinearHorizontal));
    inBig.back()->setBounds({40.0f + (float) (i % 4) * 380.0f, 40.0f + (float) (i / 4) * 500.0f, 300.0f, 120.0f});
    big.addChild(inBig.back().get());
  }
  viewport.setViewedComponent(&big);

  ConcertinaPanel concertina;
  Slider secA(Slider::Style::LinearBar), secB(Slider::Style::LinearHorizontal);
  ComboBox secC;
  secC.setItems({"Sine", "Saw", "Square", "Noise"});
  concertina.addSection("Oscillator", &secA, 40.0f, true);
  concertina.addSection("Filter", &secB, 40.0f);
  concertina.addSection("Wave", &secC, 30.0f);
  concertina.setMaximumOpen(2);
  concertina.onSectionToggled = [&](int index, bool) {
    ++g_reached[kSection];
    if (index < 0 || index > 2) fail("a section that does not exist was toggled", index);
  };

  RangeSlider range;
  range.onRangeChange = [&](float low, float high) {
    ++g_reached[kRange];
    if (!(low <= high) || low < -1e-6f || high > 1.0f + 1e-6f) fail("a range out of order or out of 0..1", low, high);
  };
  MidiKeyboard keys;
  keys.onNoteOn = [&](int note, float velocity) {
    ++g_reached[kNote];
    if (note < 0 || note > 127 || !(velocity >= 0.0f && velocity <= 1.0f)) fail("a note out of range", note, velocity);
  };
  keys.onNoteOff = [&](int note) {
    if (note < 0 || note > 127) fail("a note-off out of range", note);
  };
  ScrollBar scroll(true);
  scroll.setRange(1000.0f, 120.0f, 0.0f);
  ValueBox value;
  value.setText("-6.0 dB");
  value.onTextEntered = [](const std::string&) { return true; };
  ProgressBar progress;
  double progressValue = 0.25;
  progress.setSource(&progressValue);
  IconButton icon;
  icon.setIcon("<svg viewBox=\"0 0 24 24\"><path d=\"M4 4 L20 12 L4 20 Z\"/></svg>");
  icon.setToggleable(true);
  Button toggle("Bypass");
  toggle.setToggleable(true);
  Slider rotary(Slider::Style::Rotary), barV(Slider::Style::LinearBarVertical);
  ComboBox combo;
  combo.setItems({"A", "B", "C", "D", "E"});
  combo.onOpenMenu = [](ComboBox& box) {
    box.setSelectedIndex((box.selectedIndex() + 1) % 5);
  };
  combo.onChange = [&](int index) {
    ++g_reached[kCombo];
    if (index < 0 || index > 4) fail("a choice that does not exist was made", index);
  };

  Component* all[] = {&text, &digits, &list, &table, &tree, &tabs, &viewport, &concertina,
                      &range, &keys, &scroll, &value, &progress, &icon, &toggle, &rotary,
                      &barV, &combo, &notes};
  const int count = (int) (sizeof(all) / sizeof(all[0]));
  for (Component* c : all) root.addChild(c);
  auto layout = [&]() {
    // A grid of six columns, every cell its share of whatever the window is.
    const float cw = width / 6.0f, ch = height / 3.0f;
    for (int i = 0; i < count; ++i)
      all[i]->setBounds({(float) (i % 6) * cw + 4.0f, (float) (i / 6) * ch + 4.0f, cw - 8.0f, ch - 8.0f});
  };
  layout();
  MouseRouter router(root);

  std::mt19937 rng(seed);
  auto pick = [&](uint32_t m) { return m ? rng() % m : 0u; };
  auto chance = [&](int percent) { return (int) pick(100) < percent; };
  auto anywhere = [&]() {
    return Point{(float) pick((uint32_t) width + 40) - 20.0f, (float) pick((uint32_t) height + 40) - 20.0f};
  };
  const uint32_t typed[] = {'a', 'Z', '0', '7', '.', '-', ' ', '/', 0xE9, 0x4E2D, 0x1F3B5, 'q'};
  const int named[] = {KeyPress::Backspace, KeyPress::Tab, KeyPress::Return, KeyPress::Escape,
                       KeyPress::Delete, KeyPress::Left, KeyPress::Right, KeyPress::Up,
                       KeyPress::Down, KeyPress::Home, KeyPress::End, KeyPress::PageUp,
                       KeyPress::PageDown};
  bool down = false;
  size_t paints = 0;

  for (size_t step = 0; step < steps; ++step) {
    const uint32_t roll = pick(100);
    const bool shift = chance(15), ctrl = chance(12), alt = chance(5);
    if (roll < 28) {
      router.mouseMove(anywhere(), shift, ctrl, alt);
    } else if (roll < 40) {
      if (down) router.mouseUp(anywhere(), shift, ctrl, alt);
      // Each draw in its own statement: a call's arguments are evaluated in
      // an unspecified order, and a seed has to replay under any compiler.
      const Point at = anywhere();
      const int clicks = chance(15) ? (chance(30) ? 3 : 2) : 1;
      const bool right = chance(8);
      router.mouseDown(at, clicks, shift, ctrl, alt, right);
      down = true;
    } else if (roll < 50) {
      if (down) {
        router.mouseUp(anywhere(), shift, ctrl, alt);
        down = false;
      }
    } else if (roll < 60) {
      const Point at = anywhere();
      router.mouseWheel(at, (float) ((int) pick(13) - 6) * 0.5f, shift, ctrl, alt);
    } else if (roll < 80) {
      KeyPress key;
      if (chance(55)) key.character = typed[pick(sizeof(typed) / sizeof(typed[0]))];
      else key.keyCode = named[pick(sizeof(named) / sizeof(named[0]))];
      key.shiftDown = shift;
      key.ctrlDown = ctrl; // Ctrl+A, Ctrl+C, Ctrl+V and Ctrl+X among them
      key.altDown = alt;
      router.keyPressed(key);
    } else if (roll < 84) {
      router.setFocus(all[pick((uint32_t) count)]);
    } else if (roll < 87) {
      width = 200.0f + (float) pick(1400);
      height = 150.0f + (float) pick(1000);
      root.setBounds({0.0f, 0.0f, width, height});
      layout();
    } else if (roll < 89) {
      // The model under a widget changes, as a sample folder being rescanned does.
      tableRows = (int) pick(800);
      table.setNumRows(tableRows);
      if (chance(30)) list.setItems(std::vector<std::string>(items.begin(), items.begin() + pick(200)));
    } else if (roll < 91) {
      std::vector<std::string> files = {"C:/samples/kick.wav", "/tmp/a b/snare.flac"};
      const Point at = anywhere();
      if (chance(50)) router.fileDragMove(files, at);
      else if (chance(50)) router.filesDropped(files, at);
      else router.fileDragExit();
    } else if (roll < 93) {
      router.mouseExitWindow();
    } else if (roll < 98) {
      Bitmap bmp((int) width, (int) height);
      Graphics g(bmp);
      root.paintTree(g);
      ++paints;
    } else {
      progressValue = (double) pick(1001) / 1000.0;
      const float total = 1000.0f + (float) pick(5000);
      const float visible = 50.0f + (float) pick(500);
      const float start = (float) pick(3000);
      scroll.setRange(total, visible, start);
      const float viewX = (float) pick(2000) - 200.0f;
      const float viewY = (float) pick(2000) - 200.0f;
      viewport.setViewPosition(viewX, viewY);
    }
  }
  if (down) router.mouseUp({-50.0f, -50.0f});
  router.mouseExitWindow();
  router.setFocus(nullptr);

  std::printf("Sonore SDK widget fuzz -- %zu events, seed %u, %zu paints\n  reached:", steps, seed, paints);
  int unreached = 0;
  for (int r = 0; r < kReachedCount; ++r) {
    std::printf(" %s %ld%s", kReachedNames[r], g_reached[r], r + 1 < kReachedCount ? "," : "\n");
    if (g_reached[r] == 0) ++unreached;
  }
  // Most of them, every run: a layout or focus change that hid the widgets
  // from the events would otherwise pass in silence.
  if (unreached > 2) fail("the events reached too few of the widgets", unreached, kReachedCount);
  std::printf(g_failures == 0 ? "SONORE WIDGET FUZZ PASSED\n" : "SONORE WIDGET FUZZ FAILED\n");
  return g_failures == 0 ? 0 : 1;
}
