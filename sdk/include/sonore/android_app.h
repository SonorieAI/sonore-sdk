// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: an Android app's activity, in C++.
//
// ── NativeActivity, and no Java of our own ──────────────────────────────────
//
// Android starts an app as a Java Activity. android.app.NativeActivity is the
// framework's own one for apps written in C and C++: it loads the library the
// manifest names and calls ANativeActivity_onCreate in it, then forwards the
// activity's life -- start, resume, pause, the window, the input queue -- as C
// callbacks. So an APK built by sonore_add_standalone carries one .so and no
// dex, and everything an app does runs here, on the activity's MAIN thread,
// the one Android calls the UI thread.
//
// What this file does with it:
//
//  - Puts the window on the screen: the activity's ANativeWindow is attached
//    to gfx::AndroidSurface, the compositor every native-window peer draws
//    into (gfx/window_android.h).
//  - Runs the UI: the main thread's ALooper gets the input queue, a 60 Hz
//    timerfd for the surface's clock while the activity is visible, and the
//    MessageManager's wake-up descriptor, so callAsync() lands on this
//    thread as it does on every other platform.
//  - Gives the app a place on disk: HOME is the app's files folder and TMPDIR
//    its cache, which is where every "user folder" and "temporary file" in the
//    SDK then goes. An Android shell's /tmp does not exist for an app.
//  - Asks the framework what the C side cannot: the screen's density, whether
//    the app may record audio, and whether the sound is going to headphones.
//    Through JNI on framework classes only, which is why there is no dex.
//
// An app implements ActivityHooks and returns one from sonore_android_make_app
// (standalone_android.h does, for a plugin's standalone).
#pragma once

#if defined(__ANDROID__)

#include <android/api-level.h>
#include <android/configuration.h>
#include <android/input.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <jni.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "android_jni.h"
#include "gfx/window_android.h"
#include "message_manager.h"

