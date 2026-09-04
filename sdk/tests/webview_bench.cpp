// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: measure what the web editors actually COST.
//
// The number everyone repeats, "a webview is 100-300 MB per instance", was
// folklore in this project's own notes until this file existed. The questions
// a decision needs are sharper than that: what does the FIRST editor cost
// (browser + GPU + renderer processes all spawn), what does each MARGINAL one
// cost, do two DIFFERENT plugins share the browser process tree or pay for two
// of everything, and what does hiding an editor give back?
//
// So this drives the real thing: it loads built .clap files exactly as a host
// does, opens N editors across them, and after every step sums the PRIVATE
// working memory of this process plus every descendant, which is where
// WebView2's msedgewebview2.exe tree lives. Private bytes, not working set,
// because shared pages counted N times would flatter nothing and mislead
// everyone.
//
//   webview_bench <a.clap> [b.clap] [count]
//
// With two plugins the editors alternate between them, which is what makes the
// sharing question measurable: per-plugin user-data folders mean two browser
// trees, a shared folder means one, and the "settled" breakdown at the end
// names every process so you can see which happened.
//
// Deliberately NOT a ctest, same rule as simd_bench: memory numbers on shared
// CI hardware are noise with a pass mark. This is an instrument, run by hand,
// before and after a change.
#include <clap/clap.h>

#include <windows.h>

#include <psapi.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static void* symbolOf(HMODULE h, const char* name) { return (void*) GetProcAddress(h, name); }

static void hostRequestRestart(const clap_host_t*) {}
static void hostRequestProcess(const clap_host_t*) {}
static void hostRequestCallback(const clap_host_t*) {}
static const void* hostGetExtension(const clap_host_t*, const char*) { return nullptr; }

static void pump(int ms) {
  MSG msg;
  const DWORD until = GetTickCount() + (DWORD) ms;
  while (GetTickCount() < until) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    Sleep(5);
  }
}

struct ProcSample {
  DWORD pid = 0;
  std::string name;
  SIZE_T privateBytes = 0;
};

/**
 * This process and every descendant, with private commit per process.
 *
 * The tree walk is the load-bearing part: WebView2 spawns a browser process as
 * OUR child, and the GPU process and renderers as ITS children. Sampling only
 * ourselves would measure the loader stub and miss the browser entirely, which
 * is how webview memory stays folklore.
 */
static std::vector<ProcSample> processTree() {
  std::vector<ProcSample> out;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return out;

  struct Row {
    DWORD pid, ppid;
    std::string name;
  };
  std::vector<Row> all;
  PROCESSENTRY32W e{};
  e.dwSize = sizeof(e);
  if (Process32FirstW(snap, &e)) {
    do {
      char name[MAX_PATH]{};
      WideCharToMultiByte(CP_UTF8, 0, e.szExeFile, -1, name, sizeof(name) - 1, nullptr, nullptr);
      all.push_back({e.th32ProcessID, e.th32ParentProcessID, name});
    } while (Process32NextW(snap, &e));
  }
  CloseHandle(snap);

  // Breadth-first from us. PID reuse could in principle fake an edge; for a
  // bench that runs for seconds the risk is noise, not a lie.
  std::vector<DWORD> frontier{GetCurrentProcessId()};
  std::vector<DWORD> members;
  while (!frontier.empty()) {
    const DWORD parent = frontier.back();
    frontier.pop_back();
    members.push_back(parent);
    for (const Row& row : all)
      if (row.ppid == parent && row.pid != parent) frontier.push_back(row.pid);
  }

  for (const DWORD pid : members) {
    ProcSample s;
    s.pid = pid;
    for (const Row& row : all)
      if (row.pid == pid) s.name = row.name;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h) {
      PROCESS_MEMORY_COUNTERS_EX pmc{};
      if (GetProcessMemoryInfo(h, (PROCESS_MEMORY_COUNTERS*) &pmc, sizeof(pmc)))
        s.privateBytes = pmc.PrivateUsage;
      CloseHandle(h);
    }
    out.push_back(s);
  }
  return out;
}

