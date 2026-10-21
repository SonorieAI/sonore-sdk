// SPDX-License-Identifier: Apache-2.0
// The native editor, handled the way a user's hands handle it: at random.
//
// Every widget test here clicks where a control is and checks what it did. A
// user also drags off the edge, double-clicks a value box and types letters
// into it, scrolls over a combo, tabs while a drag is held, presses Escape in
// the middle of an edit, and resizes the window while all of that is going on.
// This builds the generic editor -- every kind of row the descriptor can ask
// for: skewed and plain sliders, stepped and named controls, switches,
// groups, a read-only meter, an on-screen keyboard -- and feeds it random
// mouse, wheel, keyboard and resize events through the same MouseRouter a
// window peer uses, painting the whole tree every few events. Properties:
//   * nothing crashes or reads out of bounds (run it under ASan for that);
//   * every value the editor sends the host is finite and inside its range;
//   * every gesture the editor ends is one it began, and once the mouse is
//     released and the keyboard put down, none is left open;
//   * a read-only parameter is never written.
//
//   fuzz_ui [--steps N] [--seed S]
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include <sonore/gfx/bitmap.h>
#include <sonore/gfx/graphics.h>
#include <sonore/gfx/plugin_editor.h>
#include <sonore/gfx/truetype.h>

using namespace sonore;
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

