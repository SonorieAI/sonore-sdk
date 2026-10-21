// SPDX-License-Identifier: Apache-2.0
// The showcase example's macOS build -- CLAP, VST3, AU and LV2, its own
// native editor in the Cocoa window -- compiled on Windows, as
// mac_syntax_plugin.cpp compiles the saturator. Nothing here runs; the
// compiler reading it is the test, against tests/au_shim (au_shim/README.md
// says what that proves and what it does not).
#if !defined(__APPLE__) || defined(_WIN32)
#error "build with __APPLE__ defined and _WIN32 undefined: see sonore_mac_syntax_* in CMakeLists.txt"
#endif

#define SONORE_BUILD_VST3 1
#define SONORE_BUILD_AU 1
#define SONORE_BUILD_LV2 1
#include "../examples/showcase/plugin.cpp"

// Proves the Cocoa branch was the one read: this peer exists only there.
static_assert(sizeof(sonore::gfx::NativeWindowCocoa) > 0, "the macOS branch");
