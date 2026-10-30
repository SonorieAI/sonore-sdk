// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the macOS webview backend (WKWebView).
//
// Written against the Objective-C RUNTIME rather than in Objective-C, so the
// whole SDK stays plain C++ and a generated project needs no .mm file, no
// mixed-language build, and no Xcode-specific CMake handling. Every Cocoa call
// here is objc_msgSend on a class looked up by name, which is exactly what the
// compiler emits for Objective-C anyway.
//
// Shape mirrors the other backends: an NSView the host embeds, the bridge
// injected at document start, a ~30 Hz timer, and a message handler.
//
// macOS specifics that shape this file:
//
//  - CLAP's cocoa API uses LOGICAL points, and the OS handles Retina scaling,
//    so set_scale is (correctly) ignored, as it is on Windows.
//
//  - A script message handler is an Objective-C object with a required method.
//    There is no such class in the frameworks, so one is REGISTERED at runtime
//    with a C function as its implementation. That is the standard way to do
//    this without writing Objective-C, and it is why the file needs
//    objc/runtime.h.
//
//  - The webview is added as a subview of the host's NSView and pinned with an
//    autoresizing mask, so a host resizing its window resizes the page without
//    a round trip through set_size.
#pragma once

#if defined(__APPLE__) || defined(SONORE_APPLE_SYNTAX_CHECK)

#include <dlfcn.h>
#include <objc/message.h>
#include <objc/objc.h>
#include <objc/runtime.h>

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>

#include "gui.h"

namespace sonore {
namespace cocoa {

// objc_msgSend must be called through a correctly-typed function pointer: it is
// a trampoline, and calling it with the wrong signature corrupts arguments on
// arm64 (where variadic and regular calls use different registers).
template <typename Ret, typename... Args>
inline Ret msg(id target, SEL selector, Args... args) {
  using Fn = Ret (*)(id, SEL, Args...);
  return reinterpret_cast<Fn>(objc_msgSend)(target, selector, args...);
}

inline id cls(const char* name) { return (id) objc_getClass(name); }

/** An autoreleased NSString from UTF-8. */
inline id nsString(const std::string& s) {
  id str = msg<id>(cls("NSString"), sel_registerName("stringWithUTF8String:"), s.c_str());
  return str;
}

inline std::string fromNsString(id str) {
  if (!str) return std::string();
  const char* utf8 = msg<const char*>(str, sel_registerName("UTF8String"));
  return utf8 ? std::string(utf8) : std::string();
}

struct CGPointStruct { double x, y; };
struct CGSizeStruct { double width, height; };
struct CGRectStruct { CGPointStruct origin; CGSizeStruct size; };

/** The message handler class, registered once. Its `userContentController:
 *  didReceiveScriptMessage:` forwards to the host that owns it. */
class WebViewHost;
inline void handleScriptMessage(id self, SEL, id, id message);
inline void decideNavigationPolicy(id self, SEL, id webView, id action, id decisionHandler);
inline void startSchemeTask(id self, SEL, id webView, id task);
inline void stopSchemeTask(id self, SEL, id webView, id task);

/** A runtime class name that is unique to THIS binary.
 *
 *  The Objective-C runtime has one namespace per process. Two plugins built
 *  from different SDK versions, loaded into one host, would both register
 *  "SonoreScriptHandler": the second finds the first's class and uses it,
 *  with method implementations that live in the OTHER binary -- and when that
 *  binary unloads, every message into the class jumps into freed code. The
 *  address of a function in this image is unique per load, so the name is
 *  too (the Win32 window class does the same with its module handle). */
inline const char* uniqueClassName(const char* base, void* inThisImage, char* out, size_t cap) {
  std::snprintf(out, cap, "%s_%llx", base, (unsigned long long) (uintptr_t) inThisImage);
  return out;
}

inline Class messageHandlerClass() {
  static Class klass = [] {
    char name[96];
    uniqueClassName("SonoreScriptHandler", (void*) &handleScriptMessage, name, sizeof(name));
    Class c = objc_allocateClassPair((Class) cls("NSObject"), name, 0);
    if (!c) return (Class) objc_getClass(name); // already registered by this image
    class_addMethod(c, sel_registerName("userContentController:didReceiveScriptMessage:"),
                    (IMP) handleScriptMessage, "v@:@@");
    // The same object is the view's navigation delegate (WKNavigationDelegate
    // is looked up by selector, so no protocol needs declaring).
    class_addMethod(c, sel_registerName("webView:decidePolicyForNavigationAction:decisionHandler:"),
                    (IMP) decideNavigationPolicy, "v@:@@@?");
    // And the view's WKURLSchemeHandler, which serves the page's folder
    // under sonore-ui:// (ui_assets.h). The protocol is declared when the
    // runtime has it, in case a WebKit ever checks conformance rather than
    // selectors.
    class_addMethod(c, sel_registerName("webView:startURLSchemeTask:"), (IMP) startSchemeTask, "v@:@@");
    class_addMethod(c, sel_registerName("webView:stopURLSchemeTask:"), (IMP) stopSchemeTask, "v@:@@");
    if (Protocol* scheme = objc_getProtocol("WKURLSchemeHandler")) class_addProtocol(c, scheme);
    // One instance variable: the WebViewHost that should receive the message.
    class_addIvar(c, "owner", sizeof(void*), (uint8_t) log2(sizeof(void*)), "^v");
    objc_registerClassPair(c);
    return c;
  }();
  return klass;
}

/**
 * A WKWebView inside an NSView the host embeds.
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

  /** Where the page is served (sonore-ui://...), or empty when it is loaded
   *  as a string: the one address besides webviewMayNavigate's it may be at. */
  const std::string& servedPage() const { return pageUri_; }

