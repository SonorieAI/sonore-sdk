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
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
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
  GObjectPtr (*webkit_navigation_policy_decision_get_navigation_action)(GObjectPtr) = nullptr;
  GObjectPtr (*webkit_navigation_action_get_request)(GObjectPtr) = nullptr;
  const char* (*webkit_uri_request_get_uri)(GObjectPtr) = nullptr;
  void (*webkit_policy_decision_ignore)(GObjectPtr) = nullptr;

  // Serving a page's folder (ui_assets.h) under sonore-ui://. Resolved
  // OPTIONALLY: a WebKitGTK lacking one of them still shows a page that is one
  // string, as every plugin did before there were folders; canServe says
  // whether all of them are there.
  void (*webkit_web_view_load_uri)(GtkWidgetPtr, const char*) = nullptr;
  GObjectPtr (*webkit_web_view_get_context)(GtkWidgetPtr) = nullptr;
  void (*webkit_web_context_register_uri_scheme)(GObjectPtr, const char*, void (*)(GObjectPtr, void*),
                                                 void*, void (*)(void*)) = nullptr;
  GObjectPtr (*webkit_web_context_get_security_manager)(GObjectPtr) = nullptr;
  void (*webkit_security_manager_register_uri_scheme_as_secure)(GObjectPtr, const char*) = nullptr;
  void (*webkit_security_manager_register_uri_scheme_as_cors_enabled)(GObjectPtr, const char*) = nullptr;
  const char* (*webkit_uri_scheme_request_get_uri)(GObjectPtr) = nullptr;
  void (*webkit_uri_scheme_request_finish)(GObjectPtr, GObjectPtr, long long, const char*) = nullptr;
  void (*webkit_uri_scheme_request_finish_error)(GObjectPtr, void*) = nullptr;
  // 2.36 and later: a real 404 instead of a load error. Optional on top.
  GObjectPtr (*webkit_uri_scheme_response_new)(GObjectPtr, long long) = nullptr;
  void (*webkit_uri_scheme_response_set_status)(GObjectPtr, unsigned, const char*) = nullptr;
  void (*webkit_uri_scheme_request_finish_with_response)(GObjectPtr, GObjectPtr) = nullptr;
  void (*webkit_settings_set_enable_webgl)(GObjectPtr, int) = nullptr;
  GObjectPtr (*g_memory_input_stream_new_from_data)(const void*, long, void (*)(void*)) = nullptr;
  void (*g_object_unref)(GObjectPtr) = nullptr;
  void* (*g_error_new_literal)(unsigned, int, const char*) = nullptr;
  void (*g_error_free)(void*) = nullptr;
  unsigned (*g_quark_from_static_string)(const char*) = nullptr;
  bool canServe = false;

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

