// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: a web editor with no WebView2 runtime falls back to the native
// one.
//
// Every wrapper opens the native editor when the webview does not start: the
// plugin loses its designed face and keeps every control. But the Windows
// backend's create() returned true even when it already knew the runtime was
// missing -- CreateCoreWebView2EnvironmentWithOptions refuses synchronously --
// so the fallback never ran and the user got an editor that said "Microsoft
// Edge WebView2 Runtime is not installed" instead of knobs.
//
// A missing runtime is simulated the way WebView2 itself allows: pointing
// WEBVIEW2_BROWSER_EXECUTABLE_FOLDER at a folder with no browser in it, before
// any plugin in this process has started a webview.

#include <cstdio>
#include <string>
#include <vector>

#include <sonore/host.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

static int g_checks = 0;
static int g_failures = 0;

static void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

static void pump(int ms) {
  const DWORD until = GetTickCount() + (DWORD) ms;
  while (GetTickCount() < until) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    Sleep(5);
  }
}

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 2) {
    std::printf("usage: webview_fallback_test <plugin folder>\n");
    return 2;
  }
  std::printf("── web editor without a WebView2 runtime ──\n");

  wchar_t temp[MAX_PATH] = {};
  GetTempPathW(MAX_PATH, temp);
  const std::wstring nowhere = std::wstring(temp) + L"sonore-no-webview2-here";
  CreateDirectoryW(nowhere.c_str(), nullptr);
  SetEnvironmentVariableW(L"WEBVIEW2_BROWSER_EXECUTABLE_FOLDER", nowhere.c_str());

  const auto found = sonore::host::scanDirectory(argv[1]);
  char line[300];
  for (const char* format : {"CLAP", "VST3"}) {
    const sonore::host::PluginDescription* pick = nullptr;
    for (const auto& d : found)
      if (d.format == format && d.name.find("Saturator") != std::string::npos) pick = &d;
    if (!pick) {
      std::snprintf(line, sizeof(line), "%s: the Saturator, a web-editor plugin, is in the build", format);
      check(false, line);
      continue;
    }
    auto plugin = sonore::host::loadPlugin(*pick);
    if (!plugin || !plugin->hasEditor()) {
      std::snprintf(line, sizeof(line), "%s: the Saturator loads, with an editor", format);
      check(false, line);
      continue;
    }
    HWND parent = CreateWindowExW(0, L"STATIC", L"sonore fallback host", WS_OVERLAPPEDWINDOW, 0, 0,
                                  900, 600, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    const bool opened = plugin->openEditor((void*) parent);
    pump(300);
    wchar_t cls[128] = L"(none)";
    if (HWND child = GetWindow(parent, GW_CHILD)) GetClassNameW(child, cls, 128);
    const std::wstring name(cls);
    std::string narrow;
    for (wchar_t c : name) narrow.push_back(c < 128 ? (char) c : '?');
    std::snprintf(line, sizeof(line), "%s: the editor opens, and it is the native one (%s)", format,
                  narrow.c_str());
    check(opened && name.rfind(L"SonoreNativeUI_", 0) == 0, line);
    plugin->closeEditor();
    pump(50);

    // The same at 200%, as a host on a HiDPI display says before it opens the
    // editor, and then at 100% as the window moves to another monitor. The
    // fallback is a native editor laid out in LOGICAL units with the scale
    // applied apart from them, as when it is opened on purpose; the CLAP side
    // opened it with the device size as its logical size and never told it
    // the scale, so its layout was twice too big and it stayed that big when
    // the scale fell: a 1400-pixel editor in a 700-pixel frame.
    plugin->setEditorScale(2.0);
    const bool openedScaled = plugin->openEditor((void*) parent);
    pump(300);
    plugin->setEditorScale(1.0);
    pump(100);
    uint32_t wantW = 0, wantH = 0;
    plugin->editorSize(wantW, wantH);
    RECT got{};
    if (HWND child = GetWindow(parent, GW_CHILD)) GetClientRect(child, &got);
    std::snprintf(line, sizeof(line),
                  "%s: opened at 200%% and moved to 100%%, the fallback editor is the size it reports "
                  "(%ldx%ld, reports %ux%u)",
                  format, (long) got.right, (long) got.bottom, wantW, wantH);
    check(openedScaled && wantW > 0 && (uint32_t) got.right == wantW && (uint32_t) got.bottom == wantH, line);
    plugin->closeEditor();
    plugin->setEditorScale(1.0);
    pump(50);
    DestroyWindow(parent);
  }

  // The standalone has no native editor to fall back to: it must say why its
  // window is empty and keep running, not leave a blank window or fall over.
  // The Saturator app, with a one-second screenshot as its exit.
  {
    const std::string app = std::string(argv[1]) + "/SonoreSaturatorApp.exe";
    const std::string shot = std::string(argv[1]) + "/webview_fallback_shot.bmp";
    DeleteFileA(shot.c_str());
    std::string command = "\"" + app + "\" --shot \"" + shot + "\" 1";
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<char> cmd(command.begin(), command.end());
    cmd.push_back(0);
    DWORD code = 99;
    if (CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
      WaitForSingleObject(pi.hProcess, 30000);
      GetExitCodeProcess(pi.hProcess, &code);
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
    }
    const DWORD attrs = GetFileAttributesA(shot.c_str());
    std::snprintf(line, sizeof(line), "the standalone without a runtime still runs and shows its "
                  "window (exit %lu, screenshot %s)", (unsigned long) code,
                  attrs != INVALID_FILE_ATTRIBUTES ? "written" : "missing");
    check(code == 0 && attrs != INVALID_FILE_ATTRIBUTES, line);
    DeleteFileA(shot.c_str());
  }
  RemoveDirectoryW(nowhere.c_str());

  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  if (g_failures == 0) std::printf("SONORE WEBVIEW FALLBACK PASSED\n");
  return g_failures == 0 ? 0 : 1;
}