  bool create(void* parentNsView, uint32_t width, uint32_t height, std::string html,
              std::string bridge, std::string userDataName) {
    if (webview_) return true;
    width_ = width;
    height_ = height;
    if (!assets_) assets_ = registeredUiAssets();

    id configClass = cls("WKWebViewConfiguration");
    id controllerClass = cls("WKUserContentController");
    id scriptClass = cls("WKUserScript");
    id webviewClass = cls("WKWebView");
    if (!configClass || !controllerClass || !scriptClass || !webviewClass) {
      status_ = "WebKit is not available on this system";
      std::fprintf(stderr, "[sonore:webview] %s\n", status_.c_str());
      return false;
    }

    id config = msg<id>(msg<id>(configClass, sel_registerName("alloc")),
                        sel_registerName("init"));
    id controller = msg<id>(msg<id>(controllerClass, sel_registerName("alloc")),
                            sel_registerName("init"));

    // The bridge, injected at document START so it exists before page script.
    // WKUserScriptInjectionTimeAtDocumentStart == 0.
    id script = msg<id>(
        msg<id>(scriptClass, sel_registerName("alloc")),
        sel_registerName("initWithSource:injectionTime:forMainFrameOnly:"),
        nsString(bridge), (long) 0, (BOOL) YES);
    msg<void>(controller, sel_registerName("addUserScript:"), script);
    msg<void>(script, sel_registerName("release")); // the controller holds it now

    // The channel the bridge posts to: window.webkit.messageHandlers.sonore
    handler_ = msg<id>(msg<id>((id) messageHandlerClass(), sel_registerName("alloc")),
                       sel_registerName("init"));
    object_setInstanceVariable(handler_, "owner", this);
    msg<void>(controller, sel_registerName("addScriptMessageHandler:name:"), handler_,
              nsString("sonore"));

    // A page with a folder is served under sonore-ui://<the plugin's origin
    // label>/, so its modules and files load by relative URL; the handler is
    // the same object, and the configuration must have it before the view
    // is made from it.
    if (assets_) {
      msg<void>(config, sel_registerName("setURLSchemeHandler:forURLScheme:"), handler_, nsString(kUiScheme));
      pageUri_ = std::string(kUiScheme) + "://" + uiOriginLabel(userDataName) + "/editor.html";
      page_ = html;
    }

    msg<void>(config, sel_registerName("setUserContentController:"), controller);
    msg<void>(controller, sel_registerName("release")); // the configuration holds it now

    const CGRectStruct frame{{0.0, 0.0}, {(double) width, (double) height}};
    using NewWebView = id (*)(id, SEL, CGRectStruct, id);
    webview_ = reinterpret_cast<NewWebView>(objc_msgSend)(
        msg<id>(webviewClass, sel_registerName("alloc")),
        sel_registerName("initWithFrame:configuration:"), frame, config);
    // The web view copied what it needs from the configuration; ours was +1
    // from alloc and, in plain C++, nobody else was ever going to release it.
    msg<void>(config, sel_registerName("release"));
    if (!webview_) {
      status_ = "WKWebView could not be created";
      std::fprintf(stderr, "[sonore:webview] %s\n", status_.c_str());
      return false;
    }

    // The page stays the page (webviewMayNavigate). The delegate property is
    // weak; handler_ is what keeps the object alive. With no UI delegate,
    // window.open already opens nothing.
    msg<void>(webview_, sel_registerName("setNavigationDelegate:"), handler_);

    // Fill the parent as it resizes: NSViewWidthSizable | NSViewHeightSizable.
    msg<void>(webview_, sel_registerName("setAutoresizingMask:"), (unsigned long) (2 | 16));

    id parent = (id) parentNsView;
    msg<void>(parent, sel_registerName("addSubview:"), webview_);
    if (!pageUri_.empty()) {
      id url = msg<id>(cls("NSURL"), sel_registerName("URLWithString:"), nsString(pageUri_));
      id request = url ? msg<id>(cls("NSURLRequest"), sel_registerName("requestWithURL:"), url) : nullptr;
      if (request) msg<id>(webview_, sel_registerName("loadRequest:"), request);
    } else {
      msg<void>(webview_, sel_registerName("loadHTMLString:baseURL:"), nsString(html), (id) nullptr);
    }

    // The UI clock, as an NSTimer on the main run loop.
    timer_ = msg<id>(cls("NSTimer"),
                     sel_registerName("scheduledTimerWithTimeInterval:repeats:block:"),
                     (double) 0.033, (BOOL) YES, makeTimerBlock());
    // In the COMMON modes too: a scheduled timer fires in the default mode
    // only, and the editor froze while a host held its loop in a modal or
    // mouse-tracking mode -- a menu open, a fader dragged, a dialog up.
    // NSRunLoopCommonModes is the string "kCFRunLoopCommonModes".
    if (timer_) {
      id loop = msg<id>(cls("NSRunLoop"), sel_registerName("currentRunLoop"));
      if (loop)
        msg<void>(loop, sel_registerName("addTimer:forMode:"), timer_,
                  nsString("kCFRunLoopCommonModes"));
    }
    ready_ = true;
    return true;
  }

