// SPDX-License-Identifier: Apache-2.0
// The application furniture under random hands.
//
// fuzz_widgets drives the everyday toolkit. This drives the rest of it: the
// colour selector, a customisable toolbar (and its customisation dialog), a
// menu bar and its burger list, a drawer, a multi-document panel adding and
// closing documents as it goes, the key mapping editor over a command
// manager, and the file components over a real folder -- with the overlays
// they open (menus, dialogs, questions) appearing and disappearing on top.
// Random mouse, wheel, keyboard (F10, Alt+letters, Ctrl+Tab among them),
// focus, resize, clock and model changes go through the MouseRouter a window
// peer uses, painting every few events. Properties:
//   * nothing crashes or reads out of bounds (run it under ASan for that);
//   * what the widgets report stays inside what they were given: a colour's
//     hue, saturation and brightness are in 0..1, a toolbar's saved layout
//     restores to itself, the active document is one of the documents, no
//     key belongs to two commands, a drawer is between shut and open.
//
//   fuzz_gui_widgets [--steps N] [--seed S]
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <sonore/commands.h>
#include <sonore/gfx/bitmap.h>
#include <sonore/gfx/colour_selector.h>
#include <sonore/gfx/file_chooser.h>
#include <sonore/gfx/graphics.h>
#include <sonore/gfx/key_mapping_editor.h>
#include <sonore/gfx/menu_bar.h>
#include <sonore/gfx/multi_document.h>
#include <sonore/gfx/side_panel.h>
#include <sonore/gfx/toolbar.h>
#include <sonore/gfx/truetype.h>

#include "temp_path.h"

using namespace sonore;
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
enum Reached {
  kColour, kToolbarClick, kToolbarLayout, kMenuItem, kBurger, kDrawer, kActiveDoc, kDocClosed,
  kMappings, kFileSelect, kFileChosen, kDialogAnswer, kPaths, kReachedCount
};
const char* const kReachedNames[kReachedCount] = {"colour", "toolbar click", "toolbar layout", "menu item",
                                                  "burger", "drawer", "active document", "document closed",
                                                  "key mapping", "file selected", "file chosen",
                                                  "dialog answer", "search path"};
long g_reached[kReachedCount] = {};

void fail(const char* what, double a, double b = 0.0) {
  ++g_failures;
  if (g_failures <= 10) std::printf("  FAIL %s (%g, %g)\n", what, a, b);
}

const char* kIcon = "<svg viewBox=\"0 0 24 24\"><path d=\"M4 4 L20 12 L4 20 Z\"/></svg>";

struct Factory : ToolbarItemFactory {
  void getAllToolbarItemIds(std::vector<int>& ids) override {
    ids = {1, 2, 3, 4, 5, 6, separatorBarId, spacerId, flexibleSpacerId};
  }
  void getDefaultItemSet(std::vector<int>& ids) override { ids = {1, 2, separatorBarId, 3, flexibleSpacerId, 4}; }
  std::unique_ptr<ToolbarItemComponent> createItem(int id) override {
    static const char* names[] = {"", "New", "Open", "Save", "Undo", "Redo", "A rather long label"};
    if (id < 1 || id > 6) return nullptr;
    std::unique_ptr<ToolbarButton> b(new ToolbarButton(id, names[id], id % 2 ? kIcon : ""));
    b->setToggleable(id == 5);
    b->onClick = []() { ++g_reached[kToolbarClick]; };
    return std::unique_ptr<ToolbarItemComponent>(b.release());
  }
};

struct Menus : MenuBarModel {
  int variant = 0;
  std::vector<std::string> getMenuBarNames() override {
    if (variant % 3 == 0) return {"&File", "&Edit", "View"};
    if (variant % 3 == 1) return {"File", "Edit", "View", "&Help", "A very long menu name"};
    return {};
  }
  PopupMenu getMenuForIndex(int index, const std::string&) override {
    PopupMenu m;
    m.addSectionHeader("Section");
    for (int i = 0; i < 3 + index * 7; ++i) {
      if (i % 4 == 3) m.addSeparator();
      m.addItem(index * 100 + i + 1, "Item " + std::to_string(i), i % 5 != 2, i % 3 == 0);
    }
    return m;
  }
  void menuItemSelected(int id, int top) override {
    ++g_reached[kMenuItem];
    if (id <= 0 || top < 0 || id / 100 != top) fail("a menu item from the wrong menu", id, top);
  }
};

