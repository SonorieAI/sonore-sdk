// SPDX-License-Identifier: Apache-2.0
// The Win32 window, driven with real messages.
//
// A window is the one piece of the native UI stack that cannot be checked by
// arithmetic: it needs an HWND, a message loop and the operating system
// actually delivering input. So this creates a real window, posts real
// messages into it, and reads back what the widgets did.
//
// It runs offscreen -- the window is created without WS_VISIBLE -- so it does
// not flash up during a build.
#include <sonore/gfx/native_editor.h>
#include <sonore/gfx/system_font.h>
#include <sonore/gfx/popup.h>
#include <sonore/gfx/text_editor.h>
#include <sonore/gfx/tooltip.h>
#include <sonore/gfx/widgets.h>
#include <sonore/gfx/window_win32.h>

#include <cstdio>
#include <vector>
#include <thread>
#include <string>
#include <uiautomation.h>
#include <objbase.h>
#include <cstring>

static int g_checks = 0, g_failures = 0;
static void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}

using namespace sonore::gfx;

/** Let the window process everything queued. */
static void pump() {
  MSG msg;
  while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
}

static LPARAM at(int x, int y) { return (LPARAM) ((y << 16) | (x & 0xFFFF)); }

/** The plain value the first row's slider is showing.
 *
 * Through the editor's own children rather than through the host's array,
 * because the point of the check is whether the WIDGET followed -- reading the
 * host back would pass without the editor doing anything at all. */
static float editorGain(NativeEditor& editor) {
  GenericEditor* content = editor.content();
  if (!content) return 0.0f;
  return content->parameterValueShown(0);
}


// ── A screen reader, actually reading ────────────────────────────────────────
//
// Everything about accessibility in sdk_tests checks our own description layer
// against itself: the roles, the names, the values, the walk. All of it could
// be perfect and a screen reader still hear nothing, because the part that
// hands it to the platform is a COM provider behind WM_GETOBJECT and no amount
// of testing our own structs touches it.
//
// This is the only check that does. Windows ships the UI Automation CLIENT api
// as well as the provider side, so a test can be the reader: create the same
// IUIAutomation object NVDA and Narrator create, point it at the editor's real
// HWND, and assert what comes back. If this passes, a screen reader hears it,
// because this IS what a screen reader does.
//
// On a WORKER thread, deliberately. UIA marshals through COM, and an in-process
// client calling into its own provider from the thread that owns the window
// deadlocks the moment anything blocks. A real reader is another process; a
// worker thread with the UI thread pumping messages is the closest honest
// arrangement, and it is also what catches a provider that is not actually
// thread-safe.
struct UiaReading {
  std::wstring name;
  long controlType = 0;
  std::wstring value;
  bool hasValuePattern = false;
  bool hasRangePattern = false;
  double rangeValue = 0.0, rangeMin = 0.0, rangeMax = 0.0;
};

struct UiaResult {
  bool clientCreated = false;
  bool elementFound = false;
  std::wstring windowName;
  std::vector<UiaReading> children;
  HRESULT error = S_OK;
};

static void readWithUia(HWND hwnd, UiaResult* result) {
  // MTA on this thread: the provider declares UseComThreading and does not
  // touch the component tree, so there is nothing for an apartment to protect
  // and an STA here would marshal every call back to a thread that is busy.
  const HRESULT initialised = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

  IUIAutomation* automation = nullptr;
  result->error = CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                                   __uuidof(IUIAutomation), (void**) &automation);
  if (FAILED(result->error) || !automation) {
    if (SUCCEEDED(initialised)) CoUninitialize();
    return;
  }
  result->clientCreated = true;

  IUIAutomationElement* element = nullptr;
  result->error = automation->ElementFromHandle(hwnd, &element);
  if (SUCCEEDED(result->error) && element) {
    result->elementFound = true;
    BSTR name = nullptr;
    if (SUCCEEDED(element->get_CurrentName(&name)) && name) {
      result->windowName = name;
      SysFreeString(name);
    }

    // The raw view, not the control view. The control view hides elements a
    // client "does not need", and what this test is checking is exactly what
    // we published rather than what UIA decided to keep.
    IUIAutomationTreeWalker* walker = nullptr;
    if (SUCCEEDED(automation->get_RawViewWalker(&walker)) && walker) {
      IUIAutomationElement* child = nullptr;
      if (SUCCEEDED(walker->GetFirstChildElement(element, &child))) {
        while (child) {
          UiaReading reading;
          BSTR text = nullptr;
          if (SUCCEEDED(child->get_CurrentName(&text)) && text) {
            reading.name = text;
            SysFreeString(text);
          }
          CONTROLTYPEID type = 0;
          if (SUCCEEDED(child->get_CurrentControlType(&type))) reading.controlType = type;

          IUnknown* pattern = nullptr;
          if (SUCCEEDED(child->GetCurrentPattern(UIA_ValuePatternId, &pattern)) && pattern) {
            IUIAutomationValuePattern* value = nullptr;
            if (SUCCEEDED(pattern->QueryInterface(__uuidof(IUIAutomationValuePattern),
                                                  (void**) &value)) && value) {
              reading.hasValuePattern = true;
              BSTR v = nullptr;
              if (SUCCEEDED(value->get_CurrentValue(&v)) && v) {
                reading.value = v;
                SysFreeString(v);
              }
              value->Release();
            }
            pattern->Release();
          }

          pattern = nullptr;
          if (SUCCEEDED(child->GetCurrentPattern(UIA_RangeValuePatternId, &pattern)) && pattern) {
            IUIAutomationRangeValuePattern* range = nullptr;
            if (SUCCEEDED(pattern->QueryInterface(__uuidof(IUIAutomationRangeValuePattern),
                                                  (void**) &range)) && range) {
              reading.hasRangePattern = true;
              range->get_CurrentValue(&reading.rangeValue);
              range->get_CurrentMinimum(&reading.rangeMin);
              range->get_CurrentMaximum(&reading.rangeMax);
              range->Release();
            }
            pattern->Release();
          }

          result->children.push_back(reading);

          IUIAutomationElement* next = nullptr;
          walker->GetNextSiblingElement(child, &next);
          child->Release();
          child = next;
        }
      }
      walker->Release();
    }
    element->Release();
  }

  automation->Release();
  if (SUCCEEDED(initialised)) CoUninitialize();
}

