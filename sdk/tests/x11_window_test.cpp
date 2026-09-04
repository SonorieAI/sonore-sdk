// SPDX-License-Identifier: Apache-2.0
// The X11 window, driven against a real X server.
//
// The mirror of native_window_test.cpp, and it exists for the same reason: a
// window is the one piece of the native UI stack that cannot be checked by
// arithmetic. It needs a display, a real window, and a server actually
// delivering input.
//
// Input is synthesised with XSendEvent rather than XTEST, so this needs no
// extension and no pointer to move: the events are constructed exactly as the
// server would deliver them and posted to our own window. What is being checked
// is the SDK's translation of X events into MouseEvents, not the server's
// ability to generate them.
//
// It skips loudly rather than failing where there is no display. A machine with
// no X server is a legitimate place to build a plugin, and a test that failed
// there would fail on every headless runner and teach people to ignore it.
#include <sonore/gfx/native_editor.h>
#include <sonore/gfx/popup.h>
#include <sonore/gfx/text_editor.h>
#include <sonore/gfx/system_font.h>
#include <sonore/gfx/widgets.h>
#include <sonore/gfx/window_x11.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if !defined(SONORE_X11_HEADERS)
int main() {
  std::printf("── x11 window ──\n");
  std::printf("  ---- SKIPPED: built without <X11/Xlib.h>, so there is no peer to test ----\n");
  return 0;
}
#else

#include <X11/Xlib.h>
#include <unistd.h>

static int g_checks = 0, g_failures = 0;
static void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}

using namespace sonore::gfx;

/** A pointer event, constructed as the server would and posted to `window`.
 *
 *  `sendEvent` is set on anything XSendEvent delivers, and a real toolkit is
 *  entitled to ignore such events for security. This SDK does not filter on it
 *  -- a plugin is not a login screen -- which is what makes this test possible
 *  at all, and is worth knowing rather than discovering. */
static void postButton(::Display* d, Window w, int type, unsigned button, int x, int y,
                       unsigned state, Time when) {
  XEvent e{};
  e.type = type;
  e.xbutton.display = d;
  e.xbutton.window = w;
  e.xbutton.root = DefaultRootWindow(d);
  // 0 rather than X11's None: window_x11.h puts that macro away when it is
  // finished with it, precisely so the rest of a plugin can have identifiers
  // called None. See the note at the bottom of that header.
  e.xbutton.subwindow = 0;
  e.xbutton.time = when;
  e.xbutton.x = x;
  e.xbutton.y = y;
  e.xbutton.x_root = x;
  e.xbutton.y_root = y;
  e.xbutton.state = state;
  e.xbutton.button = button;
  e.xbutton.same_screen = 1;
  e.xbutton.send_event = 1;
  XSendEvent(d, w, 0, type == ButtonPress ? ButtonPressMask : ButtonReleaseMask, &e);
}

static void postMotion(::Display* d, Window w, int x, int y, unsigned state, Time when) {
  XEvent e{};
  e.type = MotionNotify;
  e.xmotion.display = d;
  e.xmotion.window = w;
  e.xmotion.root = DefaultRootWindow(d);
  e.xmotion.subwindow = 0;
  e.xmotion.time = when;
  e.xmotion.x = x;
  e.xmotion.y = y;
  e.xmotion.x_root = x;
  e.xmotion.y_root = y;
  e.xmotion.state = state;
  e.xmotion.is_hint = 0; // NotifyNormal
  e.xmotion.same_screen = 1;
  e.xmotion.send_event = 1;
  XSendEvent(d, w, 0, PointerMotionMask, &e);
}