  void destroy() {
    if (timer_) {
      msg<void>(timer_, sel_registerName("invalidate"));
      timer_ = nullptr;
    }
    // The handler's owner pointer is cleared FIRST. WebKit's content process
    // may still deliver a queued script message after the view is torn down,
    // and a handler whose owner is a destroyed WebViewHost would hand it a
    // freed object.
    if (webview_) msg<void>(webview_, sel_registerName("setNavigationDelegate:"), (id) nullptr);
    if (handler_) {
      object_setInstanceVariable(handler_, "owner", nullptr);
      msg<void>(handler_, sel_registerName("release"));
      handler_ = nullptr;
    }
    if (webview_) {
      msg<void>(webview_, sel_registerName("removeFromSuperview"));
      msg<void>(webview_, sel_registerName("release")); // ours from alloc/init
      webview_ = nullptr;
    }
    ready_ = false;
    captureKeys = false; // the page that asked is gone (see webview_win32.h)
    pageUri_.clear();
    std::string().swap(page_);
  }

  bool ready() const { return ready_; }

  /** The answer to a request on this view's scheme: the page, a file of the
   *  folder, or nothing (a 404). */
  bool lookUp(const std::string& path, const unsigned char*& data, size_t& size, std::string& mime) const {
    if (uiAssetPath(path) == "editor.html" && !page_.empty()) {
      data = (const unsigned char*) page_.data();
      size = page_.size();
      mime = "text/html; charset=utf-8";
      return true;
    }
    if (const UiAsset* asset = assets_ ? assets_->find(path) : nullptr) {
      data = asset->data;
      size = asset->size;
      mime = uiMimeFor(asset->path);
      return true;
    }
    return false;
  }
  /** The platform window this view lives in, as an opaque handle: an HWND on
   *  Windows, a GtkWidget* under GTK, an NSView* on macOS. Only a modal
   *  dialog needs it, and only so the dialog is owned by the editor rather
   *  than floating loose behind the DAW. */
  void* nativeWindow() const { return (void*) webview_; }
  const std::string& status() const { return status_; }
  void* handle() const { return webview_; }

