// SPDX-License-Identifier: Apache-2.0
// The application an AUv3 extension ships inside, on macOS (sonore_add_auv3).
//
// The extension is the product; this app is its envelope. Opening it once
// registers the extension with the system, and then every Audio Unit host
// lists the plugin. So all it does is say that, in one alert, and quit.
// `--quit` skips the alert (CI registers the extension with pluginkit and
// has no one to click OK).
//
// Plain C++ against the Objective-C runtime, like every Cocoa file in the SDK.
#include <objc/message.h>
#include <objc/runtime.h>

#include <cstring>

#ifndef SONORE_AUV3_CONTAINER_TITLE
#define SONORE_AUV3_CONTAINER_TITLE "Audio Unit"
#endif

namespace {

template <typename Ret, typename... Args>
Ret send(id target, const char* selector, Args... args) {
  using Fn = Ret (*)(id, SEL, Args...);
  return reinterpret_cast<Fn>(objc_msgSend)(target, sel_registerName(selector), args...);
}

id cls(const char* name) { return (id) objc_getClass(name); }

id text(const char* utf8) { return send<id>(cls("NSString"), "stringWithUTF8String:", utf8); }

} // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i)
    if (std::strcmp(argv[i], "--quit") == 0) return 0;
  id pool = send<id>(send<id>(cls("NSAutoreleasePool"), "alloc"), "init");
  id app = send<id>(cls("NSApplication"), "sharedApplication");
  send<void>(app, "setActivationPolicy:", (long) 0); // regular: a Dock icon while the alert shows
  send<void>(app, "activateIgnoringOtherApps:", (signed char) 1);
  id alert = send<id>(send<id>(cls("NSAlert"), "alloc"), "init");
  send<void>(alert, "setMessageText:", text(SONORE_AUV3_CONTAINER_TITLE " is installed"));
  send<void>(alert, "setInformativeText:",
             text("Its Audio Unit is now available to Logic Pro, GarageBand and every other host on "
                  "this Mac. Keep this app in Applications: the plugin lives inside it."));
  send<long>(alert, "runModal");
  send<void>(alert, "release");
  send<void>(pool, "drain");
  return 0;
}