static double totalMb(const std::vector<ProcSample>& t) {
  SIZE_T sum = 0;
  for (const ProcSample& s : t) sum += s.privateBytes;
  return (double) sum / (1024.0 * 1024.0);
}

static double gLast = 0.0;

/**
 * Did the window actually PAINT something?
 *
 * The park cycle destroys the webview and rebuilds it on show, and every
 * memory number in this file would look identical if the rebuild produced a
 * permanently blank editor. Counting distinct colours in a composited capture
 * is the cheap honest check: a dead webview is one colour, a painted
 * faceplate is many.
 */
static int distinctColours(HWND hwnd) {
  RECT rc{};
  if (!GetClientRect(hwnd, &rc)) return 0;
  const int w = rc.right - rc.left, h = rc.bottom - rc.top;
  if (w <= 0 || h <= 0) return 0;
  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
  HGDIOBJ old = SelectObject(mem, bmp);
  if (!PrintWindow(hwnd, mem, 2 /* PW_RENDERFULLCONTENT */))
    BitBlt(mem, 0, 0, w, h, screen, 0, 0, SRCCOPY);
  BITMAPINFOHEADER bi{};
  bi.biSize = sizeof(bi);
  bi.biWidth = w;
  bi.biHeight = -h;
  bi.biPlanes = 1;
  bi.biBitCount = 32;
  bi.biCompression = BI_RGB;
  std::vector<unsigned char> px((size_t) w * h * 4);
  GetDIBits(mem, bmp, 0, (UINT) h, px.data(), (BITMAPINFO*) &bi, DIB_RGB_COLORS);
  SelectObject(mem, old);
  DeleteObject(bmp);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);

  std::vector<unsigned int> seen;
  for (size_t i = 0; i + 3 < px.size(); i += 4 * 97) { // sparse sample is plenty
    const unsigned int c =
        (unsigned int) px[i] | ((unsigned int) px[i + 1] << 8) | ((unsigned int) px[i + 2] << 16);
    bool known = false;
    for (unsigned int k : seen)
      if (k == c) known = true;
    if (!known) seen.push_back(c);
    if (seen.size() > 64) break;
  }
  return (int) seen.size();
}

static void sample(const char* label) {
  const auto tree = processTree();
  const double mb = totalMb(tree);
  std::printf("  %-34s %3zu processes  %8.1f MB  (%+7.1f)\n", label, tree.size(), mb,
              mb - gLast);
  gLast = mb;
}

static void breakdown(const char* label) {
  std::printf("  ---- %s, by process ----\n", label);
  for (const auto& s : processTree())
    std::printf("    %-28s pid %-7lu %8.1f MB\n", s.name.c_str(), (unsigned long) s.pid,
                (double) s.privateBytes / (1024.0 * 1024.0));
}

struct LoadedPlugin {
  HMODULE lib = nullptr;
  const clap_plugin_entry_t* entry = nullptr;
  const clap_plugin_factory_t* factory = nullptr;
  const clap_plugin_descriptor_t* desc = nullptr;
};

