// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: photograph a plugin's real interface.
//
// Loads a built .clap, embeds its GUI in a window exactly as a host does, pumps
// the message loop until the webview has painted, and writes a BMP of the
// window's client area. The lint and the bridge test prove the UI *works*; this
// is how a human confirms it does not look broken.
//
//   gui_shot <path-to.clap> <out.bmp> [seconds]
#include <clap/clap.h>

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static void* symbolOf(HMODULE h, const char* name) { return (void*) GetProcAddress(h, name); }

static void hostRequestRestart(const clap_host_t*) {}
static void hostRequestProcess(const clap_host_t*) {}
static void hostRequestCallback(const clap_host_t*) {}
static const void* hostGetExtension(const clap_host_t*, const char*) { return nullptr; }

/** Save a device-independent bitmap of a window's client area. */
static bool captureWindow(HWND hwnd, const char* path) {
  RECT rc{};
  if (!GetClientRect(hwnd, &rc)) return false;
  const int w = rc.right - rc.left, h = rc.bottom - rc.top;
  if (w <= 0 || h <= 0) return false;

  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
  HGDIOBJ old = SelectObject(mem, bmp);

  // PW_RENDERFULLCONTENT is what captures a composited child like a webview;
  // a plain BitBlt of the desktop would photograph whatever is on top instead.
  if (!PrintWindow(hwnd, mem, 2 /* PW_RENDERFULLCONTENT */)) {
    BitBlt(mem, 0, 0, w, h, screen, 0, 0, SRCCOPY);
  }

  BITMAPINFOHEADER bi{};
  bi.biSize = sizeof(bi);
  bi.biWidth = w;
  bi.biHeight = -h; // top-down
  bi.biPlanes = 1;
  bi.biBitCount = 32;
  bi.biCompression = BI_RGB;

  std::vector<unsigned char> pixels((size_t) w * h * 4);
  GetDIBits(mem, bmp, 0, (UINT) h, pixels.data(), (BITMAPINFO*) &bi, DIB_RGB_COLORS);

  BITMAPFILEHEADER fh{};
  fh.bfType = 0x4D42;
  fh.bfOffBits = sizeof(fh) + sizeof(bi);
  fh.bfSize = fh.bfOffBits + (DWORD) pixels.size();

  FILE* f = std::fopen(path, "wb");
  bool ok = false;
  if (f) {
    std::fwrite(&fh, sizeof(fh), 1, f);
    std::fwrite(&bi, sizeof(bi), 1, f);
    std::fwrite(pixels.data(), 1, pixels.size(), f);
    std::fclose(f);
    ok = true;
  }

  SelectObject(mem, old);
  DeleteObject(bmp);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  return ok;
}

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

int main(int argc, char** argv) {
  if (argc < 3) {
    std::printf("usage: gui_shot <path-to.clap> <out.bmp> [seconds]\n");
    return 2;
  }
  const char* pluginPath = argv[1];
  const char* outPath = argv[2];
  const int seconds = argc > 3 ? std::atoi(argv[3]) : 4;

  HMODULE lib = LoadLibraryA(pluginPath);
  if (!lib) {
    std::printf("could not load %s\n", pluginPath);
    return 1;
  }
  const auto* entry = static_cast<const clap_plugin_entry_t*>(symbolOf(lib, "clap_entry"));
  if (!entry || !entry->init(pluginPath)) {
    std::printf("no clap_entry\n");
    return 1;
  }
  const auto* factory =
      static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
  const clap_plugin_descriptor_t* desc = factory->get_plugin_descriptor(factory, 0);

  clap_host_t host{};
  host.clap_version = CLAP_VERSION;
  host.name = "Sonore GUI Shot";
  host.vendor = "Sonorie";
  host.url = "";
  host.version = "1.0.0";
  host.get_extension = hostGetExtension;
  host.request_restart = hostRequestRestart;
  host.request_process = hostRequestProcess;
  host.request_callback = hostRequestCallback;

  const clap_plugin_t* plugin = factory->create_plugin(factory, &host, desc->id);
  if (!plugin || !plugin->init(plugin)) {
    std::printf("could not create the plugin\n");
    return 1;
  }
  plugin->activate(plugin, 48000.0, 1, 512);

  const auto* gui =
      static_cast<const clap_plugin_gui_t*>(plugin->get_extension(plugin, CLAP_EXT_GUI));
  if (!gui || !gui->create(plugin, CLAP_WINDOW_API_WIN32, false)) {
    std::printf("the plugin has no embeddable gui\n");
    return 1;
  }
  uint32_t w = 700, h = 420;
  gui->get_size(plugin, &w, &h);

  // A real top-level window sized to the plugin's CLIENT area, like a host's
  // editor frame: otherwise the shot would include the title bar's margins.
  RECT want{0, 0, (LONG) w, (LONG) h};
  AdjustWindowRect(&want, WS_OVERLAPPEDWINDOW, FALSE);
  HWND parent = CreateWindowExW(0, L"STATIC", L"Sonore plugin", WS_OVERLAPPEDWINDOW, 80, 80,
                                want.right - want.left, want.bottom - want.top, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
  ShowWindow(parent, SW_SHOW);

  clap_window_t window{};
  window.api = CLAP_WINDOW_API_WIN32;
  window.win32 = parent;
  if (!gui->set_parent(plugin, &window)) {
    std::printf("set_parent failed\n");
    return 1;
  }
  gui->show(plugin);

  std::printf("rendering %s (%ux%u) for %ds...\n", desc->name, w, h, seconds);
  pump(seconds * 1000);

  const bool ok = captureWindow(parent, outPath);
  std::printf(ok ? "wrote %s\n" : "capture failed\n", outPath);

  gui->destroy(plugin);
  DestroyWindow(parent);
  plugin->deactivate(plugin);
  plugin->destroy(plugin);
  entry->deinit();
  FreeLibrary(lib);
  return ok ? 0 : 1;
}
