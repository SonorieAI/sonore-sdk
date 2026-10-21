// SPDX-License-Identifier: Apache-2.0
// What the X11 peer's input method integration does on THIS machine.
//
// Written on a machine with no X server and no input method, where it has
// only ever been compiled (against the real Xlib.h). This is the program a
// Linux developer runs to find out whether it works:
//
//   x11_input_selftest            checks, then 20 seconds to type into the field
//   x11_input_selftest 0          checks only (CI, Xvfb)
//   x11_input_selftest 60         a minute to type
//
// with an input method running (ibus, fcitx) and XMODIFIERS naming it, e.g.
// XMODIFIERS=@im=ibus. SONORE_XIM_STYLE=callbacks|position|nothing forces a
// style. It prints what the peer negotiated, where it told the input method
// the caret is, and every composition and commit as they happen -- compose
// "nihon", convert, commit, and the lines say what the field saw.
//
// It does not set the locale for you. A plugin cannot (the host owns it), so
// neither does this: run it with the locale your host would have, and the
// status line says what that locale means for composing.
//
// Exit code: non-zero when a check fails. "No input method" is NOT a failure
// -- it is reported -- because CI has none; a negotiated input method that
// then misplaces the caret is.
#if !defined(__linux__)
#error "x11_input_selftest is for Linux with the X11 headers"
#endif

#include <sonore/gfx/text_editor.h>
#include <sonore/gfx/window_x11.h>
#include <sonore/gfx/system_font.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#if !defined(SONORE_X11_HEADERS)
#error "the X11 headers are needed: install libx11-dev"
#endif

using sonore::gfx::Component;
using sonore::gfx::NativeWindowX11;
using sonore::gfx::Rect;
using sonore::gfx::TextEditor;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
  std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++failures;
}

std::string described(const TextEditor& field) {
  const sonore::gfx::TextComposition& c = field.composition();
  std::string out = "text \"" + field.getText() + "\"";
  if (c.isActive()) {
    out += ", composing \"" +
           sonore::gfx::detail::textInputEncode(c.text.data(), c.text.data() + c.text.size()) +
           "\" caret " + std::to_string(c.caret);
    if (c.clauseEnd > c.clauseStart)
      out += " clause " + std::to_string(c.clauseStart) + ".." + std::to_string(c.clauseEnd);
  }
  return out;
}

} // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  const int seconds = argc > 1 ? std::atoi(argv[1]) : 20;
  std::printf("X11 INPUT METHOD SELF-TEST\n");

  if (!sonore::gfx::x11::xlib().ok) {
    std::printf("  no libX11.so.6: nothing to test\n");
    return 0;
  }
  std::printf("  libX11 XIM entry points: %s\n",
              sonore::gfx::x11::xlib().hasInputMethods() ? "all present" : "MISSING");

  auto face = sonore::gfx::systemTypeface();
  const sonore::gfx::Font font = face ? sonore::gfx::Font(face, 15.0f) : sonore::gfx::Font();

  Component page;
  page.setBounds({0.0f, 0.0f, 420.0f, 80.0f});
  TextEditor field;
  field.setFont(font);
  field.setPlaceholder("type here with your input method");
  field.setBounds({20.0f, 24.0f, 380.0f, 32.0f});
  page.addChild(&field);

  NativeWindowX11 window;
  if (!window.open(nullptr, page, 420, 80)) {
    std::printf("  no X display (DISPLAY=%s): nothing to test\n",
                std::getenv("DISPLAY") ? std::getenv("DISPLAY") : "(unset)");
    return 0;
  }
  check(window.inputMethodStatus().empty(), "no input method is opened before a text field has the focus");

  window.router()->setFocus(&field);
  window.processEvents();
  std::printf("  status: %s\n", window.inputMethodStatus().c_str());
  check(!window.inputMethodStatus().empty(), "focusing a text field opens (or reports on) the input method");

  if (window.inputMethodActive()) {
    std::printf("  preedit style bit: 0x%lx\n", window.inputMethodPreeditStyle());
    int sx = 0, sy = 0;
    if (window.inputMethodSpot(&sx, &sy)) {
      // The spot must be the caret's bottom-left in device pixels -- the
      // failure this guards against is (0,0), a point nobody converted.
      Rect caret;
      sonore::gfx::focusedCaretInRoot(window.router(), &caret);
      const float scale = window.scale();
      const int ex = (int) std::lround(caret.x * scale), ey = (int) std::lround((caret.y + caret.h) * scale);
      char msg[160];
      std::snprintf(msg, sizeof(msg), "spot location (%d, %d) is the caret's bottom-left (%d, %d)", sx, sy, ex, ey);
      check(sx == ex && sy == ey && sx > 0 && sy > 0, msg);
    } else {
      std::printf("  this style takes no spot location\n");
    }
  } else {
    std::printf("  no input method: keys are read with XLookupString as before\n");
  }

  if (seconds <= 0) {
    window.close();
    std::printf(failures ? "X11 INPUT METHOD SELF-TEST FAILED\n" : "X11 INPUT METHOD SELF-TEST PASSED\n");
    return failures ? 1 : 0;
  }

  std::printf("\n  %d seconds: click the window and type with your input method.\n", seconds);
  std::string last = described(field);
  std::printf("  %s\n", last.c_str());
  field.onTextChange = [&](const std::string& text) { std::printf("  commit -> \"%s\"\n", text.c_str()); };
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  while (std::chrono::steady_clock::now() < end && window.isOpen()) {
    window.processEvents();
    const std::string now = described(field);
    if (now != last) {
      std::printf("  %s\n", now.c_str());
      last = now;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  std::printf("  final status: %s\n", window.inputMethodStatus().c_str());
  window.close();
  page.removeChild(&field);
  std::printf(failures ? "X11 INPUT METHOD SELF-TEST FAILED\n" : "X11 INPUT METHOD SELF-TEST PASSED\n");
  return failures ? 1 : 0;
}
