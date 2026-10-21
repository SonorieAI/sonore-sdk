// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: sonore/host.h answers a CLAP editor that destroyed itself.
//
// "If was_destroyed is true, then the host must call clap_plugin_gui->destroy()
// to acknowledge the gui destruction." host.h forgot the editor instead, so it
// never destroyed it and the next openEditor() called create() on a GUI still
// alive. The fixture's editor reports itself destroyed the moment it is shown
// and counts what the host then does.
//
// And an editor that resizes itself -- its corner dragged, its zoom changed --
// asks the host with request_resize. The host refused every such request,
// "honestly", leaving a caller to follow editorSize(): but a plugin refused
// keeps its size, so editorSize() never moved and no plugin editor in this
// host could ever change size. The fixture's second plugin asks for 640x480
// as it is shown.

#include <cstdio>
#include <cstdlib>
#include <string>

#include <sonore/host.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

static int g_checks = 0;
static int g_failures = 0;

static void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

static std::string text(const char* name) {
  char value[64] = {0};
#if defined(_WIN32)
  GetEnvironmentVariableA(name, value, (DWORD) sizeof(value));
#else
  if (const char* v = std::getenv(name)) std::snprintf(value, sizeof(value), "%s", v);
#endif
  return value;
}

static int counter(const char* name) {
  char text[32] = {0};
#if defined(_WIN32)
  GetEnvironmentVariableA(name, text, (DWORD) sizeof(text));
#else
  if (const char* v = std::getenv(name)) std::snprintf(text, sizeof(text), "%s", v);
#endif
  return std::atoi(text);
}

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 2) {
    std::printf("usage: host_gui_closed_test <SonoreGuiClosedFixture.clap>\n");
    return 2;
  }
  std::printf("── CLAP gui.closed(was_destroyed): %s ──\n", argv[1]);

  const auto found = sonore::host::describeFile(argv[1]);
  check(!found.empty(), "the fixture is described");
  if (found.empty()) return 1;

  char line[200];
  {
    auto plugin = sonore::host::loadPlugin(found.front());
    check(plugin != nullptr, "the fixture loads");
    if (!plugin) return 1;
    check(plugin->hasEditor(), "…and has an editor");

    // The fixture never touches the window, so any non-null handle will do.
    int notAWindow = 0;
    check(plugin->openEditor(&notAWindow), "the editor opens, and reports itself destroyed");
    // The caller's message loop: the next main-thread call the host gets.
    plugin->idleEditor();
    std::snprintf(line, sizeof(line), "the host acknowledged the destruction with destroy() "
                  "(%d destroy for %d create)", counter("SONORE_GUI_FIXTURE_DESTROYS"),
                  counter("SONORE_GUI_FIXTURE_CREATES"));
    check(counter("SONORE_GUI_FIXTURE_DESTROYS") == 1, line);
    uint32_t w = 0, h = 0;
    check(!plugin->editorSize(w, h), "…and no longer reports an editor that is gone");

    check(plugin->openEditor(&notAWindow), "the editor can be opened again");
    std::snprintf(line, sizeof(line), "…without create() on a GUI still alive (%d times)",
                  counter("SONORE_GUI_FIXTURE_CREATE_WHILE_ALIVE"));
    check(counter("SONORE_GUI_FIXTURE_CREATE_WHILE_ALIVE") == 0, line);
    plugin->closeEditor();
  }
  std::snprintf(line, sizeof(line), "after unload every create had its destroy (%d of %d)",
                counter("SONORE_GUI_FIXTURE_DESTROYS"), counter("SONORE_GUI_FIXTURE_CREATES"));
  check(counter("SONORE_GUI_FIXTURE_CREATES") == 2 &&
            counter("SONORE_GUI_FIXTURE_DESTROYS") == 2, line);

  std::printf("── A CLAP editor that resizes itself ──\n");
  {
    const sonore::host::PluginDescription* resizing = nullptr;
    for (const auto& d : found)
      if (d.id == "com.sonorie.test.gui-resize") resizing = &d;
    auto plugin = resizing ? sonore::host::loadPlugin(*resizing) : nullptr;
    int notAWindow = 0;
    const bool opened = plugin && plugin->openEditor(&notAWindow);
    check(opened, "the self-resizing fixture loads and its editor opens");
    if (opened) {
      plugin->idleEditor(); // the caller's message loop
      uint32_t w = 0, h = 0;
      const bool sized = plugin->editorSize(w, h);
      std::snprintf(line, sizeof(line), "an editor that asks for 640x480 is that size, for the "
                    "caller to follow (%ux%u; request %s, set_size %s)", (unsigned) w, (unsigned) h,
                    text("SONORE_GUI_RESIZE_ANSWER") == "1" ? "accepted" : "refused",
                    text("SONORE_GUI_RESIZE_SET").empty() ? "never" : text("SONORE_GUI_RESIZE_SET").c_str());
      check(sized && w == 640 && h == 480 && text("SONORE_GUI_RESIZE_ANSWER") == "1", line);
      // And the other way: the caller's window dragged to 800x600 asks the
      // editor to follow it, through can_resize(), adjust_size() and
      // set_size(). There was no call for it at all.
      const bool took = plugin->setEditorSize(800, 600);
      const bool resized = plugin->editorSize(w, h);
      std::snprintf(line, sizeof(line), "the caller resizes the editor to 800x600 (%s, %ux%u, "
                    "set_size %s)", took ? "taken" : "refused", (unsigned) w, (unsigned) h,
                    text("SONORE_GUI_RESIZE_SET").empty() ? "never" : text("SONORE_GUI_RESIZE_SET").c_str());
      check(took && resized && w == 800 && h == 600 && text("SONORE_GUI_RESIZE_SET") == "800x600", line);
      plugin->closeEditor();
      check(!plugin->setEditorSize(800, 600), "...and with no editor open there is nothing to resize");
    }
  }

  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  if (g_failures == 0) std::printf("SONORE HOST GUI CLOSED PASSED\n");
  return g_failures == 0 ? 0 : 1;
}
