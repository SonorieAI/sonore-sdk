// SPDX-License-Identifier: Apache-2.0
// The macOS standalone -- runInteractive, the device hub, CoreAudio and
// CoreMIDI, the Cocoa window -- compiled on Windows, as mac_syntax_plugin.cpp
// compiles a plugin; see the note there. An instrument, so the keyboard and
// MIDI paths are read too.
#if !defined(__APPLE__) || defined(_WIN32)
#error "build with __APPLE__ defined and _WIN32 undefined: see sonore_mac_syntax_* in CMakeLists.txt"
#endif

#define SONORE_BUILD_STANDALONE 1
#define SONORE_STANDALONE_NO_MAIN 1
#include "../examples/synth/plugin.cpp"
