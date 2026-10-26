// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: a plugin's standalone, as an iPhone and iPad app.
//
// Included by standalone.h on iOS, where it is runInteractive: the same App
// every platform's standalone runs -- the plugin's Instance, processBlock, the
// parameters and their state -- under UIKit instead of a desktop window.
// main() is still standalone.h's, so the offline modes (--verify, --render)
// work as they do everywhere, which is how CI's simulator checks the app's own
// binary; with none of them, runInteractive hands the process to
// UIApplicationMain and an app delegate registered here at run time (no
// Objective-C source, as everywhere in the SDK).
//
//  - The editor fills the screen inside the safe area -- clear of the notch,
//    the Dynamic Island and the home indicator -- and follows a rotation.
//    The same choice every format makes: the plugin's page in a WKWebView
//    when it has one, else the SDK-drawn native editor (gfx/window_uikit.h),
//    whose on-screen keyboard a finger plays.
//  - Sound runs while the app is in front. RemoteIO under the app's
//    AVAudioSession (audio_ios.h); an interruption -- a call -- stops it and
//    its end restarts it.
//  - An effect hears the microphone once the user allows it, and only while
//    the sound goes to headphones or an interface: through the speaker the
//    microphone would hear the effect and howl. The permission is asked for
//    at the first launch; NSMicrophoneUsageDescription, which iOS requires
//    before it will ask, is in the Info.plist sonore_add_standalone writes.
#pragma once

#include "apple_platform.h"

#if defined(SONORE_IOS) && defined(SONORE_BUILD_STANDALONE)

#include <CoreFoundation/CFRunLoop.h>
#include <objc/message.h>
#include <objc/runtime.h>

#include <cstdio>

#include "gfx/window_uikit.h"

extern "C" int UIApplicationMain(int argc, char* argv[], void* principalClassName, void* delegateClassName);