/** A symbol that may be missing: no error, just a null. */
template <typename Fn>
inline void bindOptional(void* lib, const char* name, Fn* out) {
  if (void* sym = lib ? dlsym(lib, name) : nullptr) *out = reinterpret_cast<Fn>(sym);
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
    bind(wk, "webkit_navigation_policy_decision_get_navigation_action",
         &api.webkit_navigation_policy_decision_get_navigation_action, &api);
    bind(wk, "webkit_navigation_action_get_request", &api.webkit_navigation_action_get_request,
         &api);
    bind(wk, "webkit_uri_request_get_uri", &api.webkit_uri_request_get_uri, &api);
    bind(wk, "webkit_policy_decision_ignore", &api.webkit_policy_decision_ignore, &api);

    // GIO is WebKitGTK's own dependency, so it is already loaded by now.
    void* gio = dlopen("libgio-2.0.so.0", RTLD_LAZY | RTLD_GLOBAL);
    bindOptional(wk, "webkit_web_view_load_uri", &api.webkit_web_view_load_uri);
    bindOptional(wk, "webkit_web_view_get_context", &api.webkit_web_view_get_context);
    bindOptional(wk, "webkit_web_context_register_uri_scheme", &api.webkit_web_context_register_uri_scheme);
    bindOptional(wk, "webkit_web_context_get_security_manager", &api.webkit_web_context_get_security_manager);
    bindOptional(wk, "webkit_security_manager_register_uri_scheme_as_secure",
                 &api.webkit_security_manager_register_uri_scheme_as_secure);
    bindOptional(wk, "webkit_security_manager_register_uri_scheme_as_cors_enabled",
                 &api.webkit_security_manager_register_uri_scheme_as_cors_enabled);
    bindOptional(wk, "webkit_uri_scheme_request_get_uri", &api.webkit_uri_scheme_request_get_uri);
    bindOptional(wk, "webkit_uri_scheme_request_finish", &api.webkit_uri_scheme_request_finish);
    bindOptional(wk, "webkit_uri_scheme_request_finish_error", &api.webkit_uri_scheme_request_finish_error);
    bindOptional(wk, "webkit_uri_scheme_response_new", &api.webkit_uri_scheme_response_new);
    bindOptional(wk, "webkit_uri_scheme_response_set_status", &api.webkit_uri_scheme_response_set_status);
    bindOptional(wk, "webkit_uri_scheme_request_finish_with_response",
                 &api.webkit_uri_scheme_request_finish_with_response);
    bindOptional(wk, "webkit_settings_set_enable_webgl", &api.webkit_settings_set_enable_webgl);
    bindOptional(gio, "g_memory_input_stream_new_from_data", &api.g_memory_input_stream_new_from_data);
    bindOptional(gobj, "g_object_unref", &api.g_object_unref);
    bindOptional(glib, "g_error_new_literal", &api.g_error_new_literal);
    bindOptional(glib, "g_error_free", &api.g_error_free);
    bindOptional(glib, "g_quark_from_static_string", &api.g_quark_from_static_string);
    api.canServe = api.webkit_web_view_load_uri && api.webkit_web_view_get_context &&
                   api.webkit_web_context_register_uri_scheme &&
                   api.webkit_web_context_get_security_manager &&
                   api.webkit_security_manager_register_uri_scheme_as_secure &&
                   api.webkit_security_manager_register_uri_scheme_as_cors_enabled &&
                   api.webkit_uri_scheme_request_get_uri && api.webkit_uri_scheme_request_finish &&
                   api.webkit_uri_scheme_request_finish_error && api.g_memory_input_stream_new_from_data &&
                   api.g_object_unref && api.g_error_new_literal && api.g_error_free &&
                   api.g_quark_from_static_string;

    api.ok = api.error.empty();
    return api;
  }();
  return a;
}

/** Injection time for a user script: at document START, so the bridge exists
 *  before any page script runs (mirrors AddScriptToExecuteOnDocumentCreated). */
constexpr int kInjectAtDocumentStart = 0;
/** WEBKIT_USER_CONTENT_INJECT_TOP_FRAME: the page's own document, not every
 *  frame in it (0 is ALL_FRAMES). */
constexpr int kInjectTopFrameOnly = 1;
/** WebKitPolicyDecisionType. */
constexpr int kPolicyNavigationAction = 0;
constexpr int kPolicyNewWindowAction = 1;

