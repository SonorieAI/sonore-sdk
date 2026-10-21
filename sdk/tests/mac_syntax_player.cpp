// SPDX-License-Identifier: Apache-2.0
// The player example's macOS build -- the AppKit application, the NSWindow
// frame, Core Audio output -- compiled on Windows. Nothing here runs; the
// compiler reading it is the test, against tests/au_shim. An OBJECT library:
// the main() SONORE_START_APPLICATION writes is compiled, never linked.
#if !defined(__APPLE__) || defined(_WIN32)
#error "build with __APPLE__ defined and _WIN32 undefined: see sonore_mac_syntax_* in CMakeLists.txt"
#endif

#include "../examples/player/main.cpp"

// Proves the Core Audio branch was the one read.
static_assert(sizeof(sonore::standalone::CoreAudioOutput) > 0, "the macOS branch");