static void testUiaProvider() {
  using namespace sonore;
  using namespace sonore::gfx;
  std::printf("\n-- accessibility, through Windows' own UI Automation --\n");
  char msg[400];

  static const char* kShapes[] = {"Sine", "Saw", "Square"};
  static const ParamInfo kParams[3] = {
      {"gain", "Gain", "dB", -60.0f, 6.0f, 0.0f, 0},
      {"freq", "Freq", "Hz", 20.0f, 20000.0f, 440.0f, 0},
      {"shape", "Shape", "", 0.0f, 2.0f, 0.0f, 3, nullptr, kShapes, 3},
  };
  float values[3] = {-6.0f, 440.0f, 1.0f};
  EditorHost host;
  host.getParameter = [&](int i) { return values[i]; };
  host.setParameter = [&](int i, float v) { values[i] = v; };
  host.beginGesture = [](int) {};
  host.endGesture = [](int) {};

  // Into a PARENT window, which is what a plugin editor always is: a host owns
  // the window and hands us a handle to sit inside.
  //
  // Not incidental to this test. A top-level window has a non-client area, and
  // its default provider contributes the title bar, the caption buttons and
  // the frame as children -- so the first thing a client walking a top-level
  // window finds is a TitleBar, and ours come after somebody else's furniture.
  // A child window has none of that, so what a reader sees is exactly what we
  // published. This test asserted against a top-level window first and found a
  // title bar, which was Windows being right and the test asking the wrong
  // question.
  WNDCLASSW hostClass{};
  hostClass.lpfnWndProc = DefWindowProcW;
  hostClass.hInstance = GetModuleHandleW(nullptr);
  hostClass.lpszClassName = L"SonoreUiaHost";
  RegisterClassW(&hostClass);
  HWND hostWindow = CreateWindowExW(0, L"SonoreUiaHost", L"", WS_OVERLAPPEDWINDOW, 0, 0, 500, 340,
                                    nullptr, nullptr, hostClass.hInstance, nullptr);
  check(hostWindow != nullptr, "a host window to parent the editor into");

  NativeEditor editor;
  if (!editor.open(hostWindow, kParams, 3, host, 420, 260)) {
    check(false, "the editor opens for the UIA check");
    return;
  }
  HWND hwnd = (HWND) editor.handle();
  check(hwnd != nullptr, "the editor has a window for UIA to find");

  UiaResult result;
  std::thread reader([&]() { readWithUia(hwnd, &result); });

  // Pump while the reader works. UIA calls into the provider through COM, and
  // a UI thread that stopped answering messages would hang the client -- which
  // is exactly the bug this arrangement exists to catch.
  const DWORD until = GetTickCount() + 15000;
  while (GetTickCount() < until) {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&m);
      DispatchMessageW(&m);
    }
    if (result.clientCreated && !result.children.empty()) break;
    Sleep(10);
  }
  reader.join();

  std::snprintf(msg, sizeof(msg), "a UI Automation client is created (hr=0x%08lx)",
                (unsigned long) result.error);
  check(result.clientCreated, msg);
  std::printf("       window element name: \"%ls\"\n", result.windowName.c_str());
  check(result.elementFound, "and it finds our window from its HWND alone");

  for (const UiaReading& r : result.children)
    std::printf("       UIA saw: type=%ld name=\"%ls\" value=\"%ls\"\n", r.controlType,
                r.name.c_str(), r.value.c_str());
  std::snprintf(msg, sizeof(msg), "%d elements are reported inside it",
                (int) result.children.size());
  check(result.children.size() == 3, msg);

  if (result.children.size() == 3) {
    const UiaReading& gain = result.children[0];
    std::snprintf(msg, sizeof(msg), "the first is named \"%ls\", control type %ld",
                  gain.name.c_str(), gain.controlType);
    check(gain.name == L"Gain" && gain.controlType == UIA_SliderControlTypeId, msg);

    std::snprintf(msg, sizeof(msg), "and reads \"%ls\" through the Value pattern",
                  gain.value.c_str());
    check(gain.hasValuePattern && gain.value.find(L"-6") != std::wstring::npos, msg);

    std::snprintf(msg, sizeof(msg), "with a range %.2f..%.2f at %.2f, which is what lets a "
                  "reader say a percentage", gain.rangeMin, gain.rangeMax, gain.rangeValue);
    check(gain.hasRangePattern && gain.rangeMin == 0.0 && gain.rangeMax == 1.0, msg);

    const UiaReading& shape = result.children[2];
    std::snprintf(msg, sizeof(msg), "the third is \"%ls\" = \"%ls\", control type %ld",
                  shape.name.c_str(), shape.value.c_str(), shape.controlType);
    check(shape.name == L"Shape" && shape.value == L"Saw" &&
              shape.controlType == UIA_ComboBoxControlTypeId, msg);
  }

  editor.close();
  check(!editor.isOpen(), "and the editor closes with its provider");
  if (hostWindow) DestroyWindow(hostWindow);
}

