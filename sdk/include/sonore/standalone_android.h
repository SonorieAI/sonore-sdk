// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: a plugin's standalone, as an Android app.
//
// The same App the desktop standalone runs (standalone.h) -- the plugin's
// Instance, processBlock, the same parameters and state -- inside an Android
// activity (android_app.h) instead of a desktop window:
//
//  - The editor fills the screen, by the choice every format makes: the
//    plugin's page in a WebView when it has one (webview_android.h), else the
//    SDK-drawn native editor, sized to the surface in logical pixels at the
//    screen's density, re-laid out when the phone turns. An instrument's
//    editor carries its on-screen keyboard, which a finger plays.
//  - A MIDI keyboard plugged in plays it: every device that sends MIDI, the
//    ones plugged in later too (Android 10 and later; midi_input.h).
//  - Sound runs while the app is on screen: AAudio opens at start and closes
//    at stop. An app that keeps playing in the background needs a foreground
//    service, which is the app's to declare, not this file's.
//  - An effect hears the microphone, once the user has allowed it -- and only
//    while the sound goes to headphones (or another private route). Through
//    the speaker the microphone would hear the effect's own output and howl,
//    so the input stays closed there and the log says why.
//  - A keyboard that comes and goes, a route that changes under the stream:
//    the output and input are reopened when AAudio reports them lost.
//
// sonore_add_standalone builds this into an APK on Android (no Java of its
// own: NativeActivity loads the library and calls ANativeActivity_onCreate
// below), and also the command-line program, which renders and verifies on a
// device from an adb shell.
#pragma once

#if defined(__ANDROID__) && defined(SONORE_BUILD_STANDALONE)

#include "android_app.h"

namespace sonore {
namespace standalone {

/** The plugin as an Android app: the activity's hooks over one App. */
class AndroidApp : public android::ActivityHooks {
public:
  void created(ANativeActivity* activity) override {
    android::keepScreenOn(activity);
    tellHostInfo(app_);
    // The parameters' defaults before anything reads them: the editor opens
    // before the audio does, and prepare() is what sets them otherwise.
    initialiseParams(app_);
    clapwrap::primeParamsBlock(&app_.instance);
    if (!kDesc.isInstrument && !android::hasPermission(kRecordAudio))
      android::requestPermission(kRecordAudio);
    startMidi(app_);
  }

  void started() override { startSound(); }
  void stopped() override { stopSound(); }

  /** Back from the permission dialog, among other things: the input opens now
   *  if it may. */
  void resumed() override { updateInput(); }

  void windowReady(int width, int height, float scale) override {
    const int w = (int) ((float) width / scale), h = (int) ((float) height / scale);
    Instance& inst = app_.instance;
    if (inst.guiCreated) {
      if (inst.guiIsNative) inst.nativeEditor.setSize(w, h);
      else inst.webview.setSize((uint32_t) w, (uint32_t) h);
      return;
    }
    // The page when the plugin has one and the APK can show it; the native
    // editor otherwise, and whenever the page will not open.
    const EditorChoice choice = clapwrap::editorChoice();
    if (choice.backend == EditorBackend::Web) {
      inst.webview.onMessage = [&inst](const BridgeMessage& m) { clapwrap::guiOnMessage(&inst, m); };
      inst.webview.onTick = [&inst]() { clapwrap::guiTick(&inst); };
      inst.guiIsNative = false;
      inst.guiCreated = inst.webview.create(nullptr, (uint32_t) w, (uint32_t) h, clapwrap::uiHtml(),
                                            clapwrap::bridgeScriptFor(inst), kDesc.id);
      if (inst.guiCreated) return;
      android::log("the page did not open (%s): the native editor instead", inst.webview.status().c_str());
    }
    inst.guiIsNative = true;
    inst.nativeEditor.setContentFactory(clapwrap::customEditorFactory(inst.dsp));
    inst.guiCreated = inst.nativeEditor.open(nullptr, kDesc.params, (int) kDesc.numParams,
                                             clapwrap::makeEditorHost(&inst), w, h);
    if (!inst.guiCreated) android::log("the editor did not open");
  }

  void frame() override {
    if (app_.instance.guiCreated && !app_.instance.guiIsNative) app_.instance.webview.tick();
    if (!running_) return;
    // A route that went away under the stream -- headphones pulled, an
    // interface unplugged: reopened on the new one.
    if (audio_.takeDeviceLost()) {
      android::log("the audio route changed: reopening the output");
      stopSound();
      startSound();
      return;
    }
    if (app_.inputOpen && app_.audioInput.takeDeviceLost()) {
      app_.inputOpen = false;
      app_.audioInput.stop();
    }
    // About once a second, whether the input may be heard (see the file
    // comment): asking the framework every frame would be wasted calls.
    if (++frames_ % 60 == 0) updateInput();
  }

  void destroyed() override {
    stopSound();
    stopMidi(app_);
    app_.instance.nativeEditor.close();
    app_.instance.webview.destroy();
  }

private:
  static constexpr const char* kRecordAudio = "android.permission.RECORD_AUDIO";

  void startSound() {
    if (running_) return;
    running_ = startAudio(app_, audio_, -1, /*remember=*/false);
    if (!running_) {
      android::log("audio unavailable: %s", audio_.error().c_str());
      return;
    }
    android::log("audio: %s, %.0f Hz, %u frames a burst", audio_.deviceName().c_str(), audio_.sampleRate(),
                 audio_.bufferFrames());
    updateInput();
  }

  void stopSound() {
    if (app_.inputOpen) {
      app_.inputOpen = false;
      waitForBlockBoundary(app_);
      app_.audioInput.stop();
    }
    if (running_) audio_.stop();
    running_ = false;
  }

  /** Open the microphone when an effect may hear it, close it when not. */
  void updateInput() {
    if (kDesc.isInstrument || !running_) return;
    const bool allowed = android::hasPermission(kRecordAudio);
    const bool privateRoute = android::outputIsPrivate();
    const bool want = allowed && privateRoute;
    if (want && !app_.inputOpen) {
      startLiveInput(app_, 0, audio_.sampleRate());
    } else if (!want && app_.inputOpen) {
      app_.inputOpen = false;
      waitForBlockBoundary(app_);
      app_.audioInput.stop();
    }
    if (allowed && !privateRoute && !warnedSpeaker_) {
      android::log("the input stays closed while the sound goes to the speaker: plug in headphones");
      warnedSpeaker_ = true;
    }
    if (privateRoute) warnedSpeaker_ = false;
  }

  App app_;
  PlatformAudio audio_;
  bool running_ = false;
  bool warnedSpeaker_ = false;
  uint64_t frames_ = 0;
};

} // namespace standalone
} // namespace sonore

#if defined(SONORE_ANDROID_ACTIVITY)
/** NativeActivity's entry point: the library the manifest names is loaded and
 *  this is called on the main thread, once per activity. */
extern "C" __attribute__((visibility("default"))) void ANativeActivity_onCreate(ANativeActivity* activity, void*,
                                                                                  size_t) {
  sonore::android::run(activity, new sonore::standalone::AndroidApp());
}
#endif

#endif // __ANDROID__ && SONORE_BUILD_STANDALONE