// ── A page's folder, served under sonore-ui://<origin label>/ ───────────────
//
// WebKitGTK registers a scheme once per web context, and every editor in the
// process shares the default one, so ONE handler answers for all of them and
// finds the page by the host part of the address: the plugin's origin label
// (uiOriginLabel), the name Windows serves under too. Every editor of one
// plugin serves the same page and folder, so an entry is counted rather than
// owned. The GTK main loop is the only thread that touches any of it.
//
// The scheme is registered SECURE and CORS-ENABLED. Without the second,
// WebKit refuses fetch() and module scripts on a scheme that is not http(s),
// and a page that imports three.js fails on its first line.
namespace uidetail {

struct ServedPage {
  std::string html;
  const UiAssets* assets = nullptr;
  int editors = 0;
};

inline std::map<std::string, ServedPage>& servedPages() {
  static std::map<std::string, ServedPage> pages;
  return pages;
}

inline void answerUiRequest(GObjectPtr request, void*) {
  Api& a = api();
  std::string host, path;
  const char* uri = a.webkit_uri_scheme_request_get_uri(request);
  uiSplitSchemeUrl(uri ? uri : "", host, path);
  const auto page = servedPages().find(host);
  const unsigned char* data = nullptr;
  size_t size = 0;
  std::string mime;
  void (*release)(void*) = nullptr;
  if (page != servedPages().end()) {
    if (uiAssetPath(path) == "editor.html") {
      // A copy: the editor that owns this page may close before WebKit has
      // read the stream to its end.
      void* copy = std::malloc(page->second.html.size() + 1);
      if (copy) {
        std::memcpy(copy, page->second.html.data(), page->second.html.size());
        data = (const unsigned char*) copy;
        size = page->second.html.size();
        release = &std::free;
        mime = "text/html";
      }
    } else if (const UiAsset* asset = page->second.assets ? page->second.assets->find(path) : nullptr) {
      data = asset->data; // compiled into the plugin: lives as long as it does
      size = asset->size;
      mime = uiMimeFor(asset->path);
      mime = mime.substr(0, mime.find(';')); // the type alone; WebKit takes no parameters here
    }
  }
  if (data) {
    GObjectPtr stream = a.g_memory_input_stream_new_from_data(data, (long) size, release);
    a.webkit_uri_scheme_request_finish(request, stream, (long long) size, mime.c_str());
    a.g_object_unref(stream);
    return;
  }
  // Not in the folder: a 404 where WebKit can say one, a load error where not.
  if (a.webkit_uri_scheme_response_new && a.webkit_uri_scheme_response_set_status &&
      a.webkit_uri_scheme_request_finish_with_response) {
    GObjectPtr empty = a.g_memory_input_stream_new_from_data("", 0, nullptr);
    GObjectPtr response = a.webkit_uri_scheme_response_new(empty, 0);
    a.webkit_uri_scheme_response_set_status(response, 404, "Not Found");
    a.webkit_uri_scheme_request_finish_with_response(request, response);
    a.g_object_unref(response);
    a.g_object_unref(empty);
    return;
  }
  void* error = a.g_error_new_literal(a.g_quark_from_static_string("sonore-ui"), 404,
                                      "not in the plugin's folder");
  a.webkit_uri_scheme_request_finish_error(request, error);
  a.g_error_free(error);
}

/** The scheme on this view's context, registered the first time it is met. */
inline void serveUiScheme(GObjectPtr context) {
  static std::set<GObjectPtr> registered;
  if (!context || !registered.insert(context).second) return;
  Api& a = api();
  if (GObjectPtr security = a.webkit_web_context_get_security_manager(context)) {
    a.webkit_security_manager_register_uri_scheme_as_secure(security, kUiScheme);
    a.webkit_security_manager_register_uri_scheme_as_cors_enabled(security, kUiScheme);
  }
  a.webkit_web_context_register_uri_scheme(context, kUiScheme, &answerUiRequest, nullptr, nullptr);
}

} // namespace uidetail

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

  /** The folder the page is served with, for a page that is not the
   *  plugin's own (see webview_win32.h). Before create(). */
  void setAssets(const UiAssets* assets) { assets_ = assets; }

  bool create(unsigned long parentXWindow, uint32_t width, uint32_t height, std::string html,
              std::string bridge, std::string userDataName) {
    if (plug_) return true;
    id_ = std::move(userDataName);
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
                         std::string html, std::string bridge, std::string userDataName) {
    if (plug_) return true;
    id_ = std::move(userDataName);
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

    // A plugin face is an appliance: no dev tools. WebGL said in so many
    // words: it has been on by default for years, but a page drawing in 3D
    // should not depend on a distribution's choice of default.
    if (a.webkit_web_view_get_settings && a.webkit_settings_set_enable_developer_extras) {
      if (GObjectPtr settings = a.webkit_web_view_get_settings(webview_)) {
        a.webkit_settings_set_enable_developer_extras(settings, 0);
        if (a.webkit_settings_set_enable_webgl) a.webkit_settings_set_enable_webgl(settings, 1);
      }
    }

    // The bridge, injected before page script, plus the message channel the
    // bridge posts to (window.webkit.messageHandlers.sonore).
    if (GObjectPtr manager = a.webkit_web_view_get_user_content_manager(webview_)) {
      if (GObjectPtr script = a.webkit_user_script_new(bridge.c_str(), kInjectTopFrameOnly,
                                                       kInjectAtDocumentStart, nullptr, nullptr))
        a.webkit_user_content_manager_add_script(manager, script);
      a.webkit_user_content_manager_register_script_message_handler(manager, "sonore");
      a.g_signal_connect_data(manager, "script-message-received::sonore",
                              (void (*)()) &WebViewHost::onScriptMessage, this, nullptr, 0);
    }

    // The page stays the page (webviewMayNavigate): asked before every
    // navigation, in the page and in its frames, and before every new window.
    a.g_signal_connect_data(webview_, "decide-policy", (void (*)()) &WebViewHost::onDecidePolicy,
                            this, nullptr, 0);

    a.gtk_widget_set_size_request(webview_, (int) width_, (int) height_);
    a.gtk_container_add(plug_, webview_);
    // A page with a folder is served, under the plugin's own origin, so its
    // modules and files load by relative URL; one without is loaded as the
    // string it is, as it always was.
    if (!assets_) assets_ = registeredUiAssets();
    if (assets_ && a.canServe) {
      uidetail::serveUiScheme(a.webkit_web_view_get_context(webview_));
      servedHost_ = uiOriginLabel(id_);
      uidetail::ServedPage& page = uidetail::servedPages()[servedHost_];
      page.html = html;
      page.assets = assets_;
      ++page.editors;
      pageUri_ = std::string(kUiScheme) + "://" + servedHost_ + "/editor.html";
      a.webkit_web_view_load_uri(webview_, pageUri_.c_str());
    } else {
      if (assets_) {
        status_ = "this WebKitGTK cannot serve the page's folder; showing the page alone";
        report();
      }
      a.webkit_web_view_load_html(webview_, html.c_str(), nullptr);
    }
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
      // container (the standalone's own window) is not ours to destroy;
      // only the webview inside it is.
      if (ownsPlug_) a.gtk_widget_destroy(plug_);
      else if (webview_) a.gtk_widget_destroy(webview_);
      plug_ = nullptr;
      webview_ = nullptr;
    }
    if (!servedHost_.empty()) {
      auto& pages = uidetail::servedPages();
      const auto page = pages.find(servedHost_);
      if (page != pages.end() && --page->second.editors <= 0) pages.erase(page);
      servedHost_.clear();
      pageUri_.clear();
    }
    ready_ = false;
    captureKeys = false; // the page that asked is gone (see webview_win32.h)
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
    if (js.empty() || !ready_ || !webview_ || !a.webkit_web_view_run_javascript) return;
    a.webkit_web_view_run_javascript(webview_, js.c_str(), nullptr, nullptr, nullptr);
  }

