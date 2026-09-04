// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the Linux webview backend (GTK3 + WebKitGTK).
//
// Mirrors webview_win32.h: a child window the host embeds, a webview inside it,
// the `window.sonore` bridge injected before page script, and a ~30 Hz timer
// pushing parameters and meters into the page.
//
// Linux specifics that shape this file:
//
//  - CLAP's X11 embedding hands us a raw X window id, and GTK plugs into it
//    through gtk_plug_new(). A GtkPlug is the standard XEmbed client: it is
//    what makes the plugin's widgets live inside the host's window rather than
//    floating.
//
//  - WebKitGTK ships as webkit2gtk-4.1 (libsoup3) on current distros and 4.0
//    (libsoup2) on older ones, and the two CANNOT be loaded into one process
//    together. Rather than link either, everything is resolved with dlopen at
//    runtime, newest first: a plugin binary then runs on both, and a host that
//    already pulled in the other version does not crash us. Linking directly is
//    how a plugin ends up refusing to load on half the distros in the world.
//
//  - Same reason GTK itself is dlopened: hosts vary in what they already have.
//
// When nothing can be loaded the plugin says so instead of showing a blank box.
#pragma once

#if defined(__linux__)

#include <dlfcn.h>

#include <cstdio>
#include <functional>
#include <string>

#include "gui.h"

namespace sonore {
namespace gtk {

// ── Minimal GLib/GTK/WebKit surface, declared rather than #included ──────────
// Only what this file calls. Declaring it keeps the SDK free of a build-time
// dependency on GTK headers, which a plugin author on another distro may not
// have, and every symbol is resolved at runtime anyway.
using GtkWidgetPtr = void*;
using GObjectPtr = void*;

struct Api {
  // GTK
  int (*gtk_init_check)(int*, char***) = nullptr;
  GtkWidgetPtr (*gtk_plug_new)(unsigned long) = nullptr;
  unsigned long (*gtk_plug_get_id)(GtkWidgetPtr) = nullptr;
  void (*gtk_widget_show_all)(GtkWidgetPtr) = nullptr;
  void (*gtk_widget_hide)(GtkWidgetPtr) = nullptr;
  void (*gtk_widget_destroy)(GtkWidgetPtr) = nullptr;
  void (*gtk_container_add)(GtkWidgetPtr, GtkWidgetPtr) = nullptr;
  void (*gtk_widget_set_size_request)(GtkWidgetPtr, int, int) = nullptr;
  GtkWidgetPtr (*gtk_label_new)(const char*) = nullptr;
  int (*gtk_events_pending)() = nullptr;
  void (*gtk_main_iteration_do)(int) = nullptr;
  // Standalone windows (the plugin editors only ever embed via GtkPlug).
  GtkWidgetPtr (*gtk_window_new)(int) = nullptr;
  void (*gtk_window_set_title)(GtkWidgetPtr, const char*) = nullptr;
  void (*gtk_window_set_default_size)(GtkWidgetPtr, int, int) = nullptr;
  void (*gtk_main)() = nullptr;
  void (*gtk_main_quit)() = nullptr;

  // GLib
  unsigned (*g_timeout_add)(unsigned, int (*)(void*), void*) = nullptr;
  int (*g_source_remove)(unsigned) = nullptr;
  unsigned long (*g_signal_connect_data)(GObjectPtr, const char*, void (*)(),
                                         void*, void*, int) = nullptr;
  void (*g_free)(void*) = nullptr;

  // WebKit
  GtkWidgetPtr (*webkit_web_view_new)() = nullptr;
  void (*webkit_web_view_load_html)(GtkWidgetPtr, const char*, const char*) = nullptr;
  void (*webkit_web_view_run_javascript)(GtkWidgetPtr, const char*, void*, void*, void*) = nullptr;
  GObjectPtr (*webkit_web_view_get_user_content_manager)(GtkWidgetPtr) = nullptr;
  GObjectPtr (*webkit_user_script_new)(const char*, int, int, const char* const*,
                                       const char* const*) = nullptr;
  void (*webkit_user_content_manager_add_script)(GObjectPtr, GObjectPtr) = nullptr;
  int (*webkit_user_content_manager_register_script_message_handler)(GObjectPtr,
                                                                     const char*) = nullptr;
  GObjectPtr (*webkit_javascript_result_get_js_value)(void*) = nullptr;
  char* (*jsc_value_to_string)(GObjectPtr) = nullptr;
  GObjectPtr (*webkit_web_view_get_settings)(GtkWidgetPtr) = nullptr;
  void (*webkit_settings_set_enable_developer_extras)(GObjectPtr, int) = nullptr;
  void (*webkit_settings_set_enable_write_console_messages_to_stdout)(GObjectPtr, int) = nullptr;