struct Document : Widget {
  Button button{"Press"};
  TextEditor field;
  Document() {
    addChild(&button);
    addChild(&field);
  }
  void resized() override {
    button.setBounds({10.0f, 10.0f, 80.0f, 24.0f});
    field.setBounds({10.0f, 40.0f, 120.0f, 24.0f});
  }
};

bool under(const std::string& path, const std::string& base) {
  return path.size() >= base.size() && path.compare(0, base.size(), base) == 0;
}

/** A dialog's list kept inside the fuzz folder: walking up into a system
 *  folder with forty thousand entries is a test of the disk, not the code. */
void fence(FileListComponent& list, const std::string& base) {
  std::function<void(const std::string&)> inner = list.onDirectoryChanged;
  list.onDirectoryChanged = [&list, base, inner](const std::string& d) {
    if (!under(d, base)) list.setDirectory(base);
    else if (inner) inner(d);
  };
}

} // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  size_t steps = 4000;
  uint32_t seed = 0x6E1Du;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--steps") == 0 && i + 1 < argc) steps = std::strtoull(argv[++i], nullptr, 10);
    if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) seed = (uint32_t) std::strtoul(argv[++i], nullptr, 10);
  }
  auto face = loadAnyTypeface();
  const Font font = face ? Font(face, 13.0f) : Font();

  // A small folder to browse.
  namespace fs = std::filesystem;
  const std::string base = sonoretest::tempPath("fuzz_gui_widgets");
  std::error_code ec;
  fs::create_directories(fs::path(base) / "Drums" / "Deep", ec);
  fs::create_directories(fs::path(base) / "Bass", ec);
  for (const char* leaf : {"kick.wav", "snare.wav", "notes.txt", "pic.png", "broken.svg"}) {
    std::FILE* f = pathio::open(joinPath(base, leaf).c_str(), "wb");
    if (f) {
      std::fputs(leaf, f);
      std::fclose(f);
    }
  }

  Widget root;
  root.setFont(font);
  float width = 1000.0f, height = 760.0f;
  root.setBounds({0.0f, 0.0f, width, height});

  // ── Every widget ──
  ColourSelector colour;
  colour.setSwatches({Colour(255, 0, 0), Colour(0, 255, 0, 120), Colour(0, 0, 255)});
  colour.onChange = [&](Colour) {
    ++g_reached[kColour];
    const float h = colour.hue(), s = colour.saturation(), v = colour.brightness();
    if (!(h >= 0.0f && h <= 1.0f && s >= 0.0f && s <= 1.0f && v >= 0.0f && v <= 1.0f))
      fail("a colour outside HSV's ranges", h, s);
  };

  Factory factory;
  Toolbar toolbar;
  toolbar.addDefaultItems(factory);
  toolbar.onItemsChanged = [&]() {
    ++g_reached[kToolbarLayout];
    Toolbar check;
    if (!check.restoreFromString(factory, toolbar.toString()) || check.toString() != toolbar.toString())
      fail("a toolbar layout does not restore to itself", (double) toolbar.numItems());
  };

  Menus menus;
  MenuBarComponent bar(&menus);
  BurgerMenuComponent burger(&menus);
  burger.onItemInvoked = [](int, int) { ++g_reached[kBurger]; };

  Button drawerButton("Drawer button");
  SidePanel drawer("Drawer", 220.0f, true, &drawerButton);
  drawer.onPanelShowHide = [](bool) { ++g_reached[kDrawer]; };

  MultiDocumentPanel docs;
  docs.setMaximumNumDocuments(6);
  docs.onActiveDocumentChanged = [&](Component* active) {
    ++g_reached[kActiveDoc];
    if (active && docs.indexOf(active) < 0) fail("the active document is not a document", 0);
  };
  std::vector<std::function<void(bool)>> pendingAnswers;

  CommandManager commands;
  for (int i = 1; i <= 12; ++i)
    commands.registerCommand({i, "Command " + std::to_string(i), i % 3 == 0 ? "Edit" : (i % 3 == 1 ? "File" : "")},
                             {Shortcut::fromCharacter((uint32_t) ('a' + i), true)});
  KeyMappingEditorComponent keys(commands);
  keys.isCommandReadOnly = [](int id) { return id == 7; };
  keys.onMappingsChanged = [&]() {
    ++g_reached[kMappings];
    for (int id : commands.commandIds())
      for (const Shortcut& s : commands.shortcutsFor(id))
        if (commands.commandForShortcut(s) != id) fail("a key belongs to two commands", id, commands.commandForShortcut(s));
  };

  FileListComponent files;
  files.setDirectory(base);
  fence(files, base);
  files.onSelectionChange = [](const std::string&) { ++g_reached[kFileSelect]; };
  files.onFileChosen = [](const std::string&) { ++g_reached[kFileChosen]; };
  ImagePreviewComponent preview;
  files.setPreview(&preview);
  FileTreeComponent tree(base);
  tree.setPreview(&preview);
  tree.onSelectionChange = [](const std::string&) { ++g_reached[kFileSelect]; };

  FileChooserDialogBox chooser("Choose", "Anything", FileChooserDialogBox::Mode::saveFile);
  fence(chooser.fileList(), base);
  FileChooserDialogBox folderChooser("Folder", "", FileChooserDialogBox::Mode::chooseDirectory);
  fence(folderChooser.fileList(), base);
  FilenameComponent filename("Sample", joinPath(base, "kick.wav"), true, false, false, "*.wav", ".wav", "none");
  filename.setRecentlyUsedFilenames({joinPath(base, "snare.wav"), "x.wav"});
  filename.onBrowse = [&](FilenameComponent& self) {
    if (chooser.isShowing()) return;
    chooser.show(self.currentFile(), [&self](const std::string& chosen) {
      ++g_reached[kDialogAnswer];
      if (!chosen.empty()) self.setCurrentFile(chosen, true);
    });
  };
  FileSearchPathListComponent paths;
  paths.onChange = []() { ++g_reached[kPaths]; };
  paths.onBrowseForDirectory = [&](std::function<void(const std::string&)> answer) {
    if (folderChooser.isShowing()) return;
    folderChooser.show(base, [answer](const std::string& chosen) {
      ++g_reached[kDialogAnswer];
      answer(chosen);
    });
  };

  Component* grid[] = {&colour, &toolbar, &burger, &docs, &keys, &files, &preview, &tree, &filename, &paths};
  const int count = (int) (sizeof(grid) / sizeof(grid[0]));
  root.addChild(&bar);
  for (Component* c : grid) root.addChild(c);
  root.addChild(&drawer);
  root.addChild(&chooser);
  root.addChild(&folderChooser);
  auto layout = [&]() {
    bar.setBounds({0.0f, 0.0f, width, 24.0f});
    const float cw = width / 4.0f, ch = (height - 24.0f) / 3.0f;
    for (int i = 0; i < count; ++i)
      grid[i]->setBounds({(float) (i % 4) * cw + 3.0f, 24.0f + (float) (i / 4) * ch + 3.0f, cw - 6.0f, ch - 6.0f});
    drawer.coverParent();
    drawer.resized();
    if (chooser.isShowing()) chooser.coverParent();
    if (folderChooser.isShowing()) folderChooser.coverParent();
  };
  layout();
  MouseRouter router(root);
  router.onUnhandledKey = [&](const KeyPress& k) { return bar.handleShortcutKey(k) || commands.keyPressed(k); };

  std::mt19937 rng(seed);
  auto pick = [&](uint32_t m) { return m ? rng() % m : 0u; };
  auto chance = [&](int percent) { return (int) pick(100) < percent; };
  auto anywhere = [&]() {
    const float x = (float) pick((uint32_t) width + 40) - 20.0f;
    const float y = (float) pick((uint32_t) height + 40) - 20.0f;
    return Point{x, y};
  };
  const uint32_t typed[] = {'a', 'f', 'e', 'Z', '0', '7', '.', '#', ' ', 'c', 0xE9, 'n', 's'};
  const int named[] = {KeyPress::Backspace, KeyPress::Tab, KeyPress::Return, KeyPress::Escape,
                       KeyPress::Delete, KeyPress::Left, KeyPress::Right, KeyPress::Up,
                       KeyPress::Down, KeyPress::Home, KeyPress::End, KeyPress::PageUp,
                       KeyPress::PageDown, KeyPress::F10, KeyPress::F4, KeyPress::F5};
  bool down = false;
  size_t paints = 0;
  double now = 0.0;

  for (size_t step = 0; step < steps; ++step) {
    const uint32_t roll = pick(100);
    const bool shift = chance(15), ctrl = chance(12), alt = chance(8);
    now += 0.005 + (double) pick(40) / 1000.0;
    drawer.setInputClock(now);
    if (roll < 26) {
      router.mouseMove(anywhere(), shift, ctrl, alt);
    } else if (roll < 38) {
      if (down) router.mouseUp(anywhere(), shift, ctrl, alt);
      const Point at = anywhere();
      const int clicks = chance(15) ? 2 : 1;
      const bool right = chance(8);
      router.mouseMove(at, shift, ctrl, alt);
      router.mouseDown(at, clicks, shift, ctrl, alt, right);
      down = !right;
      // Half of all presses are CLICKS -- let go where they went down, as a
      // hand does. A release somewhere random cancels every button, and a
      // run made only of those never presses one.
      if (down && chance(50)) {
        router.mouseUp(at, shift, ctrl, alt);
        down = false;
      }
    } else if (roll < 47) {
      if (down) {
        router.mouseUp(anywhere(), shift, ctrl, alt);
        down = false;
      }
    } else if (roll < 52) {
      const Point at = anywhere();
      router.mouseWheel(at, (float) ((int) pick(13) - 6) * 0.5f, shift, ctrl, alt);
    } else if (roll < 72) {
      KeyPress key;
      if (chance(50)) key.character = typed[pick(sizeof(typed) / sizeof(typed[0]))];
      else key.keyCode = named[pick(sizeof(named) / sizeof(named[0]))];
      key.shiftDown = shift;
      key.ctrlDown = ctrl;
      key.altDown = alt;
      router.keyPressed(key);
    } else if (roll < 75) {
      Component* targets[] = {&colour, &keys, &burger, &files.list(), &filename.editor(), &paths.list(), &docs, nullptr};
      router.setFocus(targets[pick(8)]);
    } else if (roll < 77) {
      width = 300.0f + (float) pick(1300);
      height = 220.0f + (float) pick(900);
      root.setBounds({0.0f, 0.0f, width, height});
      layout();
    } else if (roll < 81) {
      // The drawer, the clock, and the model under the menus.
      const uint32_t what = pick(5);
      if (what == 0) drawer.showOrHide(!drawer.isPanelShowing(), now);
      else if (what == 1) drawer.setPositionOnLeft(chance(50));
      else if (what == 2) {
        menus.variant = (int) pick(3);
        menus.menuItemsChanged();
      } else if (what == 3) {
        burger.refresh();
      } else {
        bar.showMenu((int) pick(6) - 1, chance(50));
      }
      drawer.tick(now);
      const float f = drawer.openFraction();
      if (!(f >= 0.0f && f <= 1.0f)) fail("a drawer outside shut..open", f);
    } else if (roll < 86) {
      // Documents come and go, some asking first and answering later.
      const uint32_t what = pick(6);
      if (what < 2) {
        std::unique_ptr<Component> doc(new Document());
        docs.addDocument(std::move(doc), "Doc " + std::to_string(step), Colour(20, 30, (uint8_t) pick(255)));
      } else if (what == 2 && docs.numDocuments() > 0) {
        Component* victim = docs.document((int) pick((uint32_t) docs.numDocuments()));
        docs.closeDocument(victim, chance(50), [](bool closed) {
          if (closed) ++g_reached[kDocClosed];
        });
      } else if (what == 3) {
        docs.setLayoutMode(chance(50) ? MultiDocumentPanel::LayoutMode::FloatingWindows
                                      : MultiDocumentPanel::LayoutMode::MaximisedWindowsWithTabs);
        docs.useFullscreenWhenOneDocument(chance(30));
      } else if (what == 4 && !pendingAnswers.empty()) {
        std::function<void(bool)> answer = pendingAnswers.back();
        pendingAnswers.pop_back();
        answer(chance(70));
      } else if (docs.numDocuments() > 0) {
        docs.setActiveDocument(docs.document((int) pick((uint32_t) docs.numDocuments())));
      }
      docs.tryToCloseDocument = [&](Component*, std::function<void(bool)> answer) {
        if (chance(50)) answer(chance(60));
        else pendingAnswers.push_back(answer);
      };
      if (docs.numDocuments() > 6) fail("more documents than the maximum", docs.numDocuments());
      if (docs.activeDocument() && docs.indexOf(docs.activeDocument()) < 0) fail("a stale active document", 0);
    } else if (roll < 89) {
      // The toolbar's own furniture.
      const uint32_t what = pick(6);
      if (what == 0) {
        if (toolbar.isCustomisationDialogShowing()) toolbar.closeCustomisationDialog();
        else toolbar.showCustomisationDialog(factory);
      } else if (what == 1) {
        toolbar.setVertical(chance(40));
        layout();
      } else if (what == 2) {
        toolbar.setStyle(chance(33) ? Toolbar::iconsOnly : (chance(50) ? Toolbar::textOnly : Toolbar::iconsWithText));
      } else if (what == 3) {
        toolbar.addItem(factory, (int) pick(10) - 3, (int) pick(12) - 1);
      } else if (what == 4 && toolbar.numItems() > 0) {
        toolbar.removeToolbarItem((int) pick((uint32_t) toolbar.numItems()));
      } else {
        toolbar.showOverflowMenu(chance(50));
      }
    } else if (roll < 91) {
      // Dialogs and their questions.
      const uint32_t what = pick(4);
      if (what == 0) filename.browse();
      else if (what == 1) filename.showRecentFilesMenu(chance(50));
      else if (what == 2 && !chooser.isShowing()) {
        chooser.show(chance(50) ? base : joinPath(base, "notes.txt"), [](const std::string&) { ++g_reached[kDialogAnswer]; });
      } else {
        keys.startKeyCapture((int) pick(14));
      }
    } else if (roll < 93) {
      const std::vector<std::string> dropped = {joinPath(base, "Bass"), joinPath(base, "kick.wav")};
      const Point at = anywhere();
      if (chance(50)) router.fileDragMove(dropped, at);
      else router.filesDropped(dropped, at);
    } else if (roll < 96) {
      Bitmap bmp((int) width, (int) height);
      Graphics g(bmp);
      root.paintTree(g);
      ++paints;
    } else if (roll < 99) {
      // Everything that covers the editor goes away, in a random order of
      // ways -- or the run spends most of its events on whichever overlay is
      // up and never reaches what is under it.
      if (chance(50)) {
        drawer.showOrHide(false, now);
        drawer.tick(now + 1.0);
      }
      if (chance(50)) chooser.dismiss();
      if (chance(50)) folderChooser.dismiss();
      if (chance(50)) toolbar.closeCustomisationDialog();
      if (chance(50)) bar.closeMenu();
      if (chance(50)) keys.cancelKeyCapture();
      for (PopupMenuOverlay* menu : {&toolbar.overflowMenu(), &filename.recentFilesMenu(), &keys.keyMenu(), &colour.swatchMenu()})
        if (chance(50)) menu->dismiss(0);
      for (AlertOverlay* question : {&keys.questionOverlay(), &chooser.question(), &folderChooser.question()})
        if (question->isVisible() && chance(50)) {
          router.setFocus(question);
          KeyPress escape;
          escape.keyCode = KeyPress::Escape;
          router.keyPressed(escape);
        }
    } else {
      router.mouseExitWindow();
    }
  }
  if (down) router.mouseUp({-50.0f, -50.0f});
  router.mouseExitWindow();
  router.setFocus(nullptr);
  // Answers still outstanding arrive after everything else -- including for
  // documents closed some other way in the meantime.
  for (auto& answer : pendingAnswers) answer(true);

  std::printf("Sonore SDK GUI furniture fuzz -- %zu events, seed %u, %zu paints\n  reached:", steps, seed, paints);
  int unreached = 0;
  for (int r = 0; r < kReachedCount; ++r) {
    std::printf(" %s %ld%s", kReachedNames[r], g_reached[r], r + 1 < kReachedCount ? "," : "\n");
    if (g_reached[r] == 0) ++unreached;
  }
  if (unreached > 2) fail("the events reached too few of the widgets", unreached, kReachedCount);
  std::printf(g_failures == 0 ? "SONORE GUI WIDGET FUZZ PASSED\n" : "SONORE GUI WIDGET FUZZ FAILED\n");
  return g_failures == 0 ? 0 : 1;
}