int main() {
  std::printf("── x11 window ──\n");

  if (!x11::xlib().ok) {
    std::printf("  ---- SKIPPED: libX11.so.6 is not on this machine ----\n");
    return 0;
  }
  // The peer opens its own connection; this one is only for posting events and
  // for asking whether there is a server at all.
  ::Display* probe = XOpenDisplay(nullptr);
  if (!probe) {
    std::printf("  ---- SKIPPED: no X display (DISPLAY=%s) ----\n",
                std::getenv("DISPLAY") ? std::getenv("DISPLAY") : "unset");
    return 0;
  }
  XCloseDisplay(probe);

  char msg[240];

  // ── The peer, with a component tree in it ────────────────────────────────
  Component page;
  Slider knob(Slider::Style::Rotary);
  knob.setBounds({20.0f, 20.0f, 80.0f, 80.0f});
  knob.setValue(0.5f, false);
  page.addChild(&knob);

  Button button("Bypass");
  button.setBounds({120.0f, 20.0f, 80.0f, 28.0f});
  button.setToggleable(true);
  page.addChild(&button);

  int clicks = 0, gestures = 0, doubles = 0;
  button.onClick = [&]() { ++clicks; };
  knob.onDragStart = [&]() { ++gestures; };

  NativeWindowX11 window;
  check(window.open(nullptr, page, 240, 140), "a window opens on the X server");
  if (!window.isOpen()) {
    std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
    return 1;
  }

  const Window wid = (Window) (uintptr_t) window.handle();
  check(wid != 0, "and has an X window id a host could parent");
  std::snprintf(msg, sizeof(msg), "its connection is file descriptor %d, which a host can poll",
                window.connectionFd());
  check(window.connectionFd() >= 0, msg);

  // A second connection for posting. The peer must not be sharing ours.
  ::Display* poster = XOpenDisplay(nullptr);
  check(poster != nullptr, "the test opened its own connection");
  if (!poster) return 1;

  auto flushAndPump = [&]() {
    XFlush(poster);
    // Several rounds: XSendEvent goes through the server, so there is a real
    // round trip between posting and the peer being able to read it.
    for (int i = 0; i < 40; ++i) {
      window.processEvents();
      usleep(2000);
    }
  };

  // ── It painted ───────────────────────────────────────────────────────────
  int distinct = 0;
  {
    bool seen[64] = {false};
    for (int y = 0; y < window.bitmap().height(); y += 3)
      for (int x = 0; x < window.bitmap().width(); x += 3) {
        const PremulColour c = window.bitmap().pixelAt(x, y);
        const int bucket = ((c.r >> 6) << 4) | ((c.g >> 6) << 2) | (c.b >> 6);
        if (!seen[bucket]) {
          seen[bucket] = true;
          ++distinct;
        }
      }
  }
  std::snprintf(msg, sizeof(msg), "the tree painted into the window (%d distinct colours)",
                distinct);
  check(distinct >= 4, msg);

  // ── A click on the button ────────────────────────────────────────────────
  Time when = 10000;
  postButton(poster, wid, ButtonPress, Button1, 150, 34, 0, when);
  postButton(poster, wid, ButtonRelease, Button1, 150, 34, Button1Mask, when + 20);
  flushAndPump();
  std::snprintf(msg, sizeof(msg), "a press/release pair over the button clicked it (%d)", clicks);
  check(clicks == 1, msg);
  check(button.isToggled(), "and a toggling button is now on");

  // ── A drag on the knob, ending OUTSIDE the window ────────────────────────
  //
  // X gives the pressed window an implicit passive grab until the button is
  // released, so motion past the edge still arrives. That is what SetCapture
  // buys on Windows, and the reason neither peer needs an explicit grab.
  knob.setValue(0.5f, false);
  gestures = 0;
  when += 1000;
  postButton(poster, wid, ButtonPress, Button1, 60, 60, 0, when);
  for (int i = 1; i <= 10; ++i)
    postMotion(poster, wid, 60, 60 - i * 12, Button1Mask, when + (Time) i);
  postButton(poster, wid, ButtonRelease, Button1, 60, -60, Button1Mask, when + 40);
  flushAndPump();
  std::snprintf(msg, sizeof(msg), "dragging up from 0.50 to %.2f, ending outside the window",
                knob.value());
  check(knob.value() > 0.9f, msg);
  check(gestures == 1, "as exactly one gesture");

  // ── Double click ─────────────────────────────────────────────────────────
  //
  // X has no such thing: the server reports two presses and every toolkit
  // decides for itself. A knob that would not return to its default on a
  // double click is a control users quietly stop trusting.
  knob.setDefaultValue(0.25f);
  knob.setValue(0.9f, false);
  when += 1000;
  postButton(poster, wid, ButtonPress, Button1, 60, 60, 0, when);
  postButton(poster, wid, ButtonRelease, Button1, 60, 60, Button1Mask, when + 5);
  postButton(poster, wid, ButtonPress, Button1, 60, 60, 0, when + 60);
  postButton(poster, wid, ButtonRelease, Button1, 60, 60, Button1Mask, when + 65);
  flushAndPump();
  std::snprintf(msg, sizeof(msg), "two presses 60 ms apart return the knob to its default (%.2f)",
                knob.value());
  check(std::fabs(knob.value() - 0.25f) < 1e-3f, msg);

  // And two presses far apart are NOT a double click, which is the half that
  // a fixed window gets wrong in the other direction.
  knob.setValue(0.9f, false);
  when += 5000;
  postButton(poster, wid, ButtonPress, Button1, 60, 60, 0, when);
  postButton(poster, wid, ButtonRelease, Button1, 60, 60, Button1Mask, when + 5);
  postButton(poster, wid, ButtonPress, Button1, 60, 60, 0, when + 900);
  postButton(poster, wid, ButtonRelease, Button1, 60, 60, Button1Mask, when + 905);
  flushAndPump();
  std::snprintf(msg, sizeof(msg), "and 900 ms apart is two clicks, not one double (%.2f)",
                knob.value());
  check(std::fabs(knob.value() - 0.25f) > 1e-3f, msg);
  (void) doubles;

  // ── The wheel, which X delivers as buttons 4 and 5 ───────────────────────
  knob.setValue(0.5f, false);
  const float beforeWheel = knob.value();
  when += 1000;
  postButton(poster, wid, ButtonPress, Button4, 60, 60, 0, when);
  flushAndPump();
  std::snprintf(msg, sizeof(msg), "a Button4 press over the knob moves it from %.2f to %.2f",
                beforeWheel, knob.value());
  check(knob.value() > beforeWheel, msg);

  const float beforeDown = knob.value();
  when += 100;
  postButton(poster, wid, ButtonPress, Button5, 60, 60, 0, when);
  flushAndPump();
  check(knob.value() < beforeDown, "and Button5 moves it back down");

  // ── Resizing ─────────────────────────────────────────────────────────────
  window.setSize(320, 200);
  flushAndPump();
  std::snprintf(msg, sizeof(msg), "resizing gives a %dx%d buffer", window.bitmap().width(),
                window.bitmap().height());
  check(window.bitmap().width() == 320 && window.bitmap().height() == 200, msg);
  check(page.bounds().w == 320.0f, "and the content is resized with it");

  // ── HiDPI ────────────────────────────────────────────────────────────────
  //
  // Forced rather than waited for. This server reports 96 DPI and no Xft.dpi,
  // so nothing here would take the scaled path on its own -- and a HiDPI bug
  // that only appears on somebody else's laptop is the kind this test exists to
  // stop. setScale is the same call the LV2 UI extension's ui:scaleFactor makes.
  window.setScale(2.0f);
  window.setLogicalSize(320.0f, 200.0f);
  flushAndPump();
  std::snprintf(msg, sizeof(msg), "at 200%% a 320x200 editor asks the server for a %dx%d buffer",
                window.bitmap().width(), window.bitmap().height());
  check(window.bitmap().width() == 640 && window.bitmap().height() == 400, msg);
  std::snprintf(msg, sizeof(msg), "and the component tree is still laid out at %gx%g",
                page.bounds().w, page.bounds().h);
  check(page.bounds().w == 320.0f && page.bounds().h == 200.0f, msg);

  // The bounding box of everything drawn, at each scale. Geometry rather than a
  // count of lit pixels: a one-unit stroke is one device pixel at 100% and two
  // at 200%, and antialiased edges cross any brightness threshold differently
  // at the two resolutions.
  {
    auto drawnExtent = [&](int* w, int* h) {
      int minx = 1 << 30, miny = 1 << 30, maxx = -1, maxy = -1;
      const Bitmap& bm = window.bitmap();
      const PremulColour ground = bm.pixelAt(1, 1);
      for (int y = 0; y < bm.height(); ++y)
        for (int x = 0; x < bm.width(); ++x) {
          const PremulColour px = bm.pixelAt(x, y);
          if (px.r == ground.r && px.g == ground.g && px.b == ground.b) continue;
          if (x < minx) minx = x;
          if (y < miny) miny = y;
          if (x > maxx) maxx = x;
          if (y > maxy) maxy = y;
        }
      *w = maxx - minx + 1;
      *h = maxy - miny + 1;
    };
    int wTwo = 0, hTwo = 0, wOne = 0, hOne = 0;
    drawnExtent(&wTwo, &hTwo);
    window.setScale(1.0f);
    window.setLogicalSize(320.0f, 200.0f);
    flushAndPump();
    drawnExtent(&wOne, &hOne);
    std::snprintf(msg, sizeof(msg), "everything drawn spans %dx%d device pixels at 100%% and "
                  "%dx%d at 200%%", wOne, hOne, wTwo, hTwo);
    // Within 3%: the two images are rasterised INDEPENDENTLY at different
    // resolutions, so the outermost antialiased column lands differently.
    const bool wOk = wOne > 0 && std::fabs((double) wTwo / (wOne * 2.0) - 1.0) < 0.03;
    const bool hOk = hOne > 0 && std::fabs((double) hTwo / (hOne * 2.0) - 1.0) < 0.03;
    check(wOk && hOk, msg);
  }

  // And the pointer still lands on the control. X reports DEVICE pixels, so at
  // 200% an unconverted position is twice the coordinate the knob was drawn at.
  {
    window.setScale(2.0f);
    window.setLogicalSize(320.0f, 200.0f);
    flushAndPump();
    const float was = knob.value();
    const Point centre{knob.bounds().x + knob.bounds().w * 0.5f,
                       knob.bounds().y + knob.bounds().h * 0.5f};
    const int dx = (int) (centre.x * 2.0f), dy = (int) (centre.y * 2.0f);
    when += 1000;
    postButton(poster, wid, ButtonPress, Button1, dx, dy, 0, when);
    for (int i = 1; i <= 8; ++i)
      postMotion(poster, wid, dx, dy - i * 10, Button1Mask, when + (Time) i);
    postButton(poster, wid, ButtonRelease, Button1, dx, dy - 80, Button1Mask, when + 40);
    flushAndPump();
    std::snprintf(msg, sizeof(msg), "a drag at device (%d,%d) reaches the knob at logical "
                  "(%.0f,%.0f) and moved it from %.2f to %.2f", dx, dy, centre.x,
                  centre.y, was, knob.value());
    check(knob.value() != was, msg);
    window.setScale(1.0f);
    window.setLogicalSize(320.0f, 200.0f);
    flushAndPump();
  }

  // ── The whole editor, as a format wrapper opens it ───────────────────────
  {
    static const char* kShapes[] = {"Sine", "Saw", "Square"};
    static const sonore::ParamInfo kParams[3] = {
        {"gain", "Gain", "dB", -60.0f, 6.0f, 0.0f, 0},
        {"freq", "Freq", "Hz", 20.0f, 20000.0f, 440.0f, 0},
        {"shape", "Shape", "", 0.0f, 2.0f, 0.0f, 3, nullptr, kShapes, 3},
    };
    float values[3] = {0.0f, 440.0f, 0.0f};
    int sets = 0, begins = 0, ends = 0;

    EditorHost host;
    host.getParameter = [&](int i) { return values[i]; };
    host.setParameter = [&](int i, float v) { values[i] = v; ++sets; };
    host.beginGesture = [&](int) { ++begins; };
    host.endGesture = [&](int) { ++ends; };

    NativeEditor editor;
    check(NativeEditor::isAvailable(), "the native editor reports itself available on X11");
    check(editor.open(nullptr, kParams, 3, host, 380, 160), "and opens");

    if (editor.isOpen()) {
      std::printf("  ---- text is %s ----\n",
                  editor.hasText() ? "being drawn" : "ABSENT: no system typeface found");
      const Window ewid = (Window) (uintptr_t) editor.handle();

      // Coordinates from the same constants GenericEditor::resized() uses.
      const int rowY = (int) (GenericEditor::kPadding + GenericEditor::kRowHeight * 0.5f);
      const float trackX = GenericEditor::kPadding + GenericEditor::kLabelWidth + 8.0f;
      // From the EDITOR's width, not the window's. They stopped being the same
      // when the editor moved inside a Viewport, which takes a scroll bar's
      // width off it -- and this went on saying 380 and clicking past the end
      // of the track. It still passed, because the check only asks whether the
      // value went up, which is the kind of pass that hides a stale coordinate.
      const float editorW = editor.content() ? editor.content()->bounds().w : 380.0f;
      const float trackW =
          editorW - trackX - GenericEditor::kValueWidth - GenericEditor::kPadding * 2.0f;

      auto pumpEditor = [&]() {
        XFlush(poster);
        for (int i = 0; i < 40; ++i) {
          editor.pumpEvents();
          usleep(2000);
        }
      };

      when += 1000;
      postButton(poster, ewid, ButtonPress, Button1, (int) (trackX + trackW * 0.1f), rowY, 0, when);
      // 0.75, not 0.95: the track is inset by half a knob at each end, so 0.95
      // of the BOUNDS pins the value at its maximum -- and a value stuck at the
      // clamp would read the same whether the drag worked or not.
      postMotion(poster, ewid, (int) (trackX + trackW * 0.75f), rowY, Button1Mask, when + 10);
      postButton(poster, ewid, ButtonRelease, Button1, (int) (trackX + trackW * 0.75f), rowY,
                 Button1Mask, when + 20);
      pumpEditor();
      // Checked against the value the track position IMPLIES, not against a
      // direction. "It went up" was the old assertion, and it only held because
      // of where the track happened to be; the geometry moved twice under it and
      // it kept passing anyway. This verifies the whole chain -- event, router,
      // widget, attachment, host -- lands on the number the pixel says.
      //
      // Slider insets the track by half a knob (7px) at each end, so a click x
      // maps to (x - 7) / (w - 14).
      const float clickAt = trackW * 0.75f;
      const float expectedNorm = (clickAt - 7.0f) / (trackW - 14.0f);
      const float expectedDb = -60.0f + expectedNorm * 66.0f;
      std::snprintf(msg, sizeof(msg),
                    "a drag to 75%% of the track gave %.1f dB against the %.1f the geometry "
                    "implies, as %d begin/%d end", values[0], expectedDb, begins, ends);
      check(std::fabs(values[0] - expectedDb) < 0.5f && begins == 1 && ends == 1, msg);
      check(values[1] == 440.0f, "and left every other parameter where it was");

      // The clock. On X11 it rides on processEvents rather than on a WM_TIMER,
      // and it has the same job: bring the host's value back into the widget
      // when nothing has touched the window.
      values[0] = -48.0f;
      const int setsBefore = sets;
      pumpEditor();
      const float shown = editor.content() ? editor.content()->parameterValueShown(0) : 0.0f;
      std::snprintf(msg, sizeof(msg), "the clock pulled -48 dB back into the control (showing %.1f)",
                    shown);
      check(std::fabs(shown - (-48.0f)) < 0.01f, msg);
      check(sets == setsBefore, "and syncing set NOTHING back -- the feedback loop stays open");
    }

    editor.close();
    check(!editor.isOpen(), "the native editor closes");
    editor.close();
    check(!editor.isOpen(), "and closing twice is harmless");
  }

  // ── Closing ──────────────────────────────────────────────────────────────
  window.close();
  check(!window.isOpen(), "closing releases the window");
  check(window.connectionFd() < 0, "and the connection is gone with it");

  // -- A real popup menu -----------------------------------------------------
  //
  // The X half of what native_window_test checks on Windows. Two things here
  // are X-specific and both are the difference between a menu and a rectangle:
  // override_redirect, which tells the window manager to keep its hands off, and
  // an active pointer grab, which is what makes a press outside the menu arrive
  // at the menu at all.
  std::printf("\n-- popup window --\n");
  {
    auto face = systemTypeface();
    // QUALIFIED. Xlib typedefs a global `Font` (an XID for a server-side font
    // from before client-side rendering existed), so under
    // `using namespace sonore::gfx` the bare name is ambiguous. Nothing can be
    // undefined here -- it is a typedef, not a macro -- so any file that uses
    // both spells ours out. See the note at the top of window_x11.h.
    sonore::gfx::Font popupFont = face ? sonore::gfx::Font(face, 13.0f) : sonore::gfx::Font();

    PopupMenu menu;
    menu.addItem(1, "Sine", true, true);
    menu.addItem(2, "Saw");
    menu.addItem(3, "Square");

    int chosen = -999, calls = 0;
    PopupWindow popup;
    check(popup.show(menu, popupFont, 200.0f, 200.0f, 24.0f, 1920.0f, 1080.0f,
                     [&](int id) {
                       chosen = id;
                       ++calls;
                     }),
          "a popup menu opens as a real X window");

    if (popup.isOpen()) {
      const Window pwid = (Window) (uintptr_t) popup.handle();
      check(pwid != 0, "with a window of its own");

      // override_redirect is what every menu on every X desktop sets. Without
      // it a tiling window manager puts the plugin's menu in its own tile.
      XWindowAttributes attrs{};
      XGetWindowAttributes(poster, pwid, &attrs);
      check(attrs.override_redirect != 0,
            "override-redirect, so no window manager reframes or retiles it");

      auto pumpPopup = [&]() {
        XFlush(poster);
        for (int i = 0; i < 40 && popup.isOpen(); ++i) {
          popup.tick();
          usleep(2000);
        }
      };

      Time t = 90000;
      const int itemY = (int) (PopupMenu::kPaddingY + PopupMenu::kItemHeight * 1.5f);
      postButton(poster, pwid, ButtonPress, Button1, 30, itemY, 0, t);
      pumpPopup();
      std::snprintf(msg, sizeof(msg), "pressing the second item returns id %d, once (%d)", chosen,
                    calls);
      check(chosen == 2 && calls == 1, msg);
      check(!popup.isOpen(), "and the tick that followed closed the window");
    }

    // -- Dismissed from outside --
    //
    // Negative coordinates are exactly what a press outside looks like to a
    // window holding an owner_events-false grab.
    chosen = -999;
    calls = 0;
    check(popup.show(menu, popupFont, 200.0f, 200.0f, 24.0f, 1920.0f, 1080.0f,
                     [&](int id) {
                       chosen = id;
                       ++calls;
                     }),
          "a second menu opens");
    if (popup.isOpen()) {
      const Window pwid = (Window) (uintptr_t) popup.handle();
      Time t = 95000;
      postButton(poster, pwid, ButtonPress, Button1, -50, -50, 0, t);
      XFlush(poster);
      for (int i = 0; i < 40 && popup.isOpen(); ++i) {
        popup.tick();
        usleep(2000);
      }
      std::snprintf(msg, sizeof(msg), "a press outside dismisses with %d, not a choice", chosen);
      check(chosen == 0 && calls == 1, msg);
      check(!popup.isOpen(), "and it closes");
    }

    popup.close();
    check(!popup.isOpen(), "closing an already-closed popup is harmless");

    // The grab really is gone. A grab that outlived its window would leave the
    // whole desktop unable to click on anything until this process exits, which
    // is the single worst thing a plugin can do to somebody's session -- and it
    // would not show up as a failure anywhere else.
    Window probeWin = XCreateSimpleWindow(poster, DefaultRootWindow(poster), 0, 0, 10, 10, 0, 0, 0);
    // MAPPED, and waited for. XGrabPointer refuses a window that is not
    // viewable and answers GrabNotViewable, which is not the answer this is
    // asking about -- the first version of this check read that 3 as a failure
    // and blamed correct code.
    XMapWindow(poster, probeWin);
    XSync(poster, 0);
    int grabResult = XGrabPointer(poster, probeWin, 0, ButtonPressMask, GrabModeAsync,
                                  GrabModeAsync, 0, 0, CurrentTime);
    for (int i = 0; i < 20 && grabResult == GrabNotViewable; ++i) {
      usleep(5000);
      XSync(poster, 0);
      grabResult = XGrabPointer(poster, probeWin, 0, ButtonPressMask, GrabModeAsync, GrabModeAsync,
                                0, 0, CurrentTime);
    }
    // AlreadyGrabbed is the one answer that would mean the popup's grab
    // outlived it. Anything else -- success, or a server that will not grab for
    // an unrelated reason -- means the pointer is not ours any more.
    std::snprintf(msg, sizeof(msg), "the pointer grab was released (XGrabPointer returns %d)",
                  grabResult);
    check(grabResult != AlreadyGrabbed, msg);
    XUngrabPointer(poster, CurrentTime);
    XDestroyWindow(poster, probeWin);
    XFlush(poster);

    PopupMenu nothing;
    check(!popup.show(nothing, popupFont, 0.0f, 0.0f, 0.0f, 1920.0f, 1080.0f, [](int) {}),
          "an empty menu refuses to open rather than showing an empty box");
  }

  // -- Keys, through a real server -------------------------------------------
  //
  // Synthesised from a KEYSYM rather than a hard-coded keycode: a keycode is
  // whatever the running server assigned to a physical key and differs between
  // machines, so XKeysymToKeycode is the only way to post "the R key" and mean
  // it. The peer then runs XLookupString on the result exactly as it would for
  // a real press, which is the point -- what is being checked is the SDK's
  // translation, and it only translates what the layout hands it.
  std::printf("\n-- keyboard --\n");
  {
    auto face = systemTypeface();
    sonore::gfx::Font keyFont = face ? sonore::gfx::Font(face, 13.0f) : sonore::gfx::Font();

    Component page2;
    page2.setBounds({0.0f, 0.0f, 260.0f, 60.0f});
    TextEditor field;
    field.setFont(keyFont);
    field.setBounds({10.0f, 10.0f, 240.0f, 26.0f});
    page2.addChild(&field);

    NativeWindowX11 keyWindow;
    check(keyWindow.open(nullptr, page2, 260, 60), "a window with a text field opens");
    if (keyWindow.isOpen()) {
      const Window kwid = (Window) (uintptr_t) keyWindow.handle();
      keyWindow.router()->setFocus(&field);
      check(field.hasKeyboardFocus(), "and the field has focus");

      Time kt = 200000;
      auto postKey = [&](KeySym sym, unsigned state) {
        XEvent e{};
        e.type = x11::kKeyPressEvent;
        e.xkey.display = poster;
        e.xkey.window = kwid;
        e.xkey.root = DefaultRootWindow(poster);
        e.xkey.subwindow = 0;
        e.xkey.time = (kt += 10);
        e.xkey.x = 20;
        e.xkey.y = 20;
        e.xkey.x_root = 20;
        e.xkey.y_root = 20;
        e.xkey.state = state;
        e.xkey.keycode = XKeysymToKeycode(poster, sym);
        e.xkey.same_screen = 1;
        e.xkey.send_event = 1;
        XSendEvent(poster, kwid, 0, KeyPressMask, &e);
      };
      auto pumpKeys = [&]() {
        XFlush(poster);
        for (int i = 0; i < 40; ++i) {
          keyWindow.processEvents();
          usleep(2000);
        }
      };

      postKey(0x048, ShiftMask); // XK_H
      postKey(0x069, 0);         // XK_i
      pumpKeys();
      std::snprintf(msg, sizeof(msg), "two key events typed \"%s\"", field.getText().c_str());
      check(field.getText() == "Hi", msg);

      postKey(0xff08, 0); // XK_BackSpace
      pumpKeys();
      check(field.getText() == "H", "XK_BackSpace deletes one");

      postKey(0xff50, 0); // XK_Home
      postKey(0x04f, ShiftMask);
      pumpKeys();
      std::snprintf(msg, sizeof(msg), "XK_Home then O gives \"%s\"", field.getText().c_str());
      check(field.getText() == "OH", msg);

      // Return produces "\r" from XLookupString as well as a keysym. Letting
      // that through as a CHARACTER would type a carriage return into the
      // field on top of handling the key -- which is why the peer only reads
      // text when the key is not a named one.
      int returns = 0;
      field.onReturn = [&](const std::string&) { ++returns; };
      const std::string before = field.getText();
      postKey(0xff0d, 0); // XK_Return
      pumpKeys();
      std::snprintf(msg, sizeof(msg), "XK_Return fired %d return(s) and left the text as \"%s\"",
                    returns, field.getText().c_str());
      check(returns == 1 && field.getText() == before, msg);

      // ── Ctrl+A, which did nothing at all until this test existed ──
      //
      // XLookupString APPLIES Ctrl: Ctrl+A comes back as 0x01, not 'a'. So the
      // peer delivered a control code, TextEditor compared it against 'a' and
      // found no match, and select-all, copy, cut and paste by keyboard did
      // nothing in any plugin built with this SDK.
      //
      // Every unit test passed throughout, because they synthesise a KeyPress
      // with character 'a' and ctrlDown -- something the peer could not
      // produce. The bug lived in the gap between the two layers, which is the
      // only place this test can reach.
      field.setText("hello world");
      pumpKeys();
      postKey(0x061, ControlMask); // Ctrl+XK_a
      pumpKeys();
      std::snprintf(msg, sizeof(msg), "Ctrl+A selected %d of %d characters",
                    field.selectionEnd() - field.selectionStart(),
                    (int) field.getText().size());
      check(field.selectionEnd() - field.selectionStart() == 11, msg);

      // And an ordinary letter still types, which is the half a careless fix
      // breaks: taking the character from the keysym for EVERYTHING would give
      // an American keyboard to everybody with a layout.
      field.setText("");
      pumpKeys();
      postKey(0x07a, 0); // XK_z
      pumpKeys();
      std::snprintf(msg, sizeof(msg), "and an unmodified letter still types (\"%s\")",
                    field.getText().c_str());
      check(field.getText() == "z", msg);
    }
    keyWindow.close();
    page2.removeChild(&field);
  }

  // -- text/uri-list, the format XDND actually delivers --------------------
  //
  // This is where a file drop breaks silently. Every desktop sends percent-
  // encoded file:// URIs, so a folder called "My Samples" arrives as
  // My%20Samples -- and a peer that did not decode it would hand the plugin a
  // path that does not exist, on the machines of exactly the users who name
  // folders with spaces in them.
  //
  // Reached through a friend so the parser can stay private: it is an
  // implementation detail of the protocol, not part of the peer's interface.
  std::printf("\n-- uri list --\n");
  {
    auto parse = [](const char* text) {
      return NativeWindowX11::parseUriListForTest(text, std::strlen(text));
    };

    {
      auto files = parse("file:///home/user/kick.wav\r\n");
      std::snprintf(msg, sizeof(msg), "one uri gives %d path(s): \"%s\"", (int) files.size(),
                    files.empty() ? "" : files[0].c_str());
      check(files.size() == 1 && files[0] == "/home/user/kick.wav", msg);
    }

    {
      auto files = parse("file:///home/user/My%20Samples/snare%20hit.wav\r\n");
      std::snprintf(msg, sizeof(msg), "percent-encoding is decoded: \"%s\"",
                    files.empty() ? "" : files[0].c_str());
      check(files.size() == 1 && files[0] == "/home/user/My Samples/snare hit.wav", msg);
    }

    {
      auto files = parse("file:///a.wav\r\nfile:///b.wav\r\nfile:///c.wav\r\n");
      std::snprintf(msg, sizeof(msg), "three uris give %d paths", (int) files.size());
      check(files.size() == 3 && files[2] == "/c.wav", msg);
    }

    {
      // RFC 2483 says lines starting with # are comments, and some sources
      // really do send one.
      auto files = parse("# comment\r\nfile:///a.wav\r\n");
      check(files.size() == 1 && files[0] == "/a.wav", "a comment line is skipped");
    }

    {
      // file://localhost/... is one path, not a host and a path. Some sources
      // send it and a peer that kept the hostname produces "localhost/home/..."
      auto files = parse("file://localhost/home/user/a.wav\r\n");
      std::snprintf(msg, sizeof(msg), "a localhost uri gives \"%s\"",
                    files.empty() ? "" : files[0].c_str());
      check(files.size() == 1 && files[0] == "/home/user/a.wav", msg);
    }

    {
      // Bare LF rather than CRLF: not what the RFC says, and what several
      // desktops send anyway.
      auto files = parse("file:///a.wav\nfile:///b.wav\n");
      check(files.size() == 2, "bare LF separators are accepted, because desktops send them");
    }

    {
      auto files = parse("");
      check(files.empty(), "an empty list gives no paths rather than one empty one");
      auto blank = parse("\r\n\r\n");
      check(blank.empty(), "and neither do blank lines");
    }

    {
      // A truncated escape at the very end must not read past the buffer.
      auto files = parse("file:///a%2");
      std::snprintf(msg, sizeof(msg), "a truncated escape is left alone: \"%s\"",
                    files.empty() ? "" : files[0].c_str());
      check(files.size() == 1 && files[0] == "/a%2", msg);
      auto bad = parse("file:///a%zz.wav");
      check(bad.size() == 1 && bad[0] == "/a%zz.wav", "and a non-hex escape is left alone too");
    }
  }

  XCloseDisplay(poster);
  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  if (g_failures == 0) std::printf("SONORE X11 WINDOW TEST PASSED\n");
  return g_failures == 0 ? 0 : 1;
}

#endif // SONORE_X11_HEADERS