  bool ok = false;
  std::string error;
};

/** Load a symbol, recording the first miss so the failure names itself. */
template <typename Fn>
inline void bind(void* lib, const char* name, Fn* out, Api* api) {
  if (!api->error.empty()) return;
  void* sym = dlsym(lib, name);
  if (!sym) {
    api->error = std::string("missing symbol: ") + name;
    return;
  }
  *out = reinterpret_cast<Fn>(sym);
}

/** Resolve GTK + WebKit once per process. */
inline Api& api() {
  static Api a = [] {
    Api api;
    // Versioned sonames only: the unversioned .so lives in -dev packages, which
    // a user's machine has no reason to have installed.
    void* gtk = dlopen("libgtk-3.so.0", RTLD_LAZY | RTLD_GLOBAL);
    if (!gtk) {
      api.error = "libgtk-3.so.0 is not available";
      return api;
    }
    void* gobj = dlopen("libgobject-2.0.so.0", RTLD_LAZY | RTLD_GLOBAL);
    void* glib = dlopen("libglib-2.0.so.0", RTLD_LAZY | RTLD_GLOBAL);
    if (!gobj || !glib) {
      api.error = "glib/gobject are not available";
      return api;
    }
    // 4.1 (libsoup3) first, then 4.0. The two must never share a process, so
    // whichever loads first is the one this plugin uses for its lifetime.
    void* wk = dlopen("libwebkit2gtk-4.1.so.0", RTLD_LAZY | RTLD_GLOBAL);
    if (!wk) wk = dlopen("libwebkit2gtk-4.0.so.37", RTLD_LAZY | RTLD_GLOBAL);
    if (!wk) wk = dlopen("libwebkit2gtk-4.0.so.18", RTLD_LAZY | RTLD_GLOBAL);
    if (!wk) {
      api.error = "WebKitGTK is not installed (libwebkit2gtk-4.1 or 4.0)";
      return api;
    }

    bind(gtk, "gtk_init_check", &api.gtk_init_check, &api);
    bind(gtk, "gtk_plug_new", &api.gtk_plug_new, &api);
    bind(gtk, "gtk_plug_get_id", &api.gtk_plug_get_id, &api);
    bind(gtk, "gtk_widget_show_all", &api.gtk_widget_show_all, &api);
    bind(gtk, "gtk_widget_hide", &api.gtk_widget_hide, &api);
    bind(gtk, "gtk_widget_destroy", &api.gtk_widget_destroy, &api);
    bind(gtk, "gtk_container_add", &api.gtk_container_add, &api);
    bind(gtk, "gtk_widget_set_size_request", &api.gtk_widget_set_size_request, &api);
    bind(gtk, "gtk_label_new", &api.gtk_label_new, &api);
    bind(gtk, "gtk_events_pending", &api.gtk_events_pending, &api);
    bind(gtk, "gtk_main_iteration_do", &api.gtk_main_iteration_do, &api);
    bind(gtk, "gtk_window_new", &api.gtk_window_new, &api);
    bind(gtk, "gtk_window_set_title", &api.gtk_window_set_title, &api);
    bind(gtk, "gtk_window_set_default_size", &api.gtk_window_set_default_size, &api);
    bind(gtk, "gtk_main", &api.gtk_main, &api);
    bind(gtk, "gtk_main_quit", &api.gtk_main_quit, &api);

    bind(glib, "g_timeout_add", &api.g_timeout_add, &api);
    bind(glib, "g_source_remove", &api.g_source_remove, &api);
    bind(glib, "g_free", &api.g_free, &api);
    bind(gobj, "g_signal_connect_data", &api.g_signal_connect_data, &api);

    bind(wk, "webkit_web_view_new", &api.webkit_web_view_new, &api);
    bind(wk, "webkit_web_view_load_html", &api.webkit_web_view_load_html, &api);
    bind(wk, "webkit_web_view_run_javascript", &api.webkit_web_view_run_javascript, &api);
    bind(wk, "webkit_web_view_get_user_content_manager",
         &api.webkit_web_view_get_user_content_manager, &api);
    bind(wk, "webkit_user_script_new", &api.webkit_user_script_new, &api);
    bind(wk, "webkit_user_content_manager_add_script",
         &api.webkit_user_content_manager_add_script, &api);
    bind(wk, "webkit_user_content_manager_register_script_message_handler",
         &api.webkit_user_content_manager_register_script_message_handler, &api);
    bind(wk, "webkit_javascript_result_get_js_value",
         &api.webkit_javascript_result_get_js_value, &api);
    bind(wk, "jsc_value_to_string", &api.jsc_value_to_string, &api);
    bind(wk, "webkit_web_view_get_settings", &api.webkit_web_view_get_settings, &api);
    bind(wk, "webkit_settings_set_enable_developer_extras",
         &api.webkit_settings_set_enable_developer_extras, &api);

    api.ok = api.error.empty();
    return api;
  }();
  return a;
}

/** Injection time for a user script: at document START, so the bridge exists
 *  before any page script runs (mirrors AddScriptToExecuteOnDocumentCreated). */
constexpr int kInjectAtDocumentStart = 0;
constexpr int kInjectInAllFrames = 0;

/**
 * A webview inside a GtkPlug, embedded in the host's X11 window.
 *
 * Same contract as the Win32 backend: create() works immediately, everything
 * else tolerates not being ready, and destroy() is safe at any point.
 */
class WebViewHost {
public:
  std::function<void(const BridgeMessage&)> onMessage;
  /**
   * Whether the PAGE wants the keyboard. False by default, because a webview
   * with focus swallows every keystroke and the spacebar is transport in
   * every DAW there is.
   *
   * The FLAG is here on every backend so a wrapper does not have to know
   * which one it is talking to. The forwarding it controls is implemented on
   * Windows only: WebView2 raises AcceleratorKeyPressed before the page sees
   * a key, which is the hook this needs, and the WebKitGTK and WKWebView
   * equivalents are written against APIs there is no desktop in this loop to
   * try. Setting it here therefore records the page's intent and changes
   * nothing yet, which is better than a wrapper that fails to compile for two
   * of the three.
   */
  bool captureKeys = false;

