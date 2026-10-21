// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: what a web editor's page can and cannot do, on a real WebView2.
//
// The editor's page is the plugin's own, and it has to stay that way: the
// bridge is injected into whatever document the view shows, so a page that
// navigated to a remote site handed it window.sonore and the plugin took its
// parameter changes as the user's. And a page has to come up at all: past
// about 1.5 MB NavigateToString refused the page without an error anybody
// saw, and the editor was blank.
//
// Each case is a page that reports back through the bridge, the only witness
// that cannot be faked from outside.

#if defined(_WIN32)

#include <sonore/webview_win32.h>

#include <tlhelp32.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <malloc.h>
#include <new>
#include <string>
#include <vector>

// The heap this process holds, counted by the replaced operator new: what a
// closed editor keeps is a number of bytes, and this is where they are.
static std::atomic<long long> g_heapBytes{0};
void* operator new(size_t n) {
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  g_heapBytes += (long long) _msize(p);
  return p;
}
void* operator new[](size_t n) { return operator new(n); }
void operator delete(void* p) noexcept {
  if (!p) return;
  g_heapBytes -= (long long) _msize(p);
  std::free(p);
}
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete(void* p, size_t) noexcept { operator delete(p); }
void operator delete[](void* p, size_t) noexcept { operator delete(p); }

using namespace sonore;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

// The window standing in for the host's, and the keys the editor handed it:
// what a DAW's key bindings would have seen.
static HWND g_hostWindow = nullptr;
static std::vector<WPARAM> g_hostKeysDown;

static void pump(int ms) {
  const DWORD until = GetTickCount() + (DWORD) ms;
  while (GetTickCount() < until) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.hwnd == g_hostWindow && g_hostWindow && msg.message == WM_KEYDOWN)
        g_hostKeysDown.push_back(msg.wParam);
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    Sleep(5);
  }
}

/** WebView2 processes descending from this one: its browser is our child,
 *  the GPU process and renderers are the browser's. */
static int webviewProcesses() {
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return 0;
  struct Row {
    DWORD pid, parent;
    bool webview;
  };
  std::vector<Row> all;
  PROCESSENTRY32W e{};
  e.dwSize = sizeof(e);
  if (Process32FirstW(snap, &e)) {
    do {
      all.push_back({e.th32ProcessID, e.th32ParentProcessID, _wcsicmp(e.szExeFile, L"msedgewebview2.exe") == 0});
    } while (Process32NextW(snap, &e));
  }
  CloseHandle(snap);
  std::vector<DWORD> frontier{GetCurrentProcessId()};
  int count = 0;
  while (!frontier.empty()) {
    const DWORD parent = frontier.back();
    frontier.pop_back();
    for (const Row& row : all)
      if (row.parent == parent && row.pid != parent) {
        frontier.push_back(row.pid);
        if (row.webview) ++count;
      }
  }
  return count;
}

/** Windows of the backend's own class on this thread: editors, and the
 *  browser keep-alive's hidden one. */
static BOOL CALLBACK countOurWindow(HWND hwnd, LPARAM counter) {
  wchar_t name[128] = L"";
  GetClassNameW(hwnd, name, 128);
  if (std::wcsncmp(name, L"SonoreWebViewHost_", 18) == 0) ++*(int*) counter;
  return TRUE;
}

static int ourWindows() {
  int count = 0;
  EnumThreadWindows(GetCurrentThreadId(), countOurWindow, (LPARAM) &count);
  return count;
}

struct Outcome {
  bool created = false;
  int readies = 0;
  std::vector<double> sets;
  /** Which parameter each of `sets` was for. */
  std::vector<int> setIndices;
  /** Every captureKeys the page sent, 1 or 0, in order. */
  std::vector<int> captures;
  std::vector<WPARAM> hostKeys;
  int hostGot(WPARAM key) const {
    int n = 0;
    for (WPARAM k : hostKeys) n += k == key ? 1 : 0;
    return n;
  }
  bool saw(double v) const {
    for (double s : sets)
      if (s > v - 1e-6 && s < v + 1e-6) return true;
    return false;
  }
};

