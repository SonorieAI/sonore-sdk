// SPDX-License-Identifier: Apache-2.0
// A plugin's macOS build -- CLAP, VST3, AU and LV2 in one, with the Cocoa
// window, the WKWebView host and au_view.h -- compiled on Windows. Nothing
// here runs; the compiler reading it is the test.
//
// macOS CI has not started a job since the account's spending limit was
// reached, and the Mac branches of the wrappers had no other reader:
// apple_syntax.cpp, au_syntax.cpp and cocoa_syntax.cpp parse the backends,
// the AU wrapper and the Cocoa files one by one, but every header that
// chooses a platform -- clap_wrapper.h first among them -- takes its _WIN32
// branch on this machine, so au_view.h and the wrappers' Mac halves were
// never read. sonore_mac_syntax_plugin builds this file with __APPLE__ defined and
// _WIN32 undefined instead, against tests/au_shim (Apple's declarations) and
// tests/au_shim/msvc (the POSIX names MSVC lacks). What that proves is what
// au_shim/README.md says: every name exists and every call fits its
// declaration -- not that the declarations match Apple's ABI.
#if !defined(__APPLE__) || defined(_WIN32)
#error "build with __APPLE__ defined and _WIN32 undefined: see sonore_mac_syntax_* in CMakeLists.txt"
#endif

#define SONORE_BUILD_VST3 1
#define SONORE_BUILD_AU 1
#define SONORE_BUILD_LV2 1
#include "../examples/saturator/plugin.cpp"