  std::function<void()> onTick;

  ~WebViewHost() { destroy(); }

  bool create(unsigned long parentXWindow, uint32_t width, uint32_t height, std::string html,
              std::string bridge, std::string /*userDataName*/) {
    if (plug_) return true;
    width_ = width;
    height_ = height;

    Api& a = api();
    if (!a.ok) {
      status_ = a.error;
      report();
      return false;
    }
    // The host owns the main loop; gtk_init_check is safe to call again and
    // returns false rather than aborting if the display cannot be opened.
    if (!a.gtk_init_check(nullptr, nullptr)) {
      status_ = "no X display available";
      report();
      return false;
    }

    plug_ = a.gtk_plug_new(parentXWindow);
    if (!plug_) {
      status_ = "could not embed into the host window";
      report();
      return false;
    }
    return finishCreate(std::move(html), std::move(bridge));
  }

  /** The standalone variant: the webview goes into a container WE were given
   *  (a gtk_window), instead of a GtkPlug embedded in someone else's window.
   *  Everything after the parent differs not at all, so it is shared. */
  bool createInContainer(GtkWidgetPtr container, uint32_t width, uint32_t height,
                         std::string html, std::string bridge, std::string /*userDataName*/) {
    if (plug_) return true;
    width_ = width;
    height_ = height;
    Api& a = api();
    if (!a.ok) {
      status_ = a.error;
      report();
      return false;
    }
    plug_ = container;
    ownsPlug_ = false; // the caller's window is the caller's to destroy
    return finishCreate(std::move(html), std::move(bridge));
  }

private:
  bool finishCreate(std::string html, std::string bridge) {
    Api& a = api();

    webview_ = a.webkit_web_view_new();
    if (!webview_) {
      status_ = "WebKit refused to create a view";
      a.gtk_container_add(plug_, a.gtk_label_new(status_.c_str()));
      a.gtk_widget_show_all(plug_);
      report();
      return false;
    }

    // A plugin face is an appliance: no dev tools.
    if (a.webkit_web_view_get_settings && a.webkit_settings_set_enable_developer_extras) {
      if (GObjectPtr settings = a.webkit_web_view_get_settings(webview_))
        a.webkit_settings_set_enable_developer_extras(settings, 0);
    }

    // The bridge, injected before page script, plus the message channel the
    // bridge posts to (window.webkit.messageHandlers.sonore).
    if (GObjectPtr manager = a.webkit_web_view_get_user_content_manager(webview_)) {
      if (GObjectPtr script = a.webkit_user_script_new(bridge.c_str(), kInjectInAllFrames,
                                                       kInjectAtDocumentStart, nullptr, nullptr))
        a.webkit_user_content_manager_add_script(manager, script);
      a.webkit_user_content_manager_register_script_message_handler(manager, "sonore");
      a.g_signal_connect_data(manager, "script-message-received::sonore",
                              (void (*)()) &WebViewHost::onScriptMessage, this, nullptr, 0);
    }

    a.gtk_widget_set_size_request(webview_, (int) width_, (int) height_);
    a.gtk_container_add(plug_, webview_);
    a.webkit_web_view_load_html(webview_, html.c_str(), nullptr);
    a.gtk_widget_show_all(plug_);

    ready_ = true;
    timer_ = a.g_timeout_add(33, &WebViewHost::onTimer, this);
    return true;
  }

public:
  void destroy() {
    Api& a = api();
    if (timer_ && a.g_source_remove) {
      a.g_source_remove(timer_);
      timer_ = 0;
    }
    if (plug_ && a.gtk_widget_destroy) {
      // Destroying the plug destroys the webview it contains. A borrowed
      // container (the standalone's own window) is not ours to destroy:
      // only the webview inside it is.
      if (ownsPlug_) a.gtk_widget_destroy(plug_);
      else if (webview_) a.gtk_widget_destroy(webview_);
      plug_ = nullptr;
      webview_ = nullptr;
    }
    ready_ = false;
  }