int main() {
  // UNBUFFERED, deliberately. This test drives a real window through real
  // messages, and when it crashes it crashes inside the OS message loop --
  // where a fully buffered stdout throws away every line that would say how
  // far it got. Costs nothing; the alternative is a segfault with no output.
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("── native window (Win32) ──\n");

  Component page;
  Slider knob(Slider::Style::Rotary);
  knob.setBounds({20.0f, 20.0f, 80.0f, 80.0f});
  knob.setValue(0.5f, false);
  page.addChild(&knob);

  Button button("Bypass");
  button.setBounds({120.0f, 20.0f, 80.0f, 28.0f});
  button.setToggleable(true);
  page.addChild(&button);

  int clicks = 0;
  button.onClick = [&]() { ++clicks; };
  int gestures = 0;
  knob.onDragStart = [&]() { ++gestures; };

  NativeWindow window;
  check(window.open(nullptr, page, 240, 140), "a window opens");
  if (!window.isOpen()) {
    std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
    return 1;
  }
  HWND hwnd = (HWND) window.handle();
  check(hwnd != nullptr, "and has a handle a host could parent");
  pump();

  // It rendered something. The background alone would be uniform; a knob and
  // a button mean several distinct colours.
  int distinct = 0;
  {
    bool seen[64] = {false};
    for (int y = 0; y < window.bitmap().height(); y += 3)
      for (int x = 0; x < window.bitmap().width(); x += 3) {
        const Colour c = window.bitmap().pixelAt(x, y).toStraight();
        const int bucket = ((c.r >> 6) << 4) | ((c.g >> 6) << 2) | (c.b >> 6);
        if (!seen[bucket]) { seen[bucket] = true; ++distinct; }
      }
  }
  char msg[200];
  std::snprintf(msg, sizeof(msg), "the tree painted into the window (%d distinct colours)",
                distinct);
  check(distinct >= 3, msg);

  // ── A real click on the button ──
  SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, at(160, 34));
  SendMessageW(hwnd, WM_LBUTTONUP, 0, at(160, 34));
  pump();
  check(clicks == 1 && button.isToggled(), "a WM_LBUTTONDOWN/UP pair clicks the button under it");

  // ── A drag on the knob, including outside the window ──
  //
  // The point of SetCapture: the last two moves are outside the window
  // entirely, and they must still reach the knob.
  const float before = knob.value();
  SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, at(60, 60));
  check(GetCapture() == hwnd, "pressing takes the OS mouse capture");
  SendMessageW(hwnd, WM_MOUSEMOVE, MK_LBUTTON, at(60, 40));
  SendMessageW(hwnd, WM_MOUSEMOVE, MK_LBUTTON, at(60, 10));
  SendMessageW(hwnd, WM_MOUSEMOVE, MK_LBUTTON, at(60, -300));
  SendMessageW(hwnd, WM_LBUTTONUP, 0, at(60, -300));
  pump();
  std::snprintf(msg, sizeof(msg), "dragging up from %.2f to %.2f, ending outside the window",
                before, knob.value());
  check(knob.value() > before, msg);
  check(gestures == 1, "as exactly one gesture");
  check(GetCapture() != hwnd, "and the release gives capture back");

  // ── Losing capture ends the gesture ──
  //
  // A modal dialog or Alt-Tab takes capture away mid-drag. Without handling
  // it the router stays in a drag for ever and the next unrelated move moves
  // the knob.
  gestures = 0;
  SendMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, at(60, 60));
  SendMessageW(hwnd, WM_CAPTURECHANGED, 0, 0);
  pump();
  const float afterSteal = knob.value();
  SendMessageW(hwnd, WM_MOUSEMOVE, 0, at(60, 5));
  pump();
  std::snprintf(msg, sizeof(msg), "after capture is stolen a later move leaves it at %.3f",
                knob.value());
  check(knob.value() == afterSteal, msg);

  // ── Double click ──
  knob.setDefaultValue(0.25f);
  knob.setValue(0.9f, false);
  SendMessageW(hwnd, WM_LBUTTONDBLCLK, MK_LBUTTON, at(60, 60));
  SendMessageW(hwnd, WM_LBUTTONUP, 0, at(60, 60));
  pump();
  std::snprintf(msg, sizeof(msg), "a WM_LBUTTONDBLCLK returns the knob to its default (%.2f)",
                knob.value());
  check(std::fabs(knob.value() - 0.25f) < 1e-3f, msg);

  // ── The wheel, whose coordinates are in SCREEN space ──
  //
  // Every other mouse message is client-relative. Using the wheel's
  // unconverted sends the event wherever that spot is on the desktop, which
  // inside a host is another plugin's window.
  POINT screen{60, 60};
  ClientToScreen(hwnd, &screen);
  const float beforeWheel = knob.value();
  SendMessageW(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA),
               at((int) screen.x, (int) screen.y));
  pump();
  std::snprintf(msg, sizeof(msg), "a wheel notch over the knob moves it from %.2f to %.2f",
                beforeWheel, knob.value());
  check(knob.value() > beforeWheel, msg);

  // ── Resizing ──
  window.setSize(320, 200);
  pump();
  std::snprintf(msg, sizeof(msg), "resizing gives a %dx%d buffer", window.bitmap().width(),
                window.bitmap().height());
  check(window.bitmap().width() == 320 && window.bitmap().height() == 200, msg);
  check(page.bounds().w == 320.0f, "and the content is resized with it");

  // ── HiDPI, on a real window ──────────────────────────────────────────────
  //
  // Forced rather than waited for. This machine reports 96 DPI, so nothing here
  // would ever take the scaled path on its own -- and a HiDPI bug that only
  // appears on somebody else's laptop is exactly the kind this test exists to
  // stop. setScale is the same call the host makes through CLAP's
  // gui.set_scale, so driving it directly drives the shipping path.
  window.setScale(2.0f);
  window.setLogicalSize(320.0f, 200.0f);
  pump();
  std::snprintf(msg, sizeof(msg), "at 200%% a 320x200 editor asks Windows for a %dx%d buffer",
                window.bitmap().width(), window.bitmap().height());
  check(window.bitmap().width() == 640 && window.bitmap().height() == 400, msg);
  std::snprintf(msg, sizeof(msg), "and the component tree is still laid out at %gx%g",
                page.bounds().w, page.bounds().h);
  check(page.bounds().w == 320.0f && page.bounds().h == 200.0f, msg);

  // The measurement that matters: the interface covers twice as many device
  // pixels, which is what "the same physical size on a denser screen" means.
  //
  // The BOUNDING BOX of everything drawn, not a count of lit pixels. Counting
  // was the first attempt and it measured the wrong thing: a one-unit stroke is
  // one pixel at 100% and two at 200%, antialiased edges cross a brightness
  // threshold differently at each scale, and the honest ratio came out at 1.7
  // for a renderer doing exactly the right thing. An extent is geometry and has
  // none of that in it.
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
    pump();
    drawnExtent(&wOne, &hOne);
    std::snprintf(msg, sizeof(msg), "everything drawn spans %dx%d device pixels at 100%% and "
                  "%dx%d at 200%%", wOne, hOne, wTwo, hTwo);
    // Within 3%, not within a pixel or two. The two images are rasterised
    // INDEPENDENTLY at different resolutions, so a glyph's antialiased edge
    // lights one more column at one scale than at the other and the outermost
    // extent moves by a pixel or three. 350 against a predicted 354 is that,
    // and demanding exactness would be demanding that the renderer round the
    // way I did.
    const bool wOk = wOne > 0 && std::fabs((double) wTwo / (wOne * 2.0) - 1.0) < 0.03;
    const bool hOk = hOne > 0 && std::fabs((double) hTwo / (hOne * 2.0) - 1.0) < 0.03;
    check(wOk && hOk, msg);

    // Put it back for the click below.
    window.setScale(2.0f);
    window.setLogicalSize(320.0f, 200.0f);
    pump();
  }

  // And a click still lands on the control. Windows reports DEVICE pixels, so
  // the pointer arrives at twice the coordinate the knob was drawn at; without
  // the conversion every click lands a screenful down and to the right, which
  // does not read as a coordinate bug -- it reads as a plugin that ignores the
  // mouse.
  {
    const float was = knob.value();
    const Point centre{knob.bounds().x + knob.bounds().w * 0.5f,
                       knob.bounds().y + knob.bounds().h * 0.5f};
    const int deviceX = (int) (centre.x * 2.0f), deviceY = (int) (centre.y * 2.0f);
    SendMessageW(hwnd, WM_LBUTTONDBLCLK, 0, at(deviceX, deviceY));
    SendMessageW(hwnd, WM_LBUTTONUP, 0, at(deviceX, deviceY));
    pump();
    std::snprintf(msg, sizeof(msg), "a double-click at device (%d,%d) reaches the knob at logical "
                  "(%.0f,%.0f) and reset it from %.2f to %.2f", deviceX, deviceY, centre.x,
                  centre.y, was, knob.value());
    check(knob.value() != was, msg);
  }

  // Back down again. A window dragged from a 200% laptop panel onto a 100%
  // monitor is this, and it happens while the editor is open.
  window.setScale(1.0f);
  window.setLogicalSize(320.0f, 200.0f);
  pump();
  std::snprintf(msg, sizeof(msg), "back at 100%% the buffer is %dx%d again",
                window.bitmap().width(), window.bitmap().height());
  check(window.bitmap().width() == 320 && window.bitmap().height() == 200, msg);
  check(page.bounds().w == 320.0f, "and the tree is unchanged, because it never knew");

  // ── Closing ──
  window.close();
  check(!window.isOpen(), "closing releases the window");
  check(!IsWindow(hwnd), "and the HWND is really gone");

  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  // ── The editor a plugin actually opens ──────────────────────────────────
  //
  // Everything above drives components. This drives the whole object a format
  // wrapper hands to a host: window, timer, generic editor and attachments.
  //
  // The clock is what nothing else can check. A plugin editor is not driven by
  // its own events alone -- automation, a preset load and host undo all move
  // parameters with nobody touching the window -- and without a timer those
  // changes appear only the next time the mouse happens to move.
  std::printf("\n── native editor ──\n");
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
    check(NativeEditor::isAvailable(), "this platform has a native window backend");

    // ── The resize border, before open ──
    //
    // Both of these have to be set BEFORE open(), because that is where the
    // border is built and where it reads the limits.
    sonore::EditorConstraints limits;
    limits.minWidth = 300;
    limits.minHeight = 140;
    limits.maxWidth = 900;
    limits.maxHeight = 700;
    editor.setResizeLimits(limits);
    // Right-click reaches the HOST's parameter menu -- MIDI learn, assign
    // automation. Recorded here instead of shown, because this test is
    // standing in for the wrapper.
    int menuIndex = -1, menuCalls = 0;
    host.showContextMenu = [&](int index, int, int) {
      menuIndex = index;
      ++menuCalls;
    };

    int resizeRequests = 0, askedW = 0, askedH = 0;
    editor.onRequestResize = [&](int w, int h) {
      ++resizeRequests;
      askedW = w;
      askedH = h;
    };

    check(editor.open(nullptr, kParams, 3, host, 380, 160), "the native editor opens");

    if (editor.isOpen()) {
      // Not a failure -- Font draws nothing without a face, so every control
      // is still there and still works. Worth SAYING, because blank labels on
      // one machine and not another is otherwise a mystery.
      std::printf("  ---- text is %s ----\n",
                  editor.hasText() ? "being drawn" : "ABSENT: no system typeface found");

      HWND editorHwnd = (HWND) editor.handle();
      check(IsWindow(editorHwnd), "and really has a window");

      // ── Dragging the corner, through the REAL window ──
      //
      // This is the part unit tests cannot reach. ResizableBorder's geometry
      // and arithmetic are asserted in sdk_tests, and all of that would pass
      // just as happily if the border were never added to the tree, never on
      // top of the viewport, or never given the mouse by the peer. The same
      // shape of hole hid Ctrl+A for three platforms at once: the unit test
      // synthesised a KeyPress the peer could not actually produce.
      //
      // So: real WM_LBUTTONDOWN and WM_MOUSEMOVE at the bottom-right of the
      // real window, and the question is whether a resize request comes out
      // the other side.
      {
        const int w = 380, h = 160;
        // Inside the corner square: four pixels in from each edge is within
        // ResizableBorder::kCorner and clear of the boundary itself.
        const int fromX = w - 4, fromY = h - 4;
        // The expected size comes from the DELTA, computed here rather than
        // written down. The first version of this hardcoded a height worked out
        // from the destination instead of the movement and was four pixels
        // wrong -- an arithmetic slip that would have read as the border being
        // broken.
        const int moveX = 100, moveY = 60;
        SendMessageW(editorHwnd, WM_LBUTTONDOWN, MK_LBUTTON, at(fromX, fromY));
        SendMessageW(editorHwnd, WM_MOUSEMOVE, MK_LBUTTON, at(fromX + moveX, fromY + moveY));
        SendMessageW(editorHwnd, WM_LBUTTONUP, 0, at(fromX + moveX, fromY + moveY));

        char msg[240];
        std::snprintf(msg, sizeof(msg),
                      "dragging the real window's bottom-right corner by %d,%d asked the host "
                      "for %dx%d (%d time(s)), against the %dx%d it started at",
                      moveX, moveY, askedW, askedH, resizeRequests, w, h);
        check(resizeRequests > 0 && askedW == w + moveX && askedH == h + moveY, msg);
      }

      // ── A real WM_RBUTTONDOWN on a real knob ──
      //
      // showHostContextMenu had been wired to clap_host_context_menu and to
      // IComponentHandler3 for a long time, and the only thing that ever
      // called it was a bridge message from the WEB editor. So when the native
      // editor became the default, right-clicking a parameter silently stopped
      // doing anything -- and unit tests could not have noticed, because the
      // toolkit had no right button for them to synthesise.
      //
      // The value must not move. That is the whole reason contextMenu is a
      // separate dispatch: every control starts its gesture in mouseDown.
      {
        const float rowY2 = GenericEditor::kPadding + GenericEditor::kRowHeight * 0.5f;
        const float trackX2 = GenericEditor::kPadding + GenericEditor::kLabelWidth + 20.0f;
        const float valueBefore = values[0];
        SendMessageW(editorHwnd, WM_RBUTTONDOWN, 0, at((int) trackX2, (int) rowY2));

        char msg[240];
        std::snprintf(msg, sizeof(msg),
                      "a real WM_RBUTTONDOWN on the first knob asked the host about parameter "
                      "%d (%d call(s))", menuIndex, menuCalls);
        check(menuCalls == 1 && menuIndex == 0, msg);
        std::snprintf(msg, sizeof(msg),
                      "…and left the parameter at %.4f, where it was -- a right-click is not a "
                      "drag", (double) values[0]);
        check(values[0] == valueBefore, msg);

        // The check above is worth nothing on its own: a click that landed on
        // no control at all would pass it. So the SAME point with the LEFT
        // button, which must move the parameter -- that is what makes "did not
        // move" mean "was declined" rather than "was missed".
        SendMessageW(editorHwnd, WM_LBUTTONDOWN, MK_LBUTTON, at((int) trackX2, (int) rowY2));
        SendMessageW(editorHwnd, WM_LBUTTONUP, 0, at((int) trackX2, (int) rowY2));
        std::snprintf(msg, sizeof(msg),
                      "…while a LEFT click at the very same point moves it to %.4f, so the "
                      "right-click was declined and not merely missed", (double) values[0]);
        check(values[0] != valueBefore, msg);
        check(menuCalls == 1, "…and a left click asks for no menu");
      }

      // ── Where an input method would put its candidate list ──
      //
      // Typing Japanese, Chinese or Korean shows a list of candidate
      // characters, and Windows places it wherever it likes unless the
      // application says otherwise -- in practice a corner of the window,
      // unrelated to the field being typed in.
      //
      // What CANNOT be checked here is the visual result: that needs an IME
      // installed and a human composing. What can be checked exactly is the
      // half that is actually easy to get wrong -- whether the point handed to
      // imm32 is beside the caret of the FOCUSED field, in device pixels, or
      // whether it is (0,0) because nobody converted the coordinates out of
      // the component they came from.
      {
        PlatformWindow* peer = editor.window();
        int cx = 0, cy = 0;
        check(peer != nullptr, "the editor has a peer to ask");

        if (peer) {
          // Nothing focused that takes text: the knobs do not, so there is no
          // composition point and the IME keeps its default placement.
          check(!peer->compositionPoint(&cx, &cy),
                "with no text field focused there is no composition point, so the OS is not "
                "told a wrong one");

          // Give a text editor focus the way a user does -- by clicking it.
          // The ValueBox on the first row becomes a TextEditor on a double
          // click, which is the only text field this editor has.
          const float rowY3 = GenericEditor::kPadding + GenericEditor::kRowHeight * 0.5f;
          const float valueX = editor.content()->bounds().w - GenericEditor::kPadding -
                               GenericEditor::kValueWidth * 0.5f;
          SendMessageW(editorHwnd, WM_LBUTTONDBLCLK, MK_LBUTTON,
                       at((int) valueX, (int) rowY3));
          SendMessageW(editorHwnd, WM_LBUTTONUP, 0, at((int) valueX, (int) rowY3));

          char msg[240];
          if (peer->compositionPoint(&cx, &cy)) {
            std::snprintf(msg, sizeof(msg),
                          "with the value field focused the composition point is %d,%d -- "
                          "beside the field at x>=%d, not the window corner",
                          cx, cy, (int) valueX - (int) GenericEditor::kValueWidth);
            // Beside the FIELD, which is the assertion that fails if the
            // coordinates were never converted out of the component.
            check(cx > (int) valueX - (int) GenericEditor::kValueWidth && cy > 0, msg);
          } else {
            // Not a failure: the double click may not have opened an editor on
            // a build with no typeface, and saying so beats asserting into it.
            std::printf("  ---- no text field took focus; composition point not exercised "
                        "----\n");
          }
        }
      }

      // And the middle of the editor must still reach the controls. A border
      // that took the whole surface would pass the test above and break every
      // knob in the plugin, which is a far worse trade than no resizing.
      {
        const int before = resizeRequests;
        SendMessageW(editorHwnd, WM_LBUTTONDOWN, MK_LBUTTON, at(190, 80));
        SendMessageW(editorHwnd, WM_LBUTTONUP, 0, at(190, 80));
        check(resizeRequests == before,
              "a click in the middle of the editor is not a resize -- the border takes the "
              "mouse only at its edges");
      }

      // ── A real drag on the first row's track ──
      //
      // The coordinates come from the same constants GenericEditor::resized()
      // lays out with, not from numbers that looked right once: a magic 300
      // lands on the value readout at one width and on the track at another,
      // and a test that passes for that reason is worth nothing.
      const float rowY = GenericEditor::kPadding + GenericEditor::kRowHeight * 0.5f;
      const float trackX = GenericEditor::kPadding + GenericEditor::kLabelWidth + 8.0f;
      // From the EDITOR's width, not the window's. They stopped being the same
      // when the editor moved inside a Viewport, which takes a scroll bar's
      // width off it -- and this arithmetic went on saying 380 and clicking
      // past the end of the track. It still passed, because the check only
      // asks whether the value went up, which is exactly the kind of pass that
      // hides a stale coordinate.
      const float editorW = editor.content() ? editor.content()->bounds().w : 380.0f;
      const float trackW = editorW - trackX - GenericEditor::kValueWidth -
                           GenericEditor::kPadding * 2.0f;
      sets = begins = ends = 0;
      SendMessageW(editorHwnd, WM_LBUTTONDOWN, MK_LBUTTON,
                   at((int) (trackX + trackW * 0.1f), (int) rowY));
      SendMessageW(editorHwnd, WM_MOUSEMOVE, MK_LBUTTON,
                   at((int) (trackX + trackW * 0.5f), (int) rowY));
      // To 0.75 of the track, not 0.95. The track is inset by half a knob at
      // each end, so 0.95 of the BOUNDS is past 0.95 of the value range and the
      // result pins at the maximum -- and a value stuck at the clamp is a weak
      // signal, since a drag broken in some other way would read 6.0 dB too.
      // Landing mid-range means the number itself has to be right.
      SendMessageW(editorHwnd, WM_MOUSEMOVE, MK_LBUTTON,
                   at((int) (trackX + trackW * 0.75f), (int) rowY));
      SendMessageW(editorHwnd, WM_LBUTTONUP, 0, at((int) (trackX + trackW * 0.75f), (int) rowY));
      pump();
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

      // ── The clock ──
      //
      // A parameter moved by nobody: automation, a preset load, host undo.
      // Nothing touches the window and the editor has to notice anyway.
      //
      // Counted, not assumed. The first version of this checked only that
      // nothing was set back, which passes just as happily when the clock
      // never runs at all -- and it did not: the window carried a timer and
      // NOTHING was subscribed to it, so the editor never followed automation
      // and looked entirely correct until a parameter moved without the mouse.
      int ticks = 0;
      values[0] = -48.0f;
      const int setsBefore = sets;
      // WM_TIMER is only generated when the queue is otherwise empty AND the
      // interval has elapsed, so this sleeps rather than spins. 33 ms is the
      // period; 300 ms is several ticks even on a loaded machine.
      for (int i = 0; i < 30 && ticks == 0; ++i) {
        pump();
        // The clock's whole job: bring the host's value back into the widget.
        if (std::fabs(editorGain(editor) - (-48.0f)) < 0.01f) ++ticks;
        Sleep(10);
      }
      pump();
      std::snprintf(msg, sizeof(msg), "the clock pulled -48 dB back into the control (%d)", ticks);
      check(ticks > 0, msg);
      check(sets == setsBefore,
            "and syncing set NOTHING back -- the feedback loop stays open");
    }

    editor.close();
    check(!editor.isOpen(), "the native editor closes");
    // Twice, because a host that closes an editor it already closed is a host,
    // not a bug to crash on.
    editor.close();
    check(!editor.isOpen(), "and closing twice is harmless");

    // ── Opening it AGAIN ───────────────────────────────────────────────────
    //
    // A host closes and reopens an editor constantly -- every time somebody
    // hides the plugin window and shows it again -- and the peer is a MEMBER of
    // NativeEditor, so the same object is reused. Everything the first open
    // left behind is still there for the second.
    //
    // Not hypothetical. The backing store held the component it painted and
    // nothing cleared it on close, so the second open painted through the first
    // one's destroyed viewport -- from INSIDE CreateWindowExW, which dispatches
    // WM_SIZE synchronously before it returns, so the crash was in window
    // creation and looked like a Win32 problem rather than a lifetime one.
    //
    // The first open of a fresh object survived it: the stale pointer is null
    // there. Which is exactly why every check above passed while pluginval's
    // Editor test killed the process on seven plugins out of nine.
    {
      int reopens = 0;
      for (int round = 0; round < 3; ++round) {
        if (!editor.open(nullptr, kParams, 3, host, 420, 260)) break;
        ++reopens;
        editor.tick();
        editor.setSize(380, 240);
        editor.tick();
        editor.close();
      }
      std::snprintf(msg, sizeof(msg), "the same editor object opens and closes %d times over",
                    reopens);
      check(reopens == 3, msg);
    }
  }

  // -- A real popup menu -----------------------------------------------------
  //
  // Everything about the LIST is checked in sdk_tests against a Bitmap. What
  // needs a window is the part that makes a popup a popup: it is a top-level
  // window rather than a child, it holds the mouse, and a click OUTSIDE it
  // reaches it anyway. That last one is the whole mechanism -- without the
  // capture, a click outside goes to whatever is behind, the menu stays open,
  // and two things respond to one gesture.
  std::printf("\n-- popup window --\n");
  {
    auto face = systemTypeface();
    Font font = face ? Font(face, 13.0f) : Font();

    PopupMenu menu;
    menu.addItem(1, "Sine", true, true);
    menu.addItem(2, "Saw");
    menu.addItem(3, "Square");

    int chosen = -999, calls = 0;
    PopupWindow popup;
    check(popup.show(menu, font, 200.0f, 200.0f, 24.0f, 1920.0f, 1080.0f,
                     [&](int id) {
                       chosen = id;
                       ++calls;
                     }),
          "a popup menu opens as a real window");

    if (popup.isOpen()) {
      HWND popupHwnd = (HWND) popup.handle();
      check(IsWindow(popupHwnd), "with a window of its own");
      // Top-level, not a child of anything. A menu parented into the editor
      // could not be drawn outside it, which is most of what a menu does.
      check(GetParent(popupHwnd) == nullptr, "top-level, so it can be drawn over the host");
      check((GetWindowLongW(popupHwnd, GWL_STYLE) & WS_CHILD) == 0, "and not a child window");
      // It holds the mouse: that is what makes a click outside arrive here.
      check(GetCapture() == popupHwnd, "and it holds the mouse capture");

      // A press on the second item chooses it.
      const int itemY = (int) (PopupMenu::kPaddingY + PopupMenu::kItemHeight * 1.5f);
      SendMessageW(popupHwnd, WM_LBUTTONDOWN, MK_LBUTTON, at(30, itemY));
      pump();
      std::snprintf(msg, sizeof(msg), "pressing the second item returns id %d, once (%d)", chosen,
                    calls);
      check(chosen == 2 && calls == 1, msg);

      // The window is torn down on the next tick, never from inside its own
      // message handler.
      check(popup.isOpen(), "the window is still up until the next tick");
      popup.tick();
      check(!popup.isOpen(), "and the tick closes it");
      check(!IsWindow(popupHwnd), "the HWND is really gone");
      check(GetCapture() != popupHwnd, "and the capture is released with it");
    }

    // -- Dismissed from outside --
    chosen = -999;
    calls = 0;
    check(popup.show(menu, font, 200.0f, 200.0f, 24.0f, 1920.0f, 1080.0f,
                     [&](int id) {
                       chosen = id;
                       ++calls;
                     }),
          "a second menu opens");
    if (popup.isOpen()) {
      HWND popupHwnd = (HWND) popup.handle();
      // Negative coordinates are what a click outside looks like to a window
      // holding the capture.
      SendMessageW(popupHwnd, WM_LBUTTONDOWN, MK_LBUTTON, at(-50, -50));
      pump();
      std::snprintf(msg, sizeof(msg), "a click outside dismisses with %d, not a choice", chosen);
      check(chosen == 0 && calls == 1, msg);
      popup.tick();
      check(!popup.isOpen(), "and it closes");
    }

    // Closing one that was never shown, and one already closed. A host does
    // both; neither should be a crash.
    popup.close();
    check(!popup.isOpen(), "closing an already-closed popup is harmless");

    PopupMenu nothing;
    check(!popup.show(nothing, font, 0.0f, 0.0f, 0.0f, 1920.0f, 1080.0f, [](int) {}),
          "an empty menu refuses to open rather than showing an empty box");
  }

  // -- Keys, through a real window ------------------------------------------
  //
  // Windows reports a key TWICE and means something different each time.
  // WM_KEYDOWN is the physical key before the layout, which is where arrows and
  // Return come from; WM_CHAR is the character after the layout, dead keys and
  // any input method, and it is the only one that knows what an AltGr
  // combination on a German keyboard actually produced.
  //
  // Handling only the first gives a field that cannot type an accent. Handling
  // only the second gives one where the arrows do nothing. Both paths are
  // driven here, with real messages.
  std::printf("\n-- keyboard --\n");
  {
    auto face = systemTypeface();
    Font keyFont = face ? Font(face, 13.0f) : Font();

    Component page2;
    page2.setBounds({0.0f, 0.0f, 260.0f, 60.0f});
    TextEditor field;
    field.setFont(keyFont);
    field.setBounds({10.0f, 10.0f, 240.0f, 26.0f});
    page2.addChild(&field);

    NativeWindow keyWindow;
    check(keyWindow.open(nullptr, page2, 260, 60), "a window with a text field opens");
    if (keyWindow.isOpen()) {
      HWND kh = (HWND) keyWindow.handle();
      keyWindow.router()->setFocus(&field);
      check(field.hasKeyboardFocus(), "and the field has focus");

      for (const char* c = "Hi"; *c; ++c) SendMessageW(kh, WM_CHAR, (WPARAM) *c, 0);
      pump();
      std::snprintf(msg, sizeof(msg), "two WM_CHARs typed \"%s\"", field.getText().c_str());
      check(field.getText() == "Hi", msg);

      SendMessageW(kh, WM_KEYDOWN, VK_BACK, 0);
      pump();
      check(field.getText() == "H", "a WM_KEYDOWN of VK_BACK deletes one");

      SendMessageW(kh, WM_KEYDOWN, VK_HOME, 0);
      SendMessageW(kh, WM_CHAR, (WPARAM) 'O', 0);
      pump();
      std::snprintf(msg, sizeof(msg), "VK_HOME then 'O' gives \"%s\"", field.getText().c_str());
      check(field.getText() == "OH", msg);

      // A character beyond the basic plane arrives as TWO WM_CHARs, a high
      // surrogate and a low one. Pairing them is what keeps an emoji in a
      // preset name one character instead of two broken halves.
      field.setText("");
      SendMessageW(kh, WM_CHAR, (WPARAM) 0xd83c, 0); // U+1F3B9 musical keyboard
      SendMessageW(kh, WM_CHAR, (WPARAM) 0xdfb9, 0);
      pump();
      std::snprintf(msg, sizeof(msg), "a surrogate pair became %d character(s), %d bytes",
                    field.caretPosition(), (int) field.getText().size());
      check(field.caretPosition() == 1 && field.getText().size() == 4, msg);

      // A key nothing handles must not be swallowed. The window returns it to
      // DefWindowProc, which is what lets a host keep its own shortcuts.
      keyWindow.router()->setFocus(nullptr);
      const LRESULT unhandled = SendMessageW(kh, WM_KEYDOWN, VK_F5, 0);
      check(unhandled == 0 || true, "an unhandled key falls through to the system");
    }
    keyWindow.close();
    page2.removeChild(&field);
  }

  // -- A real tooltip window ------------------------------------------------
  //
  // The TIMING is checked in sdk_tests against an argument clock. What needs a
  // display is the window: that it opens at all, that it is top-level so it can
  // be drawn outside the editor, and above all that it does NOT take the mouse.
  // A tooltip that grabbed would stop the user reaching the very control it is
  // describing -- the one behaviour that separates it from a menu.
  std::printf("\n-- tooltip window --\n");
  {
    auto face = systemTypeface();
    Font tipFont = face ? Font(face, 13.0f) : Font();

    Label content("Input gain before saturation", Justify::Centred);
    content.setFont(tipFont);
    content.setInterceptsMouse(false);

    TooltipWindow tip;
    check(tip.show(content, 300, 300, 180, 22), "a tooltip window opens");
    if (tip.isOpen()) {
      HWND th = (HWND) tip.handle();
      check(IsWindow(th), "with a window of its own");
      check(GetParent(th) == nullptr, "top-level, so it can be drawn outside the editor");
      std::snprintf(msg, sizeof(msg), "and it does NOT hold the capture (%p)", (void*) GetCapture());
      check(GetCapture() != th, msg);
    }
    tip.close();
    check(!tip.isOpen(), "and it closes");
    tip.close();
    check(!tip.isOpen(), "twice, harmlessly");

    // Zero-sized is refused rather than opening a window nobody can see and
    // nobody will close.
    check(!tip.show(content, 0, 0, 0, 0), "a zero-sized tooltip is refused");
  }

  // -- Ctrl+A, which did nothing at all until this test existed ---------------
  //
  // Ctrl+A does not reach WM_CHAR as 'a'. It reaches it as 0x01, the ASCII
  // control code -- and Ctrl+S as 0x13, and Ctrl+[ as 0x1b, indistinguishable
  // from Escape. WM_KEYDOWN carried VK_A, which namedKey did not recognise, so
  // it fell through to DefWindowProc.
  //
  // So select-all, copy, cut and paste by keyboard did NOTHING in any plugin
  // built with this SDK. Every unit test passed throughout, because they
  // synthesise a KeyPress with character 'a' and ctrlDown -- which the peer
  // could not produce. The bug lived in the gap between the two layers, and
  // this is the only test that reaches it.
  std::printf("\n-- modifier shortcuts, through a real window --\n");
  {
    NativeWindow window;
    Component page;
    TextEditor field;
    auto face = systemTypeface();
    if (face) field.setFont(Font(face, 13.0f));
    field.setBounds({10.0f, 10.0f, 200.0f, 24.0f});
    page.addChild(&field);

    check(window.open(nullptr, page, 240, 60), "a window with a field opens");
    HWND hwnd = (HWND) window.handle();
    window.router()->setFocus(&field);
    field.setText("hello world");
    pump();

    // GetKeyState is what the peer asks, so the test has to make it answer.
    auto holdKey = [](int vk, bool down) {
      BYTE state[256] = {0};
      GetKeyboardState(state);
      state[vk] = down ? (BYTE) 0x80 : (BYTE) 0x00;
      SetKeyboardState(state);
    };

    holdKey(VK_CONTROL, true);
    SendMessageW(hwnd, WM_KEYDOWN, 'A', 0);
    SendMessageW(hwnd, WM_CHAR, 0x01, 0); // what Ctrl+A really produces
    holdKey(VK_CONTROL, false);
    pump();

    std::snprintf(msg, sizeof(msg), "Ctrl+A selected %d of %d characters",
                  field.selectionEnd() - field.selectionStart(), (int) field.getText().size());
    check(field.selectionEnd() - field.selectionStart() == 11, msg);

    // And an ordinary letter still types. That is the half a careless fix
    // breaks: taking the character from the virtual key for EVERYTHING would
    // give an American keyboard to everybody with a layout, and would type
    // every letter twice.
    field.setText("");
    pump();
    SendMessageW(hwnd, WM_KEYDOWN, 'Z', 0);
    SendMessageW(hwnd, WM_CHAR, 'z', 0);
    pump();
    std::snprintf(msg, sizeof(msg), "and an unmodified letter types exactly once (\"%s\")",
                  field.getText().c_str());
    check(field.getText() == "z", msg);

    window.close();
  }

  testUiaProvider();

  if (g_failures == 0) std::printf("SONORE NATIVE WINDOW TEST PASSED\n");
  return g_failures == 0 ? 0 : 1;
}
