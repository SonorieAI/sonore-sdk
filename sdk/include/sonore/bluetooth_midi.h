// SPDX-License-Identifier: Apache-2.0
//
// Pairing a Bluetooth LE MIDI device: the system's own dialogue, where there is
// one.
//
// ── Why this is a thin file ─────────────────────────────────────────────────
//
// Bluetooth LE MIDI is paired by the OPERATING SYSTEM, and every platform that
// supports it already has the screen for it. A plugin that drew its own would
// have to scan, pair and bond -- the half of Bluetooth that needs permissions,
// entitlements and a user who trusts a plugin with them -- to arrive at a
// worse copy of a dialogue the user has seen before. So this opens the
// system's, and says plainly where there is none.
//
//  - macOS: CoreAudioKit's CABTLEMIDIWindowController (10.11 and later), shown
//    MODELESSLY -- the host keeps running, and a modal loop inside a plugin is
//    a loop the host is not running. A device connected there appears in
//    CoreMIDI, which is what MidiInput reads on a Mac. COMPILED against the
//    shim in tests/au_shim, never run: no Mac in this loop.
//
//  - Windows: Settings > Bluetooth & devices (the ms-settings:bluetooth page),
//    opened with ShellExecuteW. RUN here, up to the launch (the tests set
//    SONORE_BLUETOOTH_MIDI_NO_LAUNCH so a test run does not open Settings).
//    Said plainly because it matters: a BLE MIDI device paired there is
//    reached through Windows.Devices.Midi, and this SDK's MidiInput uses WinMM,
//    which does not list Bluetooth LE MIDI ports. Listing them here through
//    Windows.Devices.Enumeration was considered and NOT built: it would name
//    devices MidiInput cannot then open, and finding UNPAIRED ones needs an
//    association-endpoint watcher running for seconds -- a WinRT subsystem
//    for a list the Settings page already shows.
//
//  - Linux: none. Pairing is BlueZ over D-Bus (org.bluez.Device1.Pair), which
//    this SDK does not link, and there is no system dialogue to open that
//    every desktop shares. isAvailable() is false and the reason says what to
//    do instead.
#pragma once

#include <cstdlib>
#include <functional>
#include <string>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__) || defined(SONORE_APPLE_SYNTAX_CHECK)
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/objc.h>
#include <objc/runtime.h>

#include <cstdio>
#include <cstdint>
#endif