  void setSize(uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;
    if (!webview_) return;
    const CGRectStruct frame{{0.0, 0.0}, {(double) width, (double) height}};
    using SetFrame = void (*)(id, SEL, CGRectStruct);
    reinterpret_cast<SetFrame>(objc_msgSend)(webview_, sel_registerName("setFrame:"), frame);
  }

  void setVisible(bool visible) {
    if (webview_) msg<void>(webview_, sel_registerName("setHidden:"), (BOOL) (visible ? NO : YES));
  }

  void eval(const std::string& js) {
    if (js.empty() || !ready_ || !webview_) return; // an empty script still wakes the page
    msg<void>(webview_, sel_registerName("evaluateJavaScript:completionHandler:"),
              nsString(js), (id) nullptr);
  }

  /** Called by the registered handler class. */
  void deliver(const std::string& json) {
    if (!onMessage) return;
    const BridgeMessage msg = parseBridgeMessage(json);
    if (msg.kind != BridgeMessage::Kind::None) onMessage(msg);
  }

private:
  /** An Objective-C block wrapping our tick. Blocks are a struct with a
   *  function pointer: building one by hand avoids requiring the Blocks
   *  runtime extension in a plain C++ translation unit. */
  struct BlockLiteral {
    void* isa;
    int flags;
    int reserved;
    void (*invoke)(void*, id);
    struct Descriptor {
      unsigned long reserved;
      unsigned long size;
    }* descriptor;
    WebViewHost* host;
  };

  static void invokeTimerBlock(void* block, id /*timer*/) {
    auto* self = static_cast<BlockLiteral*>(block)->host;
    if (self && self->onTick) self->onTick();
  }

  id makeTimerBlock() {
    static BlockLiteral::Descriptor descriptor{0, sizeof(BlockLiteral)};
    timerBlock_.isa = (void*) dlsymGlobalStackBlock();
    // BLOCK_IS_GLOBAL (1 << 28): without it Block_copy treats this as a stack
    // block and copies it to the heap, which happens to work, but only by
    // accident of our layout. Global blocks are returned as-is, which is what
    // a struct with process lifetime actually is.
    timerBlock_.flags = 1 << 28;
    timerBlock_.reserved = 0;
    timerBlock_.invoke = &WebViewHost::invokeTimerBlock;
    timerBlock_.descriptor = &descriptor;
    timerBlock_.host = this;
    return (id) &timerBlock_;
  }

  /** _NSConcreteGlobalBlock, the isa every literal block carries. Resolved
   *  at runtime like every other Apple symbol in this file -- a block-scope
   *  extern declaration mangles into THIS namespace and never links (the
   *  first bug macOS CI ever caught here). */
  static void* dlsymGlobalStackBlock() {
    return dlsym(RTLD_DEFAULT, "_NSConcreteGlobalBlock");
  }

