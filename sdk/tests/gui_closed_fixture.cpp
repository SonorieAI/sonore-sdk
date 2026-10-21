// SPDX-License-Identifier: Apache-2.0
// Sonore SDK test fixture: a CLAP plugin whose editor goes away by itself.
//
// clap_host_gui::closed(was_destroyed = true) is a plugin saying its GUI is
// gone -- a crashed renderer, a lost display connection -- and the spec puts
// the host under an obligation: "the host must call clap_plugin_gui->destroy()
// to acknowledge the gui destruction". sonore/host.h answered by forgetting the
// editor was open, so it never destroyed it, and the next openEditor() called
// create() on a GUI that had never been destroyed.
//
// The SDK's own wrapper never sends closed(), so nothing built from it could
// show that. This is written against the raw CLAP API instead: its show()
// reports the GUI destroyed, and it counts creates, destroys and any create
// made while a GUI is still alive in the process environment, which it shares
// with the test that loads it.
//
// A second plugin in the file, com.sonorie.test.gui-resize, has an editor
// that resizes itself, as one does when its corner is dragged or its zoom
// changed: shown at 320x200, it asks the host for 640x480 through
// request_resize. It takes the size when the host accepts, as the extension
// lets it, and records the answer as SONORE_GUI_RESIZE_ANSWER (1 or 0) and
// any set_size() the host makes as SONORE_GUI_RESIZE_SET ("WxH").

#include <clap/clap.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

void bump(const char* name) {
  char text[32] = {0};
#if defined(_WIN32)
  GetEnvironmentVariableA(name, text, (DWORD) sizeof(text));
#else
  if (const char* v = std::getenv(name)) std::snprintf(text, sizeof(text), "%s", v);
#endif
  std::snprintf(text, sizeof(text), "%d", std::atoi(text) + 1);
#if defined(_WIN32)
  SetEnvironmentVariableA(name, text);
#else
  setenv(name, text, 1);
#endif
}

struct Fixture {
  clap_plugin_t plugin{};
  const clap_host_t* host = nullptr;
  bool guiAlive = false;
  bool resizer = false; // com.sonorie.test.gui-resize
  uint32_t width = 320, height = 200;
};

void record(const char* name, const char* text) {
#if defined(_WIN32)
  SetEnvironmentVariableA(name, text);
#else
  setenv(name, text, 1);
#endif
}

Fixture* self(const clap_plugin_t* p) { return static_cast<Fixture*>(p->plugin_data); }

bool guiIsApiSupported(const clap_plugin_t*, const char*, bool floating) { return !floating; }
bool guiGetPreferredApi(const clap_plugin_t*, const char** api, bool* floating) {
#if defined(_WIN32)
  *api = CLAP_WINDOW_API_WIN32;
#elif defined(__APPLE__)
  *api = CLAP_WINDOW_API_COCOA;
#else
  *api = CLAP_WINDOW_API_X11;
#endif
  *floating = false;
  return true;
}
bool guiCreate(const clap_plugin_t* p, const char*, bool floating) {
  if (floating) return false;
  if (self(p)->resizer) return true;
  if (self(p)->guiAlive) bump("SONORE_GUI_FIXTURE_CREATE_WHILE_ALIVE");
  self(p)->guiAlive = true;
  bump("SONORE_GUI_FIXTURE_CREATES");
  return true;
}
void guiDestroy(const clap_plugin_t* p) {
  if (self(p)->resizer) return;
  self(p)->guiAlive = false;
  bump("SONORE_GUI_FIXTURE_DESTROYS");
}
bool guiSetScale(const clap_plugin_t*, double) { return false; }
bool guiGetSize(const clap_plugin_t* p, uint32_t* w, uint32_t* h) {
  *w = self(p)->width;
  *h = self(p)->height;
  return true;
}
bool guiCanResize(const clap_plugin_t* p) { return self(p)->resizer; }
bool guiGetResizeHints(const clap_plugin_t*, clap_gui_resize_hints_t*) { return false; }
bool guiAdjustSize(const clap_plugin_t* p, uint32_t*, uint32_t*) { return self(p)->resizer; }
bool guiSetSize(const clap_plugin_t* p, uint32_t w, uint32_t h) {
  if (!self(p)->resizer) return false;
  self(p)->width = w;
  self(p)->height = h;
  char text[32];
  std::snprintf(text, sizeof(text), "%ux%u", (unsigned) w, (unsigned) h);
  record("SONORE_GUI_RESIZE_SET", text);
  return true;
}
bool guiSetParent(const clap_plugin_t*, const clap_window_t*) { return true; }
bool guiSetTransient(const clap_plugin_t*, const clap_window_t*) { return false; }
void guiSuggestTitle(const clap_plugin_t*, const char*) {}
bool guiShow(const clap_plugin_t* p) {
  const auto* hostGui = static_cast<const clap_host_gui_t*>(
      self(p)->host->get_extension(self(p)->host, CLAP_EXT_GUI));
  if (self(p)->resizer) {
    // Shown, and straight away wanting to be bigger. Accepted, the size is
    // the plugin's to take: "the host doesn't have to call set_size()".
    const bool accepted =
        hostGui && hostGui->request_resize && hostGui->request_resize(self(p)->host, 640, 480);
    record("SONORE_GUI_RESIZE_ANSWER", accepted ? "1" : "0");
    if (accepted) {
      self(p)->width = 640;
      self(p)->height = 480;
    }
    return true;
  }
  // Shown, and immediately lost: the GUI is gone and the host is told so.
  if (hostGui && hostGui->closed) hostGui->closed(self(p)->host, true);
  return true;
}
bool guiHide(const clap_plugin_t*) { return true; }