private:
  static int onTimer(void* self) {
    auto* host = static_cast<WebViewHost*>(self);
    if (host->onTick) host->onTick();
    return 1; // G_SOURCE_CONTINUE
  }

  /** "decide-policy". TRUE says the decision is made here; FALSE leaves it to
   *  WebKit, whose default for a navigation is to go ahead -- which is what an
   *  allowed one wants, and what a response (type 2) should always get. A new
   *  window is refused whatever its address. */
  static int onDecidePolicy(GtkWidgetPtr /*view*/, GObjectPtr decision, int type, void* self) {
    if (type != kPolicyNavigationAction && type != kPolicyNewWindowAction) return 0;
    Api& a = api();
    if (type == kPolicyNavigationAction) {
      GObjectPtr action = a.webkit_navigation_policy_decision_get_navigation_action(decision);
      GObjectPtr request = action ? a.webkit_navigation_action_get_request(action) : nullptr;
      const char* uri = request ? a.webkit_uri_request_get_uri(request) : nullptr;
      // The served page itself (and its reload) is the one address besides
      // webviewMayNavigate's that the page may be at; another file of the
      // folder is not a page to go to.
      const auto* host = static_cast<const WebViewHost*>(self);
      if (uri && (webviewMayNavigate(uri) || (host && !host->pageUri_.empty() && host->pageUri_ == uri)))
        return 0;
    }
    a.webkit_policy_decision_ignore(decision);
    return 1;
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
  /** The plugin's id, the folder, and where its page is served (empty when
   *  it is loaded as a string). */
  std::string id_, servedHost_, pageUri_;
  const UiAssets* assets_ = nullptr;
  unsigned timer_ = 0;
  uint32_t width_ = 700, height_ = 420;
  bool ready_ = false;
  std::string status_;
};

} // namespace gtk
} // namespace sonore

#endif // __linux__