namespace sonore {

namespace detail {

/** Set (to anything but "0") by tests: open() does everything but launch. */
inline bool bluetoothMidiLaunchDisabled() {
  const char* value = std::getenv("SONORE_BLUETOOTH_MIDI_NO_LAUNCH");
  return value && *value && !(value[0] == '0' && value[1] == 0);
}

/** Why there is no dialogue on Linux, and what a user does instead. A
 *  function rather than a branch so a test on any platform can read it. */
inline const char* bluetoothMidiLinuxReason() {
  return "Bluetooth MIDI pairing on Linux goes through BlueZ over D-Bus "
         "(org.bluez.Device1.Pair), which this SDK does not link, and there is no "
         "system dialogue every desktop shares. Pair the device in the desktop's "
         "Bluetooth settings or with bluetoothctl; once connected, BlueZ's MIDI "
         "profile makes it an ALSA sequencer port, which can be connected to this "
         "plugin's MIDI input (aconnect).";
}

#if defined(_WIN32)
/** A system DLL, from System32 only -- never the application's directory or
 *  the current one, where a planted copy would be loaded instead. */
inline HMODULE bluetoothMidiSystemLibrary(const wchar_t* name) {
  return LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
}

/** Whether the ms-settings: protocol is registered -- Windows 10 and later,
 *  minus images that strip Settings. RegOpenKeyExW comes from advapi32 at
 *  RUNTIME, so including this header adds nothing to a plugin's link line. */
inline bool bluetoothMidiSettingsRegistered() {
  using OpenFn = LONG(WINAPI*)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
  using CloseFn = LONG(WINAPI*)(HKEY);
  HMODULE advapi = bluetoothMidiSystemLibrary(L"advapi32.dll");
  if (!advapi) return false;
  auto openKey = (OpenFn) (void*) GetProcAddress(advapi, "RegOpenKeyExW");
  auto closeKey = (CloseFn) (void*) GetProcAddress(advapi, "RegCloseKey");
  bool found = false;
  if (openKey && closeKey) {
    HKEY key = nullptr;
    if (openKey(HKEY_CLASSES_ROOT, L"ms-settings", 0, KEY_READ, &key) == ERROR_SUCCESS) {
      found = true;
      closeKey(key);
    }
  }
  FreeLibrary(advapi);
  return found;
}
#endif

#if defined(__APPLE__) || defined(SONORE_APPLE_SYNTAX_CHECK)
template <typename Ret, typename... Args>
inline Ret bluetoothMidiSend(id target, SEL selector, Args... args) {
  using Fn = Ret (*)(id, SEL, Args...);
  return reinterpret_cast<Fn>(objc_msgSend)(target, selector, args...);
}

/** CoreAudioKit, loaded on first use: no project has to name the framework,
 *  and a plugin that never shows the dialogue never loads it. */
inline Class bluetoothMidiControllerClass() {
  static Class klass = []() -> Class {
    if (Class existing = objc_getClass("CABTLEMIDIWindowController")) return existing;
    if (!dlopen("/System/Library/Frameworks/CoreAudioKit.framework/CoreAudioKit", RTLD_LAZY))
      return nullptr;
    return objc_getClass("CABTLEMIDIWindowController");
  }();
  return klass;
}

/** The one dialogue that may be open, and who to tell when it closes. */
struct BluetoothMidiOpenDialogue {
  id controller = nullptr;
  id window = nullptr;
  std::function<void(bool)> onClosed;
};

inline BluetoothMidiOpenDialogue& bluetoothMidiOpenDialogue() {
  static BluetoothMidiOpenDialogue state;
  return state;
}

/** NSWindowWillCloseNotification, for the dialogue's window: stop
 *  observing, let the controller go (autoreleased -- it is still on the
 *  stack that is closing its window), and tell the caller. */
inline void bluetoothMidiWindowWillClose(id self, SEL, id) {
  BluetoothMidiOpenDialogue& open = bluetoothMidiOpenDialogue();
  id centre = bluetoothMidiSend<id>((id) objc_getClass("NSNotificationCenter"),
                                    sel_registerName("defaultCenter"));
  if (centre) bluetoothMidiSend<void>(centre, sel_registerName("removeObserver:"), self);
  if (open.controller) bluetoothMidiSend<id>(open.controller, sel_registerName("autorelease"));
  std::function<void(bool)> done = std::move(open.onClosed);
  open = BluetoothMidiOpenDialogue();
  if (done) done(true);
}

/** The observer: an NSObject subclass with that one method, made at runtime
 *  as window_cocoa.h makes its view, and named after this binary for the
 *  same reason. */
inline id bluetoothMidiObserver() {
  static id observer = []() -> id {
    char name[96];
    std::snprintf(name, sizeof(name), "SonoreBtMidiObserver_%llx",
                  (unsigned long long) (uintptr_t) (void*) &bluetoothMidiWindowWillClose);
    Class c = objc_getClass(name);
    if (!c) {
      c = objc_allocateClassPair(objc_getClass("NSObject"), name, 0);
      if (!c) return nullptr;
      class_addMethod(c, sel_registerName("sonoreBluetoothWindowWillClose:"),
                      (IMP) bluetoothMidiWindowWillClose, "v@:@");
      objc_registerClassPair(c);
    }
    id made = bluetoothMidiSend<id>((id) c, sel_registerName("alloc"));
    return made ? bluetoothMidiSend<id>(made, sel_registerName("init")) : nullptr;
  }();
  return observer;
}
#endif

} // namespace detail

/**
 * The system's Bluetooth LE MIDI pairing dialogue.
 *
 * Named as the reference names it; static, because there is one system
 * dialogue and nothing for an instance to hold.
 */
class BluetoothMidiDevicePairingDialogue {
public:
  /** Whether open() can show something on this machine. */
  static bool isAvailable() { return unavailableReason().empty(); }

  /** Empty when available; otherwise why not, in words a user can act on. */
  static std::string unavailableReason() {
#if defined(_WIN32)
    if (!detail::bluetoothMidiSettingsRegistered())
      return "the ms-settings: protocol is not registered, so there is no Settings page "
             "to open (Windows 10 or later is needed)";
    return {};
#elif defined(__APPLE__) || defined(SONORE_APPLE_SYNTAX_CHECK)
    if (!detail::bluetoothMidiControllerClass())
      return "CoreAudioKit's CABTLEMIDIWindowController is not available (macOS 10.11 or later)";
    return {};
#elif defined(__linux__)
    return detail::bluetoothMidiLinuxReason();
#else
    return "this platform has no Bluetooth MIDI pairing dialogue";
#endif
  }