const clap_plugin_gui_t kGui = {
    guiIsApiSupported, guiGetPreferredApi, guiCreate,     guiDestroy,       guiSetScale,
    guiGetSize,        guiCanResize,       guiGetResizeHints, guiAdjustSize, guiSetSize,
    guiSetParent,      guiSetTransient,    guiSuggestTitle, guiShow,          guiHide,
};

const char* const kFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, nullptr};
const clap_plugin_descriptor_t kDescriptor = {
    CLAP_VERSION_INIT,
    "com.sonorie.test.gui-closed",
    "Sonore GUI Closed Fixture",
    "Sonorie",
    "https://sonorie.com",
    "",
    "",
    "1.0.0",
    "An editor that reports itself destroyed as soon as it is shown.",
    kFeatures,
};

const clap_plugin_descriptor_t kResizeDescriptor = {
    CLAP_VERSION_INIT,
    "com.sonorie.test.gui-resize",
    "Sonore GUI Resize Fixture",
    "Sonorie",
    "https://sonorie.com",
    "",
    "",
    "1.0.0",
    "An editor that asks to be bigger as soon as it is shown.",
    kFeatures,
};

bool pluginInit(const clap_plugin_t*) { return true; }
void pluginDestroy(const clap_plugin_t* p) { delete self(p); }
bool pluginActivate(const clap_plugin_t*, double, uint32_t, uint32_t) { return true; }
void pluginDeactivate(const clap_plugin_t*) {}
bool pluginStartProcessing(const clap_plugin_t*) { return true; }
void pluginStopProcessing(const clap_plugin_t*) {}
void pluginReset(const clap_plugin_t*) {}
clap_process_status pluginProcess(const clap_plugin_t*, const clap_process_t*) {
  return CLAP_PROCESS_CONTINUE;
}
const void* pluginGetExtension(const clap_plugin_t*, const char* id) {
  return std::strcmp(id, CLAP_EXT_GUI) == 0 ? &kGui : nullptr;
}
void pluginOnMainThread(const clap_plugin_t*) {}

uint32_t factoryCount(const clap_plugin_factory_t*) { return 2; }
const clap_plugin_descriptor_t* factoryDescriptor(const clap_plugin_factory_t*, uint32_t index) {
  return index == 0 ? &kDescriptor : (index == 1 ? &kResizeDescriptor : nullptr);
}
const clap_plugin_t* factoryCreate(const clap_plugin_factory_t*, const clap_host_t* host,
                                   const char* id) {
  const bool resizer = id && std::strcmp(id, kResizeDescriptor.id) == 0;
  if (!id || (!resizer && std::strcmp(id, kDescriptor.id) != 0)) return nullptr;
  auto* f = new Fixture();
  f->host = host;
  f->resizer = resizer;
  f->plugin.desc = resizer ? &kResizeDescriptor : &kDescriptor;
  f->plugin.plugin_data = f;
  f->plugin.init = pluginInit;
  f->plugin.destroy = pluginDestroy;
  f->plugin.activate = pluginActivate;
  f->plugin.deactivate = pluginDeactivate;
  f->plugin.start_processing = pluginStartProcessing;
  f->plugin.stop_processing = pluginStopProcessing;
  f->plugin.reset = pluginReset;
  f->plugin.process = pluginProcess;
  f->plugin.get_extension = pluginGetExtension;
  f->plugin.on_main_thread = pluginOnMainThread;
  return &f->plugin;
}
const clap_plugin_factory_t kFactory = {factoryCount, factoryDescriptor, factoryCreate};

bool entryInit(const char*) { return true; }
void entryDeinit() {}
const void* entryGetFactory(const char* id) {
  return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &kFactory : nullptr;
}

} // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT,
    entryInit,
    entryDeinit,
    entryGetFactory,
};