  id webview_ = nullptr;
  id handler_ = nullptr;
  id timer_ = nullptr;
  /** The folder, the page kept to serve, and where it is served. */
  const UiAssets* assets_ = nullptr;
  std::string page_, pageUri_;
  BlockLiteral timerBlock_{};
  uint32_t width_ = 700, height_ = 420;
  bool ready_ = false;
  std::string status_;
};

inline void handleScriptMessage(id self, SEL, id, id message) {
  void* owner = nullptr;
  object_getInstanceVariable(self, "owner", &owner);
  if (!owner) return;
  // From the page's own document only: the channel is reachable from every
  // frame, and the bridge is injected into the main one alone.
  id frame = msg<id>(message, sel_registerName("frameInfo"));
  if (!frame || !msg<BOOL>(frame, sel_registerName("isMainFrame"))) return;
  // A string, or nothing. The bridge always posts one, but a page may call
  // window.webkit.messageHandlers.sonore.postMessage itself, with an object:
  // WebKit hands that over as an NSDictionary, UTF8String is not a method
  // of it, and the NSInvalidArgumentException unwound through C++ frames
  // and took the host down.
  id body = msg<id>(message, sel_registerName("body"));
  if (!body || !msg<BOOL>(body, sel_registerName("isKindOfClass:"), (Class) objc_getClass("NSString")))
    return;
  static_cast<WebViewHost*>(owner)->deliver(fromNsString(body));
}

/** WKNavigationDelegate's policy method: allow only what webviewMayNavigate
 *  allows. The decision handler is a block, called through its invoke
 *  pointer, which follows isa, flags and reserved in every block's layout.
 *  WKNavigationActionPolicyCancel is 0, Allow is 1. */
inline void decideNavigationPolicy(id self, SEL, id, id action, id decisionHandler) {
  id request = action ? msg<id>(action, sel_registerName("request")) : nullptr;
  id url = request ? msg<id>(request, sel_registerName("URL")) : nullptr;
  id text = url ? msg<id>(url, sel_registerName("absoluteString")) : nullptr;
  // The served page itself (and its reload) besides webviewMayNavigate's;
  // another file of the folder is not a page to go to.
  void* owner = nullptr;
  object_getInstanceVariable(self, "owner", &owner);
  const std::string address = text ? fromNsString(text) : std::string();
  const auto* host = static_cast<const WebViewHost*>(owner);
  const bool allowed = text && (webviewMayNavigate(address) ||
                                (host && !host->servedPage().empty() && host->servedPage() == address));
  struct BlockHeader {
    void* isa;
    int flags;
    int reserved;
    void (*invoke)(void*, long);
  };
  auto* block = reinterpret_cast<BlockHeader*>(decisionHandler);
  if (block && block->invoke) block->invoke(block, allowed ? 1L : 0L);
}

/** WKURLSchemeHandler: a request on sonore-ui://, answered at once from the
 *  owner's page and folder with a real HTTP response -- 200 and the file's
 *  type, or 404 -- so fetch() sees a status and a module its JavaScript type.
 *  Answered synchronously, so a stop can never arrive between the start and
 *  the answer, and there is nothing for stopSchemeTask to cancel. */
inline void startSchemeTask(id self, SEL, id, id task) {
  void* owner = nullptr;
  object_getInstanceVariable(self, "owner", &owner);
  id request = task ? msg<id>(task, sel_registerName("request")) : nullptr;
  id url = request ? msg<id>(request, sel_registerName("URL")) : nullptr;
  id text = url ? msg<id>(url, sel_registerName("absoluteString")) : nullptr;
  if (!url) return;
  std::string host, path;
  uiSplitSchemeUrl(fromNsString(text), host, path);
  const unsigned char* data = nullptr;
  size_t size = 0;
  std::string mime = "text/plain; charset=utf-8";
  const bool found = owner && static_cast<const WebViewHost*>(owner)->lookUp(path, data, size, mime);
  id headers = msg<id>(cls("NSDictionary"), sel_registerName("dictionaryWithObject:forKey:"), nsString(mime),
                       nsString("Content-Type"));
  using NewResponse = id (*)(id, SEL, id, long, id, id);
  id response = reinterpret_cast<NewResponse>(objc_msgSend)(
      msg<id>(cls("NSHTTPURLResponse"), sel_registerName("alloc")),
      sel_registerName("initWithURL:statusCode:HTTPVersion:headerFields:"), url, (long) (found ? 200 : 404),
      nsString("HTTP/1.1"), headers);
  msg<void>(task, sel_registerName("didReceiveResponse:"), response);
  if (response) msg<void>(response, sel_registerName("release"));
  if (found && size > 0) {
    // A copy: the page belongs to an editor that may close while WebKit
    // still holds the data.
    id body = msg<id>(cls("NSData"), sel_registerName("dataWithBytes:length:"), (const void*) data,
                      (unsigned long) size);
    msg<void>(task, sel_registerName("didReceiveData:"), body);
  }
  msg<void>(task, sel_registerName("didFinish"));
}

inline void stopSchemeTask(id, SEL, id, id) {}

} // namespace cocoa
} // namespace sonore

#endif // __APPLE__