struct Editor {
  const clap_plugin_t* plugin = nullptr;
  const clap_plugin_gui_t* gui = nullptr;
  HWND parent = nullptr;
};

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: webview_bench <a.clap> [b.clap] [count]\n");
    return 2;
  }
  std::vector<const char*> paths;
  int count = 6;
  for (int i = 1; i < argc; ++i) {
    if (std::strstr(argv[i], ".clap")) paths.push_back(argv[i]);
    else count = std::atoi(argv[i]);
  }
  if (count < 1) count = 1;

  static clap_host_t host{};
  host.clap_version = CLAP_VERSION;
  host.name = "Sonore WebView Bench";
  host.vendor = "Sonorie";
  host.url = "";
  host.version = "1.0.0";
  host.get_extension = hostGetExtension;
  host.request_restart = hostRequestRestart;
  host.request_process = hostRequestProcess;
  host.request_callback = hostRequestCallback;

  std::vector<LoadedPlugin> libs;
  for (const char* path : paths) {
    LoadedPlugin lp;
    lp.lib = LoadLibraryA(path);
    if (!lp.lib) {
      std::printf("could not load %s\n", path);
      return 1;
    }
    lp.entry = static_cast<const clap_plugin_entry_t*>(symbolOf(lp.lib, "clap_entry"));
    if (!lp.entry || !lp.entry->init(path)) {
      std::printf("no clap_entry in %s\n", path);
      return 1;
    }
    lp.factory =
        static_cast<const clap_plugin_factory_t*>(lp.entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    lp.desc = lp.factory->get_plugin_descriptor(lp.factory, 0);
    libs.push_back(lp);
  }

  std::printf("\n[webview bench] %d editors across %zu plugin(s)\n\n", count, libs.size());
  sample("baseline (plugins loaded, no gui)");

  std::vector<Editor> editors;
  for (int i = 0; i < count; ++i) {
    const LoadedPlugin& lp = libs[(size_t) i % libs.size()];
    Editor ed;
    ed.plugin = lp.factory->create_plugin(lp.factory, &host, lp.desc->id);
    if (!ed.plugin || !ed.plugin->init(ed.plugin)) {
      std::printf("could not create instance %d\n", i);
      return 1;
    }
    ed.plugin->activate(ed.plugin, 48000.0, 1, 512);
    ed.gui = static_cast<const clap_plugin_gui_t*>(
        ed.plugin->get_extension(ed.plugin, CLAP_EXT_GUI));
    if (!ed.gui || !ed.gui->create(ed.plugin, CLAP_WINDOW_API_WIN32, false)) {
      std::printf("instance %d has no embeddable gui\n", i);
      return 1;
    }
    uint32_t w = 700, h = 420;
    ed.gui->get_size(ed.plugin, &w, &h);
    RECT want{0, 0, (LONG) w, (LONG) h};
    AdjustWindowRect(&want, WS_OVERLAPPEDWINDOW, FALSE);
    ed.parent = CreateWindowExW(0, L"STATIC", L"Sonore bench", WS_OVERLAPPEDWINDOW,
                                40 + 30 * i, 40 + 30 * i, want.right - want.left,
                                want.bottom - want.top, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    ShowWindow(ed.parent, SW_SHOWNOACTIVATE);
    clap_window_t window{};
    window.api = CLAP_WINDOW_API_WIN32;
    window.win32 = ed.parent;
    if (!ed.gui->set_parent(ed.plugin, &window)) {
      std::printf("set_parent failed on instance %d\n", i);
      return 1;
    }
    ed.gui->show(ed.plugin);
    editors.push_back(ed);

    // The first editor pays for the whole browser tree spawning; give it
    // longer than the rest so the sample is the settled cost, not a race.
    pump(i == 0 ? 4000 : 2500);
    char label[96];
    std::snprintf(label, sizeof(label), "editor %d shown (%s)", i + 1, lp.desc->name);
    sample(label);
  }

  pump(3000);
  sample("settled");
  breakdown("settled");

  for (const Editor& ed : editors) ed.gui->hide(ed.plugin);
  // Long enough for suspend AND, when SONORE_WEBVIEW_PARK_MS is short enough,
  // for the park to fire -- the grace clock runs on the webview's own tick.
  pump(6000);
  sample("all hidden");
  breakdown("hidden");

  for (const Editor& ed : editors) ed.gui->show(ed.plugin);
  // A parked editor REBUILDS its webview here, asynchronously like any first
  // open, so the pump is generous.
  pump(5000);
  sample("all shown again");
  {
    const int colours = distinctColours(editors[0].parent);
    std::printf("  editor 1 paints %d distinct colours after the hide/show cycle%s\n", colours,
                colours >= 8 ? "" : "  <-- BLANK: the rebuild did not paint");
  }

  for (const Editor& ed : editors) {
    ed.gui->destroy(ed.plugin);
    DestroyWindow(ed.parent);
    ed.plugin->deactivate(ed.plugin);
    ed.plugin->destroy(ed.plugin);
  }
  pump(3000);
  sample("all editors destroyed");

  for (const LoadedPlugin& lp : libs) {
    lp.entry->deinit();
    FreeLibrary(lp.lib);
  }
  std::printf("\ndone.\n");
  return 0;
}
