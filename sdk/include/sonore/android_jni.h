// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the JNI an Android app's C++ uses -- the activity, its thread's
// JNIEnv, and the SDK's own Java class.
//
// The framework is reached through JNI on its own classes (android_app.h asks
// for the density, the insets, the permissions that way). What only Java can
// do -- implement a framework callback interface -- is in SonoreBridge
// (sdk/android/java), which sonore_add_standalone compiles into the APK's
// classes.dex. FindClass from native code searches the SYSTEM class loader,
// which has never heard of an app's own classes, so SonoreBridge is loaded
// through the activity's class loader instead, once, and kept.
//
// Every call here is for the activity's main thread unless it says otherwise:
// that is the thread whose JNIEnv the activity hands over.
#pragma once

#if defined(__ANDROID__)

#include <android/native_activity.h>
#include <jni.h>

#include <string>

namespace sonore {
namespace android {

/** The activity this process is showing, while it shows one. */
inline ANativeActivity*& currentActivity() {
  static ANativeActivity* activity = nullptr;
  return activity;
}

/** The main thread's JNIEnv: the one every activity callback runs on. */
inline JNIEnv* env() {
  ANativeActivity* a = currentActivity();
  return a ? a->env : nullptr;
}

/** Whether a Java exception is pending; cleared either way, so one failed
 *  call never poisons the next. */
inline bool failed(JNIEnv* e) {
  if (!e->ExceptionCheck()) return false;
  e->ExceptionClear();
  return true;
}

/** A JNIEnv for THIS thread, attaching it to the VM if it is not yet: for a
 *  callback Java makes on a thread of its own. */
inline JNIEnv* envForThisThread() {
  ANativeActivity* a = currentActivity();
  if (!a || !a->vm) return nullptr;
  JNIEnv* e = nullptr;
  if (a->vm->GetEnv((void**) &e, JNI_VERSION_1_6) == JNI_OK) return e;
  if (a->vm->AttachCurrentThread(&e, nullptr) != JNI_OK) return nullptr;
  return e;
}

/**
 * com.sonorie.sonore.SonoreBridge, through the activity's class loader, as a
 * global reference; null when the APK carries no classes.dex (a build without
 * a JDK, or the command-line program, which has no activity at all).
 */
inline jclass bridgeClass() {
  static jclass cached = nullptr;
  static bool tried = false;
  if (tried) return cached;
  ANativeActivity* a = currentActivity();
  JNIEnv* e = env();
  if (!a || !e) return nullptr;
  tried = true;
  jclass activityClass = e->GetObjectClass(a->clazz);
  jmethodID getLoader = e->GetMethodID(activityClass, "getClassLoader", "()Ljava/lang/ClassLoader;");
  jobject loader = (!failed(e) && getLoader) ? e->CallObjectMethod(a->clazz, getLoader) : nullptr;
  if (!failed(e) && loader) {
    jclass loaderClass = e->GetObjectClass(loader);
    jmethodID loadClass = e->GetMethodID(loaderClass, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    jstring name = e->NewStringUTF("com.sonorie.sonore.SonoreBridge");
    auto local = (jclass) ((!failed(e) && loadClass) ? e->CallObjectMethod(loader, loadClass, name) : nullptr);
    if (!failed(e) && local) {
      cached = (jclass) e->NewGlobalRef(local);
      e->DeleteLocalRef(local);
    }
    e->DeleteLocalRef(name);
    e->DeleteLocalRef(loaderClass);
    e->DeleteLocalRef(loader);
  }
  failed(e);
  e->DeleteLocalRef(activityClass);
  return cached;
}

/** Bind native implementations to SonoreBridge's native methods. False when
 *  there is no SonoreBridge, or it does not declare them. */
inline bool registerNatives(const JNINativeMethod* methods, int count) {
  jclass c = bridgeClass();
  JNIEnv* e = env();
  if (!c || !e) return false;
  const bool ok = e->RegisterNatives(c, methods, count) == JNI_OK;
  failed(e);
  return ok;
}

/** A Java string, as UTF-8. */
inline std::string utf8(JNIEnv* e, jstring s) {
  if (!e || !s) return std::string();
  const char* chars = e->GetStringUTFChars(s, nullptr);
  std::string out = chars ? chars : "";
  if (chars) e->ReleaseStringUTFChars(s, chars);
  return out;
}

} // namespace android
} // namespace sonore

#endif // __ANDROID__