  bool ready() const { return ready_; }
  /** The platform window this view lives in, as an opaque handle: an HWND on
   *  Windows, a GtkWidget* under GTK, an NSView* on macOS. Only a modal
   *  dialog needs it, and only so the dialog is owned by the editor rather
   *  than floating loose behind the DAW. */
  void* nativeWindow() const { return (void*) webview_; }
  const std::string& status() const { return status_; }
  void* handle() const { return plug_; }

  void setSize(uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;
    Api& a = api();
    if (webview_ && a.gtk_widget_set_size_request)
      a.gtk_widget_set_size_request(webview_, (int) width, (int) height);
  }

  void setVisible(bool visible) {
    Api& a = api();
    if (!plug_) return;
    if (visible) a.gtk_widget_show_all(plug_);
    else a.gtk_widget_hide(plug_);
  }

  void eval(const std::string& js) {
    Api& a = api();
    if (!ready_ || !webview_ || !a.webkit_web_view_run_javascript) return;
    a.webkit_web_view_run_javascript(webview_, js.c_str(), nullptr, nullptr, nullptr);
  }

private:
  static int onTimer(void* self) {
    auto* host = static_cast<WebViewHost*>(self);
    if (host->onTick) host->onTick();
    return 1; // G_SOURCE_CONTINUE
  }

  static void onScriptMessage(GObjectPtr /*manager*/, void* result, void* self) {
    auto* host = static_cast<WebViewHost*>(self);
    Api& a = api();
    if (!host->onMessage || !a.webkit_javascript_result_get_js_value || !a.jsc_value_to_string)
      return;
    GObjectPtr value = a.webkit_javascript_result_get_js_value(result);
    if (!value) return;
    char* text = a.jsc_value_to_string(value);
    if (!text) return;
    const BridgeMessage msg = parseBridgeMessage(std::string(text));
    if (msg.kind != BridgeMessage::Kind::None) host->onMessage(msg);
    if (a.g_free) a.g_free(text);
  }

  void report() const {
    std::fprintf(stderr, "[sonore:webview] %s\n", status_.c_str());
  }

  GtkWidgetPtr plug_ = nullptr;
  GtkWidgetPtr webview_ = nullptr;
  bool ownsPlug_ = true;
  unsigned timer_ = 0;
  uint32_t width_ = 700, height_ = 420;
  bool ready_ = false;
  std::string status_;
};

} // namespace gtk
} // namespace sonore

#endif // __linux__
