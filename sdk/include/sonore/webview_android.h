// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the Android webview backend -- the plugin's page in an Android
// WebView, over the app's activity.
//
// A plugin with a page of its own (SONORE_UI_HTML) shows it in a webview on
// every platform, and on Android that is android.webkit.WebView: a Java view,
// made, placed and fed through SonoreBridge (sdk/android/java), the Java the
// APK carries. The page reaches the app through window.sonoreAndroid.post --
// a @JavascriptInterface object, the third channel gui.h's bridge script
// tries, after WebView2's and WebKit's -- which Java calls on a thread of its
// own; each message is handed to the main thread (MessageManager) before
// onMessage sees it, as every other backend delivers on the UI thread.
//
// What Android does differently:
//
//  - The bridge is put INTO the page, at the top of its head, rather than
//    injected at document start: a WebView offers no start-of-document hook
//    an app can rely on, and a script that comes first in the document runs
//    before anything the page itself declares.
//  - The page is laid over the activity's own surface, inside the system
//    bars, and sized in device pixels; its CSS pixels are the display's
//    density-independent ones, as a page expects.
//  - No timer of its own: the app's frame clock calls tick(), which runs
//    onTick at about 30 Hz.
//
// Needs the activity (android_app.h) and the APK's classes.dex; without them
// create() says so and returns false, and the native editor opens instead.
#pragma once

#if defined(__ANDROID__)

#include <android/log.h>
#include <jni.h>

#include <cstdint>
#include <functional>
#include <set>
#include <string>

#include "android_jni.h"
#include "gfx/window_android.h"
#include "gui.h"
#include "message_manager.h"

namespace sonore {
namespace android {

class WebViewHost;

namespace webviewdetail {

/** The hosts alive now: a message Java delivers after its host was
 *  destroyed finds nothing rather than freed memory. Main thread only. */
inline std::set<WebViewHost*>& live() {
  static std::set<WebViewHost*> hosts;
  return hosts;
}

inline void deliver(WebViewHost* host, const std::string& json);

/** SonoreBridge.pageMessage, on the WebView's JavaBridge thread. */
inline void pageMessage(JNIEnv* e, jclass, jlong owner, jstring json) {
  auto* host = reinterpret_cast<WebViewHost*>((intptr_t) owner);
  std::string text = utf8(e, json);
  MessageManager::callAsync([host, text]() {
    if (live().count(host)) deliver(host, text);
  });
}

} // namespace webviewdetail

/** The plugin's page in an Android WebView. The shape every backend has. */
class WebViewHost {
public:
  std::function<void(const BridgeMessage&)> onMessage;
  std::function<void()> onTick;
  /** Recorded: a WebView keeps the soft keyboard to itself already. */
  bool captureKeys = false;

  WebViewHost() = default;
  ~WebViewHost() { destroy(); }
  WebViewHost(const WebViewHost&) = delete;
  WebViewHost& operator=(const WebViewHost&) = delete;

  /** The page, `width` x `height` logical pixels at the top left of the free
   *  part of the screen. `parent` is unused: the activity is the parent. */
  bool create(void* /*parent*/, uint32_t width, uint32_t height, std::string html, std::string bridge,
              std::string /*userDataName*/) {
    if (view_) return true;
    JNIEnv* e = env();
    ANativeActivity* activity = currentActivity();
    jclass bridgeClass = android::bridgeClass();
    if (!e || !activity || !bridgeClass) {
      status_ = "the page needs the app's activity and its classes.dex";
      return false;
    }
    static bool bound = false;
    if (!bound) {
      static const JNINativeMethod natives[] = {
          {"pageMessage", "(JLjava/lang/String;)V", (void*) &webviewdetail::pageMessage},
      };
      bound = registerNatives(natives, 1);
      if (!bound) {
        status_ = "the app's SonoreBridge has no pageMessage";
        return false;
      }
    }
    jmethodID make = e->GetStaticMethodID(
        bridgeClass, "createWebView",
        "(Landroid/app/Activity;JLjava/lang/String;IIII)Landroid/webkit/WebView;");
    if (failed(e) || !make) {
      status_ = "SonoreBridge.createWebView is missing";
      return false;
    }
    webviewdetail::live().insert(this);
    width_ = width;
    height_ = height;
    int x = 0, y = 0, w = 0, h = 0;
    placement(&x, &y, &w, &h);
    jstring page = e->NewStringUTF(withBridge(html, bridge).c_str());
    jobject view = e->CallStaticObjectMethod(bridgeClass, make, activity->clazz,
                                             (jlong) (intptr_t) this, page, x, y, w, h);
    e->DeleteLocalRef(page);
    if (failed(e) || !view) {
      webviewdetail::live().erase(this);
      status_ = "the WebView could not be made";
      return false;
    }
    view_ = e->NewGlobalRef(view);
    e->DeleteLocalRef(view);
    ready_ = true;
    return true;
  }