/** Open `html` in an editor for up to `ms`, or until the page has set `until`. */
// How long a case may take to say what it came to say. Each one stops waiting
// the moment it hears it, so a pass costs what it always did; the ceiling is
// only for a machine that is busy. It was 10 to 15 s, and with a full build
// and a parallel ctest beside it, a WebView2 that took longer to start failed
// a case that passes in two.
static constexpr int kPatience = 30000;

// A page that checks something and reports the verdict on the probe: one
// value for yes, the other for no, so a failure is heard as soon as a pass.
static constexpr double kYes = 0.9, kNo = 0.1;

/** The probe parameter alone, which is all most cases need. */
static const PluginDescriptor& probeDescriptor() {
  static ParamInfo params[1];
  static PluginDescriptor desc;
  params[0].id = "probe";
  params[0].label = "Probe";
  params[0].minValue = 0.0f;
  params[0].maxValue = 1.0f;
  desc.name = "Page Probe";
  desc.params = params;
  desc.numParams = 1;
  return desc;
}

static Outcome run(const std::string& html, int ms, double until,
                   const PluginDescriptor& desc = probeDescriptor(), double orUntil = -1.0,
                   const float* values = nullptr, const char* pluginId = "sonore-page-test") {
  Outcome out;
  HWND parent = CreateWindowExW(0, L"STATIC", L"webview page test", WS_OVERLAPPEDWINDOW, 0, 0,
                                760, 520, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  g_hostWindow = parent;
  g_hostKeysDown.clear();
  win32::WebViewHost host;
  host.onMessage = [&out](const BridgeMessage& m) {
    if (m.kind == BridgeMessage::Kind::Ready) ++out.readies;
    if (m.kind == BridgeMessage::Kind::Set) {
      out.sets.push_back(m.value);
      out.setIndices.push_back(m.index);
    }
    if (m.kind == BridgeMessage::Kind::CaptureKeys) out.captures.push_back(m.value >= 0.5 ? 1 : 0);
  };
  out.created = host.create(parent, 700, 420, html, bridgeScript(desc, values), pluginId);
  if (out.created) {
    const DWORD deadline = GetTickCount() + (DWORD) ms;
    while (GetTickCount() < deadline && !out.saw(until) && !(orUntil >= 0.0 && out.saw(orUntil)))
      pump(20);
    pump(200);
  }
  host.destroy();
  out.hostKeys = g_hostKeysDown;
  g_hostWindow = nullptr;
  DestroyWindow(parent);
  pump(100);
  return out;
}

static std::string page(const std::string& onReady, const std::string& padding = std::string()) {
  return "<!doctype html><html><head><meta charset='utf-8'><title>probe</title></head><body>"
         "<!--" + padding + "-->"
         "<script>window.addEventListener('sonore:ready',function(){" + onReady +
         "});</script></body></html>";
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("── a web editor's page ──\n");

  // The runtime may be absent on a bare machine; the other GUI tests say so
  // and move on, and so does this one.
  const Outcome probe = run(page("sonore.set(0,0.5);"), kPatience, 0.5);
  if (!probe.created || probe.readies == 0) {
    std::printf("  ---- no WebView2 runtime here: not exercised ----\n");
    return 0;
  }
  check(probe.saw(0.5), "a page comes up and talks through the bridge");

  // A page that tries to leave. Cancelled, it is still there 1.5 s later to
  // say so; followed, its document would have been replaced by the error page
  // of an address that cannot resolve, and the timer with it.
  {
    const Outcome o = run(page("sonore.set(0,0.1);"
                               "setTimeout(function(){location.href='https://sonore-test.invalid/';},50);"
                               "setTimeout(function(){sonore.set(0,0.2);},1500);"),
                          kPatience, 0.2);
    check(o.saw(0.2) && o.readies == 1,
          "a page that navigates to a remote address stays where it is");
  }

  // window.open, which on WebView2 opened a top-level browser window that
  // outlived the editor and the process.
  {
    const Outcome o = run(page("var w=window.open('about:blank');sonore.set(0,w?0.9:0.3);"), kPatience,
                          0.3);
    check(o.saw(0.3) && !o.saw(0.9), "window.open opens nothing and returns null");
  }

  // A remote iframe: refused before it loads, so nothing in it can reach the
  // bridge. The page still comes up around it.
  {
    const Outcome o = run(page("var f=document.createElement('iframe');"
                               "f.src='https://sonore-test.invalid/frame.html';"
                               "document.body.appendChild(f);"
                               "setTimeout(function(){sonore.set(0,0.4);},500);"),
                          kPatience, 0.4);
    check(o.saw(0.4), "a page with a remote iframe in it still comes up");
  }

  // Past what NavigateToString takes: a page carrying an inlined filmstrip or
  // font. 2.5 MB, well past the ~1.5 MB where it was refused.
  {
    const Outcome o = run(page("sonore.set(0,0.6);", std::string(2500000, 'x')), kPatience, 0.6);
    check(o.saw(0.6), "a 2.5 MB page comes up and talks through the bridge");
  }
  {
    const Outcome o =
        run(page("sonore.set(0,0.1);"
                 "setTimeout(function(){location.href='https://sonore-test.invalid/';},50);"
                 "setTimeout(function(){sonore.set(0,0.7);},1500);",
                 std::string(2500000, 'x')),
            kPatience, 0.7);
    check(o.saw(0.7) && o.readies == 1, "…and it cannot navigate away either");
  }

  // A key the page handles is the page's. The bridge passes the rest to the
  // host -- the spacebar is transport -- unless the page called
  // preventDefault. A page playing notes from the computer keyboard in a
  // WINDOW keydown listener had every key it took sent to the host as well:
  // the bridge's own window listener was added first, so it ran first and
  // found nothing prevented. The same handler on document was fine.
  {
    const Outcome o = run(
        page("['a','b','c'].forEach(function(k){document.body.dispatchEvent(new KeyboardEvent("
             "'keydown',{key:k,keyCode:k.toUpperCase().charCodeAt(0),bubbles:true,cancelable:true}));});"
             "setTimeout(function(){sonore.set(0,0.8);},300);") +
            "<script>window.addEventListener('keydown',function(e){if(e.key==='a')e.preventDefault();});"
            "document.addEventListener('keydown',function(e){if(e.key==='c')e.preventDefault();});"
            "</script>",
        kPatience, 0.8);
    char line[200];
    std::snprintf(line, sizeof(line),
                  "a key the page took in a window listener stays the page's (host got %d; "
                  "a key it left: %d, want 1; one it took on document: %d)",
                  o.hostGot('A'), o.hostGot('B'), o.hostGot('C'));
    check(o.saw(0.8) && o.hostGot('A') == 0 && o.hostGot('B') == 1 && o.hostGot('C') == 0, line);
  }

  // A text field keeps its keys; a slider does not type. The bridge took
  // every <input> for a text field, so once the user had touched an
  // <input type=range> -- every slider of the generated fallback page, and
  // of many a designed one -- the spacebar stopped at the page until focus
  // moved: tweak a knob, press play, nothing.
  {
    const std::string html =
        "<!doctype html><html><body><input id=s type=range><input id=c type=checkbox>"
        "<input id=t type=text><script>"
        "function press(id,k){var e=document.getElementById(id);e.focus();"
        "e.dispatchEvent(new KeyboardEvent('keydown',{key:k,keyCode:k.toUpperCase().charCodeAt(0),"
        "bubbles:true,cancelable:true}));}"
        "window.addEventListener('sonore:ready',function(){press('s',' ');press('c','q');"
        "press('t','w');setTimeout(function(){sonore.set(0,0.65);},300);});"
        "</script></body></html>";
    const Outcome o = run(html, kPatience, 0.65);
    char line[200];
    std::snprintf(line, sizeof(line),
                  "a slider or a checkbox with focus passes the spacebar on; a text field keeps "
                  "its keys (host got space %d, Q %d, W %d; want 1, 1, 0)",
                  o.hostGot(VK_SPACE), o.hostGot('Q'), o.hostGot('W'));
    check(o.saw(0.65) && o.hostGot(VK_SPACE) == 1 && o.hostGot('Q') == 1 && o.hostGot('W') == 0,
          line);
  }

  // ...and a text field keeps ALL its keys while it has focus, the page
  // having said nothing. The native side forwards Delete, the arrows, Tab
  // and every Ctrl shortcut to the host unless the keyboard was asked for,
  // and only a page that knew to call captureKeys asked: in any other text
  // field Delete deleted the DAW's selection, and Ctrl+V pasted nothing --
  // the licence overlay's activation code field among them. Real keys
  // cannot be sent from here; what is checked is that the bridge asks for
  // the keyboard while a text field has focus and gives it back after. The
  // test's window never has the system focus, so the browser moves the
  // focus without announcing it, and the page announces it itself.
  {
    const std::string html =
        "<!doctype html><html><body><input id=t type=text><input id=s type=range><script>"
        "function move(id){var e=document.getElementById(id);e.focus();"
        "e.dispatchEvent(new FocusEvent('focusin',{bubbles:true}));}"
        "window.addEventListener('sonore:ready',function(){move('t');"
        "setTimeout(function(){move('s');setTimeout(function(){sonore.set(0,0.45);},100);},100);});"
        "</script></body></html>";
    const Outcome o = run(html, kPatience, 0.45);
    std::string seen;
    for (int c : o.captures) seen += c ? '1' : '0';
    // A "give it back" before anything was taken is harmless; what matters is
    // one ask for the field, then one give-back for the slider.
    const size_t first = seen.find('1');
    char line[200];
    std::snprintf(line, sizeof(line),
                  "a focused text field has the keyboard and a slider gives it back (asked: '%s', "
                  "want '10')",
                  seen.c_str());
    check(o.saw(0.45) && first != std::string::npos && seen.substr(first) == "10" &&
              seen.find_first_not_of('0') == first,
          line);
  }

  // sonore.format is the host's readout in the page's hands, and plugin.h's
  // formatParamValue is the host's: the two must print the same text. A VST3
  // host keeps a position in single precision, so REAPER hands 0 dB of a
  // -60..12 dB control back as 0.83333331 -- -0.0000014 dB -- which the host
  // shows as "0.0 dB" and the page showed as "-0.000 dB".
  {
    static ParamInfo params[2];
    params[0] = probeDescriptor().params[0];
    params[1].id = "gain";
    params[1].label = "Gain";
    params[1].unit = "dB";
    params[1].minValue = -60.0f;
    params[1].maxValue = 12.0f;
    params[1].defaultValue = 0.0f;
    PluginDescriptor desc = probeDescriptor();
    desc.params = params;
    desc.numParams = 2;
    const float zero = clampToRange(params[1], (float) toPlainValue(params[1], (double) 0.83333331f));
    char value[40], text[64];
    jsNumber(value, sizeof(value), (double) zero);
    formatParamValue(params[1], zero, text, sizeof(text));
    const Outcome o = run(page(std::string("sonore.set('probe',sonore.format('gain',") + value +
                               ")==='" + escapeForJs(text) + "'?0.9:0.1);"),
                          kPatience, kYes, desc, kNo);
    char line[200];
    std::snprintf(line, sizeof(line),
                  "the page prints a value as the host does (%s, which the host shows as \"%s\")",
                  value, text);
    check(o.saw(kYes), line);
  }

  // A page's first frame shows the values the plugin has NOW. It draws from
  // the table in its 'sonore:ready' handler, before the editor's tick has
  // pushed anything, and a table holding the defaults drew every control at
  // its default for that frame and up to 33 ms after it. Nothing here ticks,
  // so what the page reads is only what the bridge was built with.
  {
    static const float now[1] = {0.25f};
    const Outcome o = run(page("var p=sonore.params[0];"
                               "sonore.set(0,(p.value===0.25&&sonore.get('probe')===0.25&&p.default===0)"
                               "?0.9:0.1);"),
                          kPatience, kYes, probeDescriptor(), kNo, now);
    check(o.saw(kYes), "a page opens on the values the plugin has now, not on its defaults");
  }

  // A stepped control on the page sits where the plugin puts it. The plugin
  // snaps a value to its step (clampToRange) and spreads the steps evenly
  // whatever the skew (toNormalisedValue); the page did neither, so a
  // switch set to 0.4 read 0.4 in sonore.get() while the plugin was Off,
  // and a skewed stepped knob pointed elsewhere than the host's lane.
  {
    static const char* modeNames[] = {"Off", "On"};
    static ParamInfo params[3];
    params[0] = probeDescriptor().params[0];
    params[1].id = "ratio";
    params[1].label = "Ratio";
    params[1].minValue = 1.0f;
    params[1].maxValue = 20.0f;
    params[1].defaultValue = 1.0f;
    params[1].stepCount = 20;
    params[1].skew = 0.5f;
    params[2].id = "mode";
    params[2].label = "Mode";
    params[2].minValue = 0.0f;
    params[2].maxValue = 1.0f;
    params[2].stepCount = 2;
    params[2].valueNames = modeNames;
    params[2].numValueNames = 2;
    PluginDescriptor desc = probeDescriptor();
    desc.params = params;
    desc.numParams = 3;
    char where[40], mode[40];
    jsNumber(where, sizeof(where), toNormalisedValue(params[1], 5.0));
    jsNumber(mode, sizeof(mode), (double) clampToRange(params[2], 0.4f));
    const Outcome o = run(
        page(std::string("var a=sonore.toNormalised('ratio',5),b=sonore.fromNormalised('ratio',") +
             where + ");sonore.set('mode',0.4);"
             "sonore.set('probe',Math.abs(a-" + where + ")<1e-6&&Math.abs(b-5)<1e-4&&"
             "sonore.get('mode')===" + mode + "?0.9:0.1);"),
        kPatience, kYes, desc, kNo);
    double sent = -1.0;
    for (size_t i = 0; i < o.sets.size(); ++i)
      if (o.setIndices[i] == 2) sent = o.sets[i];
    char line[240];
    std::snprintf(line, sizeof(line),
                  "a stepped control on the page is where the plugin has it (ratio 5 at %s, the "
                  "host's position; mode set to 0.4 reads and sends %s; sent %g)",
                  where, mode, sent);
    check(o.saw(kYes) && sent == (double) clampToRange(params[2], 0.4f), line);
  }

  // The page follows its window, however the window was resized. An LV2
  // host has no call to resize a ui:WindowsUI and resizes the widget's
  // window itself; the backend laid the webview out at the size it was last
  // TOLD, so the page stayed 700 wide in a 1000-wide window, with a dark
  // band beside it. Shown, because a webview nobody can see does not lay
  // its page out again.
  {
    HWND parent = CreateWindowExW(0, L"STATIC", L"webview resize test",
                                  WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 1400, 900, nullptr, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
    win32::WebViewHost host;
    std::vector<double> widths;
    host.onMessage = [&widths](const BridgeMessage& m) {
      if (m.kind == BridgeMessage::Kind::Set) widths.push_back(m.value * 2000.0);
    };
    const std::string html =
        "<!doctype html><html><body><script>"
        "function tell(){sonore.set(0,window.innerWidth/2000);}"
        "window.addEventListener('resize',tell);window.addEventListener('sonore:ready',tell);"
        "</script></body></html>";
    double before = -1.0, after = -1.0;
    if (host.create(parent, 700, 420, html, bridgeScript(probeDescriptor()), "sonore-page-test")) {
      DWORD deadline = GetTickCount() + (DWORD) kPatience;
      while (widths.empty() && GetTickCount() < deadline) pump(20);
      pump(200);
      if (!widths.empty()) before = widths.back();
      SetWindowPos(host.hwnd(), nullptr, 0, 0, 1000, 600, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
      deadline = GetTickCount() + 5000;
      while (GetTickCount() < deadline && !(widths.back() > 999.0 && widths.back() < 1001.0))
        pump(20);
      after = widths.empty() ? -1.0 : widths.back();
    }
    host.destroy();
    DestroyWindow(parent);
    pump(100);
    char line[200];
    std::snprintf(line, sizeof(line),
                  "a page follows its window when the host resizes the window itself (%.0f px, "
                  "then %.0f in a 1000 px window)",
                  before, after);
    check(before > 699.0 && before < 701.0 && after > 999.0 && after < 1001.0, line);
  }

  // A served page that reloads itself comes back. The editor lets go of its
  // copy of a page once NavigateToString has it, but a served page keeps
  // its copy while the editor is open, because a reload asks for it again.
  // The bridge comes back with it: WebView2 keeps the script and runs it on
  // each new document, so the editor's own copy goes once it is handed over.
  {
    const Outcome o = run(page("var n=performance.getEntriesByType('navigation')[0];"
                               "if(n&&n.type==='reload'){sonore.set(0,0.85);}"
                               "else{sonore.set(0,0.15);location.reload();}",
                               std::string(2500000, 'x')),
                          kPatience, 0.85);
    char line[200];
    std::snprintf(line, sizeof(line),
                  "a 2.5 MB served page that reloads itself comes back, bridge and all (before %d, "
                  "after %d, %d readies)",
                  o.saw(0.15) ? 1 : 0, o.saw(0.85) ? 1 : 0, o.readies);
    check(o.saw(0.15) && o.saw(0.85) && o.readies == 2, line);
  }

  // A page of ordinary size that reloads itself comes back as well. Up to
  // 1 MiB the page went by NavigateToString, and a reload of that had
  // nothing to reload from: the editor came back blank.
  {
    const Outcome o = run(page("var n=performance.getEntriesByType('navigation')[0];"
                               "if(n&&n.type==='reload'){sonore.set(0,0.85);}"
                               "else{sonore.set(0,0.15);location.reload();}"),
                          kPatience, 0.85);
    char line[200];
    std::snprintf(line, sizeof(line),
                  "a page of ordinary size that reloads itself comes back (before %d, after %d, "
                  "%d readies)",
                  o.saw(0.15) ? 1 : 0, o.saw(0.85) ? 1 : 0, o.readies);
    check(o.saw(0.15) && o.saw(0.85) && o.readies == 2, line);
  }

  // A page has an origin, its plugin's own. NavigateToString gave it none:
  // localStorage threw. Served, it works, what a page stores is there at the
  // next open of the same plugin, and another plugin -- every Sonore editor
  // shares one user-data folder -- does not see it. The token is new each
  // run, and taken away again at the end.
  {
    const std::string token = std::to_string(GetTickCount64() * 31u + GetCurrentProcessId());
    const std::string key = "'sonore-page-test-token'";
    const Outcome wrote =
        run(page("try{localStorage.setItem(" + key + ",'" + token + "');sonore.set(0,0.21);}"
                 "catch(e){sonore.set(0,0.29);}"),
            kPatience, 0.21, probeDescriptor(), 0.29);
    const std::string read = "var v=null;try{v=localStorage.getItem(" + key + ");}catch(e){}"
                             "sonore.set(0,v==='" + token + "'?0.31:0.39);";
    const Outcome other =
        run(page(read), kPatience, 0.31, probeDescriptor(), 0.39, nullptr, "sonore-page-test-other");
    const Outcome same =
        run(page(read + "try{localStorage.removeItem(" + key + ");}catch(e){}"), kPatience, 0.31,
            probeDescriptor(), 0.39);
    check(wrote.saw(0.21), "a page's localStorage works");
    check(same.saw(0.31), "...what it stored is there at the plugin's next open");
    check(other.saw(0.39) && !other.saw(0.31), "...and another plugin's page does not see it");
  }

  // What a closed editor keeps. The WebViewHost is a member of the plugin
  // instance and outlives every editor it shows, and it kept its copy of the
  // page and of the bridge until the plugin was removed: 2.5 MB here, in
  // each instance whose editor had ever been opened.
  {
    HWND parent = CreateWindowExW(0, L"STATIC", L"webview page test", WS_OVERLAPPEDWINDOW, 0, 0, 760,
                                  520, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    win32::WebViewHost host;
    bool shown = false;
    host.onMessage = [&shown](const BridgeMessage& m) {
      if (m.kind == BridgeMessage::Kind::Set && m.value > 0.549 && m.value < 0.551) shown = true;
    };
    const long long before = g_heapBytes.load();
    if (host.create(parent, 700, 420, page("sonore.set(0,0.55);", std::string(2500000, 'x')),
                    bridgeScript(probeDescriptor()), "sonore-page-test")) {
      const DWORD deadline = GetTickCount() + (DWORD) kPatience;
      while (!shown && GetTickCount() < deadline) pump(20);
      pump(200);
    }
    host.destroy();
    pump(200);
    const long long kept = g_heapBytes.load() - before;
    DestroyWindow(parent);
    pump(100);
    char line[200];
    std::snprintf(line, sizeof(line), "a closed editor keeps no copy of its 2.5 MB page (%lld bytes kept)",
                  kept);
    check(shown && kept < 256 * 1024, line);
  }

  // The browser outlives the editor, for the next one. WebView2 shut its
  // whole process tree down a quarter of a second after the last webview
  // closed, so every reopen started Edge again: ~410 ms to a page showing
  // its values instead of ~120. A host's plugin instance outlives its
  // editors, and while one does the processes stay for the next open --
  // and once the last WebViewHost is gone, nothing of the keep-alive is
  // left behind, since the module may be unloaded next.
  // Which browser is asked about, not how many of this process's children
  // run: every Sonore editor shares one user-data folder, so when a test that
  // ran just before left its browser shutting down, this one joined that
  // browser -- not a child of this process -- and a count of children read 0
  // with the browser held and running.
  {
    double reopenedAt = -1.0;
    int windowsHeld = 0, ours = 0;
    DWORD browser = 0;
    bool browserRunning = false;
    {
      win32::WebViewHost instance; // never opened: the plugin instance holding the editor
      const Outcome first = run(page("sonore.set(0,0.4);"), kPatience, 0.4);
      pump(1000);
      browser = win32::detail::BrowserKeepAlive::browserProcessId();
      if (HANDLE h = browser ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, browser) : nullptr) {
        DWORD code = 0;
        browserRunning = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
        CloseHandle(h);
      }
      windowsHeld = ourWindows();
      ours = webviewProcesses();
      const Outcome again = run(page("sonore.set(0,0.6);"), kPatience, 0.6);
      reopenedAt = again.saw(0.6) && first.saw(0.4) ? 1.0 : -1.0;
    }
    pump(200);
    char line[200];
    std::snprintf(line, sizeof(line),
                  "a second after the last editor closed, WebView2 is still running for the next "
                  "(browser %lu %s, %d window held, %d processes of ours)",
                  (unsigned long) browser, browserRunning ? "running" : "not running", windowsHeld, ours);
    check(browserRunning && windowsHeld == 1, line);
    check(reopenedAt > 0.0, "...and the next editor comes up on it");
    std::snprintf(line, sizeof(line),
                  "nothing of it is left once the last WebViewHost is gone (%d windows)", ourWindows());
    check(ourWindows() == 0, line);
  }

  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  if (g_failures == 0) std::printf("SONORE WEBVIEW PAGE PASSED\n");
  return g_failures == 0 ? 0 : 1;
}

#else
int main() { return 0; }
#endif