namespace sonore {
namespace standalone {
namespace iosapp {

using gfx::uikit::send;
using gfx::uikit::sendRect;

/** The app's one of everything: the App the runInteractive call was given,
 *  the output, and the views the editor lives in. Main thread only. */
struct Runtime {
  App* app = nullptr;
  PlatformAudio audio;
  bool running = false;
  bool warnedSpeaker = false;
  id window = nil;
  id container = nil;
  CFRunLoopTimerRef clock = nullptr;
};

inline Runtime& rt() {
  static Runtime r;
  return r;
}

inline bool isEffect() { return !kDesc.isInstrument; }

inline void stopSound() {
  Runtime& r = rt();
  if (!r.app) return;
  if (r.app->inputOpen) {
    r.app->inputOpen = false;
    waitForBlockBoundary(*r.app);
    r.app->audioInput.stop();
  }
  if (r.running) r.audio.stop();
  r.running = false;
}

/** The microphone opened or closed for an effect, by the permission and the
 *  route (the file comment says why the route). */
inline void updateInput();

inline void startSound() {
  Runtime& r = rt();
  if (!r.app || r.running) return;
  // Recording is opened with the output, on the one RemoteIO unit, once the
  // user has said yes.
  r.audio.setRecording(isEffect() && iosaudio::recordPermission() == iosaudio::Permission::Granted);
  r.running = startAudio(*r.app, r.audio, -1, /*remember=*/false);
  if (!r.running) {
    std::fprintf(stderr, "[sonore] audio unavailable: %s\n", r.audio.error().c_str());
    return;
  }
  std::printf("[sonore] audio: %.0f Hz\n", r.audio.sampleRate());
  updateInput();
}

inline void updateInput() {
  Runtime& r = rt();
  if (!r.app || !isEffect() || !r.running) return;
  const bool allowed = iosaudio::recordPermission() == iosaudio::Permission::Granted;
  // Allowed after the audio opened without the microphone: reopened with it.
  if (allowed && !RemoteIo::instance().records()) {
    stopSound();
    startSound();
    return;
  }
  const bool privateRoute = iosaudio::outputIsPrivate();
  const bool want = allowed && privateRoute;
  if (want && !r.app->inputOpen) {
    startLiveInput(*r.app, 0, r.audio.sampleRate());
  } else if (!want && r.app->inputOpen) {
    r.app->inputOpen = false;
    waitForBlockBoundary(*r.app);
    r.app->audioInput.stop();
  }
  if (allowed && !privateRoute && !r.warnedSpeaker) {
    std::printf("[sonore] the input stays closed while the sound goes to the speaker: plug in headphones\n");
    r.warnedSpeaker = true;
  }
  if (privateRoute) r.warnedSpeaker = false;
}

/** Twice a second, on the main run loop: a lost route, the permission's
 *  answer arriving, headphones in or out. */
inline void clockTick(CFRunLoopTimerRef, void*) {
  Runtime& r = rt();
  if (!r.app) return;
  if (r.running && r.audio.takeDeviceLost()) {
    stopSound();
    startSound();
    return;
  }
  updateInput();
  clapwrap::reportOutputGuard(&r.app->instance);
}

/** UIEdgeInsets: top, left, bottom, right. */
struct EdgeInsets { double top, left, bottom, right; };

/** The editor, into the container, at the container's size: opened the first
 *  time, resized after. */
inline void layoutEditor() {
  Runtime& r = rt();
  if (!r.app || !r.container) return;
  Instance& inst = r.app->instance;
  const gfx::uikit::CGRect2 b = sendRect<gfx::uikit::CGRect2>(r.container, "bounds");
  const int w = (int) b.size.width, h = (int) b.size.height;
  if (w <= 0 || h <= 0) return;
  if (inst.guiCreated) {
    if (inst.guiIsNative) inst.nativeEditor.setSize(w, h);
#if defined(SONORE_HAS_WEBVIEW_BACKEND)
    else inst.webview.setSize((uint32_t) w, (uint32_t) h);
#endif
    return;
  }
  const EditorChoice choice = clapwrap::editorChoice();
  inst.guiIsNative = choice.backend == EditorBackend::Native;
  if (inst.guiIsNative) {
    inst.nativeEditor.setContentFactory(clapwrap::customEditorFactory(inst.dsp));
    inst.guiCreated = inst.nativeEditor.open((void*) r.container, kDesc.params, (int) kDesc.numParams,
                                             clapwrap::makeEditorHost(&inst), w, h);
    return;
  }
#if defined(SONORE_HAS_WEBVIEW_BACKEND)
  inst.webview.onMessage = [&inst](const BridgeMessage& m) { clapwrap::guiOnMessage(&inst, m); };
  inst.webview.onTick = [&inst]() { clapwrap::guiTick(&inst); };
  inst.guiCreated = inst.webview.create((void*) r.container, (uint32_t) w, (uint32_t) h, clapwrap::uiHtml(),
                                        clapwrap::bridgeScriptFor(inst), kDesc.id);
#endif
}

// ── The root view controller ──

inline void vcLoadView(id self, SEL) {
  Runtime& r = rt();
  id screen = send<id>(gfx::uikit::cls("UIScreen"), "mainScreen");
  const gfx::uikit::CGRect2 bounds = sendRect<gfx::uikit::CGRect2>(screen, "bounds");
  using InitFrame = id (*)(id, SEL, gfx::uikit::CGRect2);
  id root = reinterpret_cast<InitFrame>(objc_msgSend)(send<id>(gfx::uikit::cls("UIView"), "alloc"),
                                                      sel_registerName("initWithFrame:"), bounds);
  send<void>(root, "setBackgroundColor:", send<id>(gfx::uikit::cls("UIColor"), "blackColor"));
  r.container = reinterpret_cast<InitFrame>(objc_msgSend)(send<id>(gfx::uikit::cls("UIView"), "alloc"),
                                                          sel_registerName("initWithFrame:"), bounds);
  send<void>(root, "addSubview:", r.container);
  send<void>(self, "setView:", root);
  send<void>(root, "release");
}

/** The container follows the safe area; the editor follows the container. */
inline void vcViewDidLayoutSubviews(id self, SEL) {
  Runtime& r = rt();
  id root = send<id>(self, "view");
  if (!root || !r.container) return;
  const gfx::uikit::CGRect2 b = sendRect<gfx::uikit::CGRect2>(root, "bounds");
  const EdgeInsets in = sendRect<EdgeInsets>(root, "safeAreaInsets");
  const gfx::uikit::CGRect2 inner{{in.left, in.top},
                                  {b.size.width - in.left - in.right, b.size.height - in.top - in.bottom}};
  using SetFrame = void (*)(id, SEL, gfx::uikit::CGRect2);
  reinterpret_cast<SetFrame>(objc_msgSend)(r.container, sel_registerName("setFrame:"), inner);
  layoutEditor();
}

/** The status bar off: the editor is the whole screen there is. */
inline BOOL vcPrefersStatusBarHidden(id, SEL) { return YES; }
/** The home indicator dims when it is not touched, over an instrument. */
inline BOOL vcPrefersHomeIndicatorAutoHidden(id, SEL) { return YES; }

inline Class controllerClass() {
  static Class klass = [] {
    char name[96];
    gfx::uikit::uniqueName("SonoreAppController", (void*) &vcLoadView, name, sizeof(name));
    if (Class existing = objc_getClass(name)) return existing;
    Class c = objc_allocateClassPair(objc_getClass("UIViewController"), name, 0);
    if (!c) return (Class) nullptr;
    class_addMethod(c, sel_registerName("loadView"), (IMP) vcLoadView, "v@:");
    class_addMethod(c, sel_registerName("viewDidLayoutSubviews"), (IMP) vcViewDidLayoutSubviews, "v@:");
    class_addMethod(c, sel_registerName("prefersStatusBarHidden"), (IMP) vcPrefersStatusBarHidden, "B@:");
    class_addMethod(c, sel_registerName("prefersHomeIndicatorAutoHidden"), (IMP) vcPrefersHomeIndicatorAutoHidden,
                    "B@:");
    objc_registerClassPair(c);
    return c;
  }();
  return klass;
}

// ── The app delegate ──

inline BOOL didFinishLaunching(id, SEL, id, id) {
  Runtime& r = rt();
  id screen = send<id>(gfx::uikit::cls("UIScreen"), "mainScreen");
  const gfx::uikit::CGRect2 bounds = sendRect<gfx::uikit::CGRect2>(screen, "bounds");
  using InitFrame = id (*)(id, SEL, gfx::uikit::CGRect2);
  r.window = reinterpret_cast<InitFrame>(objc_msgSend)(send<id>(gfx::uikit::cls("UIWindow"), "alloc"),
                                                       sel_registerName("initWithFrame:"), bounds);
  id controller = send<id>(send<id>((id) controllerClass(), "alloc"), "init");
  send<void>(r.window, "setRootViewController:", controller);
  send<void>(controller, "release");
  send<void>(r.window, "makeKeyAndVisible");
  // Never dims or sleeps while an instrument is being played.
  send<void>(send<id>(gfx::uikit::cls("UIApplication"), "sharedApplication"), "setIdleTimerDisabled:", (BOOL) YES);

  if (isEffect() && iosaudio::recordPermission() == iosaudio::Permission::Undetermined)
    iosaudio::requestRecordPermission();
  CFRunLoopTimerContext context{};
  r.clock = CFRunLoopTimerCreate(nullptr, CFAbsoluteTimeGetCurrent() + 0.5, 0.5, 0, 0, &clockTick, &context);
  if (r.clock) CFRunLoopAddTimer(CFRunLoopGetMain(), r.clock, kCFRunLoopCommonModes);
  startSound();
  return YES;
}

inline void didBecomeActive(id, SEL, id) { startSound(); }
inline void didEnterBackground(id, SEL, id) { stopSound(); }

inline Class delegateClass() {
  static Class klass = [] {
    char name[96];
    gfx::uikit::uniqueName("SonoreAppDelegate", (void*) &didFinishLaunching, name, sizeof(name));
    if (Class existing = objc_getClass(name)) return existing;
    Class c = objc_allocateClassPair(objc_getClass("UIResponder"), name, 0);
    if (!c) return (Class) nullptr;
    class_addMethod(c, sel_registerName("application:didFinishLaunchingWithOptions:"), (IMP) didFinishLaunching,
                    "B@:@@");
    class_addMethod(c, sel_registerName("applicationDidBecomeActive:"), (IMP) didBecomeActive, "v@:@");
    class_addMethod(c, sel_registerName("applicationDidEnterBackground:"), (IMP) didEnterBackground, "v@:@");
    if (Protocol* p = objc_getProtocol("UIApplicationDelegate")) class_addProtocol(c, p);
    objc_registerClassPair(c);
    return c;
  }();
  return klass;
}

/** iOS: the app. UIApplicationMain does not return. --play runs the audio for
 *  a while with no window, as on every platform, for a test with no screen.
 *  standalone.h's runInteractive, on iOS. */
inline int runApp(App& app, const Options& opt) {
  if (opt.playOnly) {
    PlatformAudio audio;
    if (!startLiveAudio(app, audio, opt.deviceIndex)) return 1;
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, opt.seconds, false);
    audio.stop();
    reportPlayed(app, opt.seconds);
    return 0;
  }
  rt().app = &app;
  initialiseParams(app);
  clapwrap::primeParamsBlock(&app.instance);
  Class delegate = delegateClass();
  if (!delegate) {
    std::fprintf(stderr, "[sonore] UIKit is not available\n");
    return 1;
  }
  static char program[] = "app";
  static char* argv[] = {program, nullptr};
  CFStringRef name = CFStringCreateWithCString(nullptr, class_getName(delegate), kCFStringEncodingUTF8);
  return UIApplicationMain(1, argv, nullptr, (void*) name);
}

} // namespace iosapp
} // namespace standalone
} // namespace sonore

#endif // SONORE_IOS && SONORE_BUILD_STANDALONE