  void destroy() {
    webviewdetail::live().erase(this);
    JNIEnv* e = env();
    jclass bridgeClass = android::bridgeClass();
    if (view_ && e && bridgeClass) {
      jmethodID gone = e->GetStaticMethodID(bridgeClass, "destroyWebView", "(Landroid/webkit/WebView;)V");
      if (!failed(e) && gone) e->CallStaticVoidMethod(bridgeClass, gone, view_);
      failed(e);
      e->DeleteGlobalRef(view_);
    }
    view_ = nullptr;
    ready_ = false;
  }

  bool ready() const { return ready_; }
  void* handle() const { return (void*) view_; }
  void* nativeWindow() const { return (void*) view_; }
  const std::string& status() const { return status_; }

  /** In logical pixels, at the top left of the free part of the screen. */
  void setSize(uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;
    JNIEnv* e = env();
    jclass bridgeClass = android::bridgeClass();
    if (!view_ || !e || !bridgeClass) return;
    jmethodID place = e->GetStaticMethodID(bridgeClass, "placeWebView", "(Landroid/webkit/WebView;IIII)V");
    if (failed(e) || !place) return;
    int x = 0, y = 0, w = 0, h = 0;
    placement(&x, &y, &w, &h);
    e->CallStaticVoidMethod(bridgeClass, place, view_, x, y, w, h);
    failed(e);
  }

  void setVisible(bool visible) {
    JNIEnv* e = env();
    jclass bridgeClass = android::bridgeClass();
    if (!view_ || !e || !bridgeClass) return;
    jmethodID show = e->GetStaticMethodID(bridgeClass, "setVisible", "(Landroid/webkit/WebView;Z)V");
    if (!failed(e) && show) e->CallStaticVoidMethod(bridgeClass, show, view_, (jboolean) (visible ? 1 : 0));
    failed(e);
  }

  /** Run `js` in the page. [main-thread] */
  void eval(const std::string& js) {
    JNIEnv* e = env();
    jclass bridgeClass = android::bridgeClass();
    if (!view_ || !e || !bridgeClass) return;
    jmethodID evaluate = e->GetStaticMethodID(bridgeClass, "evaluate", "(Landroid/webkit/WebView;Ljava/lang/String;)V");
    if (failed(e) || !evaluate) return;
    jstring script = e->NewStringUTF(js.c_str());
    e->CallStaticVoidMethod(bridgeClass, evaluate, view_, script);
    failed(e);
    e->DeleteLocalRef(script);
  }

  /** The app's frame clock: onTick about thirty times a second. */
  void tick() {
    if (!ready_ || !onTick) return;
    if (++frames_ % 2 == 0) onTick();
  }

private:
  friend void webviewdetail::deliver(WebViewHost*, const std::string&);

  /** Where the page goes, in device pixels: inside the system bars, its
   *  logical size at the display's density, held to the free area. */
  void placement(int* x, int* y, int* w, int* h) const {
    const gfx::AndroidSurface& surface = gfx::AndroidSurface::instance();
    const float scale = surface.densityScale();
    *x = surface.insetLeft();
    *y = surface.insetTop();
    *w = (int) ((float) width_ * scale + 0.5f);
    *h = (int) ((float) height_ * scale + 0.5f);
    if (surface.contentWidth() > 0 && *w > surface.contentWidth()) *w = surface.contentWidth();
    if (surface.contentHeight() > 0 && *h > surface.contentHeight()) *h = surface.contentHeight();
  }

  /** The page, with the bridge as the first thing in its head. */
  static std::string withBridge(const std::string& html, const std::string& bridge) {
    const std::string script = "<script>" + bridge + "</script>";
    std::string out = html;
    size_t at = std::string::npos;
    for (const char* tag : {"<head>", "<HEAD>", "<head "}) {
      at = out.find(tag);
      if (at != std::string::npos) {
        at = out.find('>', at);
        break;
      }
    }
    if (at != std::string::npos) out.insert(at + 1, script);
    else out.insert(0, script);
    return out;
  }

  jobject view_ = nullptr;
  uint32_t width_ = 700, height_ = 420;
  bool ready_ = false;
  uint64_t frames_ = 0;
  bool heard_ = false;
  std::string status_;
};

inline void webviewdetail::deliver(WebViewHost* host, const std::string& json) {
  // Once, in the device log: the page reached the plugin. A page that loads
  // and never says so is the failure to look for first.
  if (!host->heard_) {
    host->heard_ = true;
    __android_log_print(ANDROID_LOG_INFO, "sonore", "the page is talking to the plugin");
  }
  if (host->onMessage) host->onMessage(parseBridgeMessage(json));
}

} // namespace android
} // namespace sonore

#endif // __ANDROID__