namespace sonore {
namespace android {

/** printf to the device log (logcat), under the "sonore" tag. */
inline void log(const char* format, ...) __attribute__((format(printf, 1, 2)));
inline void log(const char* format, ...) {
  va_list args;
  va_start(args, format);
  __android_log_vprint(ANDROID_LOG_INFO, "sonore", format, args);
  va_end(args);
}

/**
 * What an app does when its activity moves: all on the main thread. Every
 * hook is optional; the activity itself keeps the window on the surface.
 */
struct ActivityHooks {
  virtual ~ActivityHooks() = default;
  /** Once, before anything else: the activity exists, the paths are set. */
  virtual void created(ANativeActivity*) {}
  /** Visible (onStart) and in front (onResume), and the reverse. */
  virtual void started() {}
  virtual void resumed() {}
  virtual void paused() {}
  virtual void stopped() {}
  /** The surface is there to draw on, or resized (rotation, split screen, the
   *  system bars moving): the part of it nothing covers, in device pixels, at
   *  the given density scale. */
  virtual void windowReady(int /*width*/, int /*height*/, float /*scale*/) {}
  /** The surface is going away: nothing may be drawn until windowReady. */
  virtual void windowLost() {}
  /** Every frame the activity is visible, before the surface composites. */
  virtual void frame() {}
  /** The activity is ending. The process may go on, and may make another. */
  virtual void destroyed() {}
};

// ── JNI, on framework classes (currentActivity, env and failed are in
// android_jni.h, with the SDK's own Java class) ─────────────────────────────

/** The density scale: one logical pixel in device pixels (dpi / 160). */
inline float densityScale(ANativeActivity* activity) {
  AConfiguration* config = AConfiguration_new();
  if (!config) return 1.0f;
  AConfiguration_fromAssetManager(config, activity->assetManager);
  const int32_t dpi = AConfiguration_getDensity(config);
  AConfiguration_delete(config);
  if (dpi <= 0 || dpi == ACONFIGURATION_DENSITY_NONE || dpi == ACONFIGURATION_DENSITY_ANY) return 1.0f;
  return (float) dpi / 160.0f;
}

/** The app's cache folder (Context.getCacheDir), or empty. */
inline std::string cacheDir(ANativeActivity* activity) {
  JNIEnv* e = activity->env;
  jclass ctx = e->GetObjectClass(activity->clazz);
  jmethodID getCacheDir = e->GetMethodID(ctx, "getCacheDir", "()Ljava/io/File;");
  std::string out;
  if (!failed(e) && getCacheDir) {
    jobject file = e->CallObjectMethod(activity->clazz, getCacheDir);
    if (!failed(e) && file) {
      jclass fileClass = e->GetObjectClass(file);
      jmethodID getPath = e->GetMethodID(fileClass, "getAbsolutePath", "()Ljava/lang/String;");
      auto path = (jstring) (getPath ? e->CallObjectMethod(file, getPath) : nullptr);
      if (!failed(e) && path) {
        const char* utf = e->GetStringUTFChars(path, nullptr);
        if (utf) out = utf;
        e->ReleaseStringUTFChars(path, utf);
        e->DeleteLocalRef(path);
      }
      e->DeleteLocalRef(fileClass);
      e->DeleteLocalRef(file);
    }
  }
  e->DeleteLocalRef(ctx);
  return out;
}

/** Whether the app holds an Android permission ("android.permission...."). */
inline bool hasPermission(const char* permission) {
  ANativeActivity* a = currentActivity();
  JNIEnv* e = env();
  if (!a || !e) return false;
  jclass ctx = e->GetObjectClass(a->clazz);
  jmethodID check = e->GetMethodID(ctx, "checkSelfPermission", "(Ljava/lang/String;)I");
  bool granted = false;
  if (!failed(e) && check) {
    jstring name = e->NewStringUTF(permission);
    const jint r = e->CallIntMethod(a->clazz, check, name);
    granted = !failed(e) && r == 0; // PackageManager.PERMISSION_GRANTED
    e->DeleteLocalRef(name);
  }
  e->DeleteLocalRef(ctx);
  return granted;
}

/** Ask the user for a permission. The answer comes back as the activity
 *  resuming (the system dialog paused it): check hasPermission() there. */
inline void requestPermission(const char* permission) {
  ANativeActivity* a = currentActivity();
  JNIEnv* e = env();
  if (!a || !e) return;
  jclass ctx = e->GetObjectClass(a->clazz);
  jmethodID request = e->GetMethodID(ctx, "requestPermissions", "([Ljava/lang/String;I)V");
  if (!failed(e) && request) {
    jclass stringClass = e->FindClass("java/lang/String");
    jstring name = e->NewStringUTF(permission);
    jobjectArray names = e->NewObjectArray(1, stringClass, name);
    e->CallVoidMethod(a->clazz, request, names, (jint) 1);
    failed(e);
    e->DeleteLocalRef(names);
    e->DeleteLocalRef(name);
    e->DeleteLocalRef(stringClass);
  }
  e->DeleteLocalRef(ctx);
}

/**
 * Whether the sound is going somewhere only the user hears it -- wired or
 * Bluetooth headphones, a USB interface, a hearing aid -- rather than the
 * built-in speaker, where a microphone would hear it too and howl.
 * AudioManager.getDevices(GET_DEVICES_OUTPUTS), by AudioDeviceInfo type.
 */
inline bool outputIsPrivate() {
  ANativeActivity* a = currentActivity();
  JNIEnv* e = env();
  if (!a || !e) return false;
  bool privateRoute = false;
  jclass ctx = e->GetObjectClass(a->clazz);
  jmethodID getService = e->GetMethodID(ctx, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
  jstring audio = e->NewStringUTF("audio");
  jobject manager = (!failed(e) && getService) ? e->CallObjectMethod(a->clazz, getService, audio) : nullptr;
  if (!failed(e) && manager) {
    jclass managerClass = e->GetObjectClass(manager);
    jmethodID getDevices =
        e->GetMethodID(managerClass, "getDevices", "(I)[Landroid/media/AudioDeviceInfo;");
    auto devices = (jobjectArray) ((!failed(e) && getDevices)
                                       ? e->CallObjectMethod(manager, getDevices, (jint) 2) // GET_DEVICES_OUTPUTS
                                       : nullptr);
    if (!failed(e) && devices) {
      jclass infoClass = e->FindClass("android/media/AudioDeviceInfo");
      jmethodID getType = infoClass ? e->GetMethodID(infoClass, "getType", "()I") : nullptr;
      const jsize n = e->GetArrayLength(devices);
      for (jsize i = 0; i < n && getType && !failed(e); ++i) {
        jobject device = e->GetObjectArrayElement(devices, i);
        const jint type = device ? e->CallIntMethod(device, getType) : 0;
        if (!failed(e)) {
          // TYPE_WIRED_HEADSET 3, WIRED_HEADPHONES 4, BLUETOOTH_A2DP 8,
          // USB_DEVICE 11, USB_HEADSET 22, HEARING_AID 23, BLE_HEADSET 26.
          switch (type) {
            case 3: case 4: case 8: case 11: case 22: case 23: case 26: privateRoute = true; break;
            default: break;
          }
        }
        if (device) e->DeleteLocalRef(device);
      }
      if (infoClass) e->DeleteLocalRef(infoClass);
      e->DeleteLocalRef(devices);
    }
    failed(e);
    e->DeleteLocalRef(managerClass);
    e->DeleteLocalRef(manager);
  }
  e->DeleteLocalRef(audio);
  e->DeleteLocalRef(ctx);
  return privateRoute;
}

/**
 * The system bars and the display cutout over the window, in device pixels:
 * WindowInsets from the decor view (getInsets(systemBars | displayCutout) from
 * Android 11, the system-window insets before). False, all zero, when the
 * view has none yet.
 */
inline bool systemInsets(int* left, int* top, int* right, int* bottom) {
  *left = *top = *right = *bottom = 0;
  ANativeActivity* a = currentActivity();
  JNIEnv* e = env();
  if (!a || !e) return false;
  bool ok = false;
  jclass activityClass = e->GetObjectClass(a->clazz);
  jmethodID getWindow = e->GetMethodID(activityClass, "getWindow", "()Landroid/view/Window;");
  jobject window = (!failed(e) && getWindow) ? e->CallObjectMethod(a->clazz, getWindow) : nullptr;
  if (!failed(e) && window) {
    jclass windowClass = e->GetObjectClass(window);
    jmethodID getDecor = e->GetMethodID(windowClass, "getDecorView", "()Landroid/view/View;");
    jobject decor = (!failed(e) && getDecor) ? e->CallObjectMethod(window, getDecor) : nullptr;
    if (!failed(e) && decor) {
      jclass viewClass = e->GetObjectClass(decor);
      jmethodID getInsets = e->GetMethodID(viewClass, "getRootWindowInsets", "()Landroid/view/WindowInsets;");
      jobject insets = (!failed(e) && getInsets) ? e->CallObjectMethod(decor, getInsets) : nullptr;
      if (!failed(e) && insets) {
        jclass insetsClass = e->GetObjectClass(insets);
        if (android_get_device_api_level() >= 30) {
          jclass type = e->FindClass("android/view/WindowInsets$Type");
          jmethodID bars = type ? e->GetStaticMethodID(type, "systemBars", "()I") : nullptr;
          jmethodID cutout = type ? e->GetStaticMethodID(type, "displayCutout", "()I") : nullptr;
          jmethodID typed = e->GetMethodID(insetsClass, "getInsets", "(I)Landroid/graphics/Insets;");
          if (!failed(e) && bars && cutout && typed) {
            const jint mask = e->CallStaticIntMethod(type, bars) | e->CallStaticIntMethod(type, cutout);
            jobject box = failed(e) ? nullptr : e->CallObjectMethod(insets, typed, mask);
            if (!failed(e) && box) {
              jclass boxClass = e->GetObjectClass(box);
              *left = e->GetIntField(box, e->GetFieldID(boxClass, "left", "I"));
              *top = e->GetIntField(box, e->GetFieldID(boxClass, "top", "I"));
              *right = e->GetIntField(box, e->GetFieldID(boxClass, "right", "I"));
              *bottom = e->GetIntField(box, e->GetFieldID(boxClass, "bottom", "I"));
              ok = !failed(e);
              e->DeleteLocalRef(boxClass);
              e->DeleteLocalRef(box);
            }
          }
          failed(e);
          if (type) e->DeleteLocalRef(type);
        } else {
          const char* names[4] = {"getSystemWindowInsetLeft", "getSystemWindowInsetTop",
                                  "getSystemWindowInsetRight", "getSystemWindowInsetBottom"};
          int* outs[4] = {left, top, right, bottom};
          ok = true;
          for (int k = 0; k < 4 && ok; ++k) {
            jmethodID m = e->GetMethodID(insetsClass, names[k], "()I");
            *outs[k] = (!failed(e) && m) ? e->CallIntMethod(insets, m) : 0;
            ok = !failed(e) && m;
          }
        }
        e->DeleteLocalRef(insetsClass);
        e->DeleteLocalRef(insets);
      }
      failed(e);
      e->DeleteLocalRef(viewClass);
      e->DeleteLocalRef(decor);
    }
    e->DeleteLocalRef(windowClass);
    e->DeleteLocalRef(window);
  }
  failed(e);
  e->DeleteLocalRef(activityClass);
  if (!ok) *left = *top = *right = *bottom = 0;
  return ok;
}

/** Keep the screen on while the app is in front: an instrument being played
 *  should not go dark between notes. Window.addFlags(FLAG_KEEP_SCREEN_ON). */
inline void keepScreenOn(ANativeActivity* activity) {
  ANativeActivity_setWindowFlags(activity, 0x00000080 /* AWINDOW_FLAG_KEEP_SCREEN_ON */, 0);
}

// ── The activity's loop ──────────────────────────────────────────────────────

/** The state of the one activity this process runs, and its looper wiring. */
struct ActivityState {
  ANativeActivity* activity = nullptr;
  ActivityHooks* hooks = nullptr;
  ALooper* looper = nullptr;
  AInputQueue* queue = nullptr;
  ANativeWindow* window = nullptr;
  int frameTimer = -1;
  bool visible = false;
  float scale = 1.0f;
};

inline ActivityState& state() {
  static ActivityState s;
  return s;
}

enum : int { kLooperInput = 1, kLooperFrame = 2, kLooperMessages = 3 };

inline int onInputReady(int, int, void*) {
  ActivityState& s = state();
  if (!s.queue) return 1;
  AInputEvent* event = nullptr;
  while (AInputQueue_getEvent(s.queue, &event) >= 0) {
    // The IME gets first refusal; then the surface, which hands back what it
    // does not use -- the Back key above all.
    if (AInputQueue_preDispatchEvent(s.queue, event)) continue;
    const bool handled = gfx::AndroidSurface::instance().handleInput(event);
    AInputQueue_finishEvent(s.queue, event, handled ? 1 : 0);
  }
  return 1;
}

inline int onFrame(int fd, int, void*) {
  uint64_t expirations = 0;
  (void) ::read(fd, &expirations, sizeof(expirations));
  ActivityState& s = state();
  if (!s.visible) return 1;
  if (s.hooks) s.hooks->frame();
  gfx::AndroidSurface::instance().tick();
  return 1;
}

inline int onMessages(int, int, void*) {
  if (MessageManager* mm = MessageManager::getInstanceWithoutCreating()) mm->dispatchPendingMessages();
  return 1;
}

/** Run the surface's clock at ~60 Hz while visible, and stop it otherwise. */
inline void setFrameClock(bool running) {
  ActivityState& s = state();
  if (s.frameTimer < 0) return;
  itimerspec spec{};
  if (running) {
    spec.it_interval.tv_nsec = 16666667;
    spec.it_value.tv_nsec = 16666667;
  }
  timerfd_settime(s.frameTimer, 0, &spec, nullptr);
}

inline void windowChanged() {
  ActivityState& s = state();
  gfx::AndroidSurface& surface = gfx::AndroidSurface::instance();
  surface.attach(s.window);
  surface.resized();
  int l = 0, t = 0, r = 0, b = 0;
  systemInsets(&l, &t, &r, &b);
  surface.setInsets(l, t, r, b);
  if (s.hooks && s.window) s.hooks->windowReady(surface.contentWidth(), surface.contentHeight(), s.scale);
}

// The NativeActivity callbacks, every one on the main thread.
inline void onStart(ANativeActivity*) {
  ActivityState& s = state();
  s.visible = true;
  setFrameClock(true);
  if (s.hooks) s.hooks->started();
}
inline void onResume(ANativeActivity*) {
  if (state().hooks) state().hooks->resumed();
}
inline void onPause(ANativeActivity*) {
  if (state().hooks) state().hooks->paused();
}
inline void onStop(ANativeActivity*) {
  ActivityState& s = state();
  s.visible = false;
  setFrameClock(false);
  if (s.hooks) s.hooks->stopped();
}
inline void onWindowCreated(ANativeActivity*, ANativeWindow* window) {
  state().window = window;
  windowChanged();
}
inline void onWindowResized(ANativeActivity*, ANativeWindow*) { windowChanged(); }
inline void onWindowRedrawNeeded(ANativeActivity*, ANativeWindow*) {
  gfx::AndroidSurface::instance().invalidate();
  gfx::AndroidSurface::instance().present();
}
/** The bars appeared, went or moved: the editor follows the free area. */
inline void onContentRectChanged(ANativeActivity*, const ARect*) {
  if (state().window) windowChanged();
}
inline void onWindowDestroyed(ANativeActivity*, ANativeWindow*) {
  ActivityState& s = state();
  if (s.hooks) s.hooks->windowLost();
  gfx::AndroidSurface::instance().detach();
  s.window = nullptr;
}
inline void onInputQueueCreated(ANativeActivity*, AInputQueue* queue) {
  ActivityState& s = state();
  s.queue = queue;
  AInputQueue_attachLooper(queue, s.looper, kLooperInput, &onInputReady, nullptr);
}
inline void onInputQueueDestroyed(ANativeActivity*, AInputQueue* queue) {
  AInputQueue_detachLooper(queue);
  state().queue = nullptr;
}
inline void onConfigurationChanged(ANativeActivity* activity) {
  ActivityState& s = state();
  s.scale = densityScale(activity);
  gfx::AndroidSurface::instance().setDensityScale(s.scale);
  if (s.window) windowChanged();
}
inline void onDestroy(ANativeActivity*) {
  ActivityState& s = state();
  setFrameClock(false);
  if (s.hooks) {
    s.hooks->destroyed();
    delete s.hooks;
    s.hooks = nullptr;
  }
  if (s.frameTimer >= 0) {
    ALooper_removeFd(s.looper, s.frameTimer);
    ::close(s.frameTimer);
    s.frameTimer = -1;
  }
  if (MessageManager* mm = MessageManager::getInstanceWithoutCreating())
    if (mm->wakeUpFileDescriptor() >= 0) ALooper_removeFd(s.looper, mm->wakeUpFileDescriptor());
  gfx::AndroidSurface::instance().detach();
  currentActivity() = nullptr;
  s.activity = nullptr;
}

/** Wire an activity to `hooks` (taken over: deleted when the activity ends). */
inline void run(ANativeActivity* activity, ActivityHooks* hooks) {
  ActivityState& s = state();
  s.activity = activity;
  s.hooks = hooks;
  currentActivity() = activity;
  s.looper = ALooper_forThread(); // the main thread's: onCreate runs on it

  // Where the app's files go: HOME and TMPDIR, read by every path the SDK
  // makes (user_settings.h, user_presets.h, file_types.h, process.h).
  if (activity->internalDataPath && activity->internalDataPath[0])
    ::setenv("HOME", activity->internalDataPath, 1);
  const std::string cache = cacheDir(activity);
  ::setenv("TMPDIR", !cache.empty() ? cache.c_str()
                                    : (activity->internalDataPath ? activity->internalDataPath : "/data/local/tmp"),
           1);

  s.scale = densityScale(activity);
  gfx::AndroidSurface::instance().setDensityScale(s.scale);

  // The message thread is this one: whoever asks first owns it.
  if (MessageManager* mm = MessageManager::getInstance())
    if (mm->wakeUpFileDescriptor() >= 0)
      ALooper_addFd(s.looper, mm->wakeUpFileDescriptor(), kLooperMessages, ALOOPER_EVENT_INPUT, &onMessages,
                    nullptr);
  s.frameTimer = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (s.frameTimer >= 0)
    ALooper_addFd(s.looper, s.frameTimer, kLooperFrame, ALOOPER_EVENT_INPUT, &onFrame, nullptr);

  ANativeActivityCallbacks* cb = activity->callbacks;
  cb->onStart = &onStart;
  cb->onResume = &onResume;
  cb->onPause = &onPause;
  cb->onStop = &onStop;
  cb->onDestroy = &onDestroy;
  cb->onNativeWindowCreated = &onWindowCreated;
  cb->onNativeWindowResized = &onWindowResized;
  cb->onNativeWindowRedrawNeeded = &onWindowRedrawNeeded;
  cb->onNativeWindowDestroyed = &onWindowDestroyed;
  cb->onInputQueueCreated = &onInputQueueCreated;
  cb->onInputQueueDestroyed = &onInputQueueDestroyed;
  cb->onConfigurationChanged = &onConfigurationChanged;
  cb->onContentRectChanged = &onContentRectChanged;

  if (s.hooks) s.hooks->created(activity);
}

} // namespace android
} // namespace sonore

#endif // __ANDROID__