ParamInfo row(const char* id, const char* label, const char* unit, float lo, float hi, float def) {
  ParamInfo p;
  p.id = id;
  p.label = label;
  p.unit = unit;
  p.minValue = lo;
  p.maxValue = hi;
  p.defaultValue = def;
  return p;
}

} // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  size_t steps = 10000;
  uint32_t seed = 0x5EEDu;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--steps") == 0 && i + 1 < argc) steps = std::strtoull(argv[++i], nullptr, 10);
    if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) seed = (uint32_t) std::strtoul(argv[++i], nullptr, 10);
  }

  // Every kind of row.
  static const char* const kModes[] = {"Soft", "Hard", "Fold", "Tube"};
  std::vector<ParamInfo> params;
  {
    ParamInfo cutoff = row("cutoff", "Cutoff", "Hz", 20.0f, 20000.0f, 1000.0f);
    cutoff.skew = skewForCentre(20.0f, 20000.0f, 1000.0f);
    cutoff.group = "Filter";
    params.push_back(cutoff);
    ParamInfo res = row("res", "Resonance", "", 0.0f, 1.0f, 0.2f);
    res.group = "Filter";
    params.push_back(res);
    params.push_back(row("gain", "Gain", "dB", -60.0f, 12.0f, 0.0f));
    ParamInfo voices = row("voices", "Voices", "", 1.0f, 16.0f, 8.0f);
    voices.stepCount = 15;
    params.push_back(voices);
    ParamInfo mode = row("mode", "Mode", "", 0.0f, 3.0f, 0.0f);
    mode.stepCount = 3;
    mode.valueNames = kModes;
    mode.numValueNames = 4;
    params.push_back(mode);
    ParamInfo on = row("on", "Enabled", "", 0.0f, 1.0f, 1.0f);
    on.stepCount = 1;
    params.push_back(on);
    ParamInfo meter = row("meter", "Reduction", "dB", -30.0f, 0.0f, 0.0f);
    meter.readOnly = true;
    meter.automatable = false;
    meter.group = "Output";
    params.push_back(meter);
    ParamInfo time = row("time", "Time", "ms", 0.1f, 5000.0f, 250.0f);
    time.skew = skewForCentre(0.1f, 5000.0f, 200.0f);
    time.group = "Output";
    params.push_back(time);
  }
  const int n = (int) params.size();
  std::vector<float> values(n);
  for (int i = 0; i < n; ++i) values[i] = params[i].defaultValue;

  int failures = 0;
  auto fail = [&](const char* what, int index, double value) {
    ++failures;
    if (failures <= 8) std::printf("  FAIL %s (parameter %d, %g)\n", what, index, value);
  };
  std::vector<int> open(n, 0);
  long sets = 0, begins = 0, notes = 0;

  EditorHost host;
  host.getParameter = [&](int i) { return i >= 0 && i < n ? values[i] : 0.0f; };
  host.setParameter = [&](int i, float v) {
    ++sets;
    if (i < 0 || i >= n) return fail("a value for a parameter that does not exist", i, v);
    if (!std::isfinite(v)) return fail("a value that is not finite", i, v);
    if (v < params[i].minValue - 1e-4f || v > params[i].maxValue + 1e-4f)
      return fail("a value outside its range", i, v);
    if (params[i].readOnly) return fail("a write to a read-only parameter", i, v);
    values[i] = v;
  };
  // A gesture on a read-only parameter is as wrong as a write to one: the
  // host is told an edit began on a control it was told it cannot set.
  host.beginGesture = [&](int i) {
    ++begins;
    if (i >= 0 && i < n && params[i].readOnly) return fail("a gesture on a read-only parameter", i, 0.0);
    if (i >= 0 && i < n) ++open[i];
  };
  host.endGesture = [&](int i) {
    if (i >= 0 && i < n && params[i].readOnly) return fail("a gesture on a read-only parameter", i, 0.0);
    if (i < 0 || i >= n || open[i] <= 0) return fail("a gesture ended that never began", i, 0.0);
    --open[i];
  };
  host.noteOn = [&](int note, float velocity) {
    ++notes;
    if (note < 0 || note > 127 || !(velocity >= 0.0f && velocity <= 1.0f)) fail("a note out of range", note, velocity);
  };
  host.noteOff = [&](int note) {
    if (note < 0 || note > 127) fail("a note-off out of range", note, 0.0);
  };
  host.showContextMenu = [](int, int, int) {};

  auto face = loadAnyTypeface();
  Font font = face ? Font(face, 13.0f) : Font();
  GenericEditor editor(params.data(), n, host, font);
  Component root;
  float width = 420.0f, height = editor.preferredHeight();
  root.setBounds({0.0f, 0.0f, width, height});
  editor.setSize(width, height);
  root.addChild(&editor);
  MouseRouter router(root);

  std::mt19937 rng(seed);
  auto pick = [&](uint32_t m) { return m ? rng() % m : 0u; };
  auto chance = [&](int percent) { return (int) pick(100) < percent; };
  auto anywhere = [&]() {
    // Mostly inside, sometimes just past an edge, as a drag that leaves does.
    return Point{(float) pick((uint32_t) width + 40) - 20.0f, (float) pick((uint32_t) height + 40) - 20.0f};
  };
  bool down = false;
  const uint32_t typed[] = {'0', '1', '5', '9', '.', '-', ',', 'k', 'e', 'x', ' ', 0xE9, 0x4E2D, 'A'};
  const int named[] = {KeyPress::Backspace, KeyPress::Tab, KeyPress::Return, KeyPress::Escape,
                       KeyPress::Delete, KeyPress::Left, KeyPress::Right, KeyPress::Up,
                       KeyPress::Down, KeyPress::Home, KeyPress::End, KeyPress::PageUp,
                       KeyPress::PageDown};
  size_t paints = 0;

  for (size_t step = 0; step < steps; ++step) {
    const uint32_t roll = pick(100);
    const bool shift = chance(15), ctrl = chance(10), alt = chance(5);
    if (roll < 30) {
      router.mouseMove(anywhere(), shift, ctrl, alt);
    } else if (roll < 42) {
      if (down) router.mouseUp(anywhere(), shift, ctrl, alt);
      // Each draw in its own statement: a call's arguments are evaluated in
      // an unspecified order, and a seed has to replay under any compiler.
      const Point at = anywhere();
      const int clicks = chance(15) ? 2 : 1;
      const bool right = chance(8);
      router.mouseDown(at, clicks, shift, ctrl, alt, right);
      down = true;
    } else if (roll < 52) {
      if (down) {
        router.mouseUp(anywhere(), shift, ctrl, alt);
        down = false;
      }
    } else if (roll < 62) {
      const Point at = anywhere();
      router.mouseWheel(at, (float) ((int) pick(9) - 4) * 0.5f, shift, ctrl, alt);
    } else if (roll < 82) {
      KeyPress key;
      if (chance(55)) key.character = typed[pick(sizeof(typed) / sizeof(typed[0]))];
      else key.keyCode = named[pick(sizeof(named) / sizeof(named[0]))];
      key.shiftDown = shift;
      key.ctrlDown = ctrl;
      key.altDown = alt;
      router.keyPressed(key);
    } else if (roll < 86) {
      // The host moved something: automation, a preset, an undo.
      const int i = (int) pick((uint32_t) n);
      values[i] = params[i].minValue + (float) pick(1001) / 1000.0f * (params[i].maxValue - params[i].minValue);
      editor.sync();
    } else if (roll < 89) {
      width = 160.0f + (float) pick(900);
      height = 80.0f + (float) pick(900);
      root.setBounds({0.0f, 0.0f, width, height});
      editor.setSize(width, height);
    } else if (roll < 91) {
      router.mouseExitWindow();
    } else if (roll < 96) {
      Bitmap bmp((int) width, (int) height);
      Graphics g(bmp);
      root.paintTree(g);
      ++paints;
    } else {
      editor.sync();
    }
  }
  // Put everything down: the mouse released, the keyboard's last edit
  // finished or abandoned, the pointer gone.
  if (down) router.mouseUp({-50.0f, -50.0f});
  KeyPress escape;
  escape.keyCode = KeyPress::Escape;
  router.keyPressed(escape);
  router.mouseExitWindow();
  router.setFocus(nullptr);
  for (int i = 0; i < n; ++i)
    if (open[i] != 0) fail("a gesture left open after everything was let go", i, open[i]);

  std::printf("Sonore SDK UI fuzz -- %zu events, seed %u: %ld values sent, %ld gestures, %ld notes, "
              "%zu paints\n", steps, seed, sets, begins, notes, paints);
  if (sets == 0 || begins == 0) {
    std::printf("  FAIL the events never moved a control -- the fuzz reached nothing\n");
    return 1;
  }
  std::printf(failures == 0 ? "SONORE UI FUZZ PASSED\n" : "SONORE UI FUZZ FAILED\n");
  return failures == 0 ? 0 : 1;
}