  /** What open() shows here, for a UI that wants to say before it does. */
  static const char* description() {
#if defined(_WIN32)
    return "Windows Settings > Bluetooth & devices. A Bluetooth LE MIDI device paired there "
           "is reached through Windows.Devices.Midi; MidiInput uses WinMM, which does not "
           "list Bluetooth LE MIDI ports.";
#elif defined(__APPLE__) || defined(SONORE_APPLE_SYNTAX_CHECK)
    return "the system's Bluetooth MIDI window (CABTLEMIDIWindowController); a device "
           "connected there appears in CoreMIDI.";
#else
    return "none";
#endif
  }

  /**
   * Show it, without blocking. False when nothing could be shown -- the reason
   * is unavailableReason() -- and then `onClosed` is not called.
   *
   * `onClosed(true)` is called when the dialogue closes. On Windows that is as
   * soon as Settings has been launched: it is another process, and its window
   * closing is nothing this one is told about. On macOS it is when the window
   * closes; opening while it is already open brings it forward and keeps the
   * first caller's callback.
   */
  static bool open(std::function<void(bool)> onClosed = {}) {
    if (!isAvailable()) return false;
#if defined(_WIN32)
    if (!detail::bluetoothMidiLaunchDisabled()) {
      using ShellExecuteFn = HINSTANCE(WINAPI*)(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT);
      HMODULE shell = detail::bluetoothMidiSystemLibrary(L"shell32.dll");
      if (!shell) return false;
      auto execute = (ShellExecuteFn) (void*) GetProcAddress(shell, "ShellExecuteW");
      // Greater than 32 is success: ShellExecute's documented, ancient,
      // return convention.
      const bool launched =
          execute && (INT_PTR) execute(nullptr, L"open", L"ms-settings:bluetooth", nullptr, nullptr,
                                       SW_SHOWNORMAL) > 32;
      FreeLibrary(shell);
      if (!launched) return false;
    }
    if (onClosed) onClosed(true);
    return true;
#elif defined(__APPLE__) || defined(SONORE_APPLE_SYNTAX_CHECK)
    using detail::bluetoothMidiSend;
    detail::BluetoothMidiOpenDialogue& state = detail::bluetoothMidiOpenDialogue();
    if (state.window) {
      bluetoothMidiSend<void>(state.window, sel_registerName("makeKeyAndOrderFront:"), (id) nullptr);
      return true;
    }
    if (detail::bluetoothMidiLaunchDisabled()) {
      if (onClosed) onClosed(true);
      return true;
    }
    id controller = bluetoothMidiSend<id>((id) detail::bluetoothMidiControllerClass(),
                                          sel_registerName("alloc"));
    controller = controller ? bluetoothMidiSend<id>(controller, sel_registerName("init")) : nullptr;
    if (!controller) return false;
    id window = bluetoothMidiSend<id>(controller, sel_registerName("window"));
    id observer = detail::bluetoothMidiObserver();
    id centre = bluetoothMidiSend<id>((id) objc_getClass("NSNotificationCenter"),
                                      sel_registerName("defaultCenter"));
    id name = bluetoothMidiSend<id>((id) objc_getClass("NSString"),
                                    sel_registerName("stringWithUTF8String:"),
                                    "NSWindowWillCloseNotification");
    if (!window || !observer || !centre || !name) {
      // Without the window's close, the controller could never be released
      // and the caller never told: refuse rather than leak and stay silent.
      bluetoothMidiSend<void>(controller, sel_registerName("release"));
      return false;
    }
    bluetoothMidiSend<void>(centre, sel_registerName("addObserver:selector:name:object:"), observer,
                            sel_registerName("sonoreBluetoothWindowWillClose:"), name, window);
    state.controller = controller;
    state.window = window;
    state.onClosed = std::move(onClosed);
    bluetoothMidiSend<void>(controller, sel_registerName("showWindow:"), (id) nullptr);
    return true;
#else
    (void) onClosed;
    return false;
#endif
  }
};

} // namespace sonore
