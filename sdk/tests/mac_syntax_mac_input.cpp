// SPDX-License-Identifier: Apache-2.0
// The Mac halves of screen-reader access and input methods -- the
// NSAccessibility bridge (gfx/accessibility_mac.h), NSTextInputClient in the
// Cocoa view (gfx/window_cocoa.h), the Bluetooth MIDI window
// (bluetooth_midi.h) and the self-test a Mac developer runs
// (tests/mac_input_selftest.cpp) -- compiled on Windows. Nothing here runs;
// the compiler reading it is the test, exactly as for mac_syntax_plugin.cpp,
// whose comment says what that proves (every name exists and every call fits
// its declaration in tests/au_shim) and what it does not (that the
// declarations match Apple's ABI, or that VoiceOver hears anything).
//
// The self-test is included rather than listed beside this file so that every
// Objective-C runtime path it drives -- the attribute API, NSTextInputClient's
// struct-passing selectors, the key event it synthesises -- is compiled on
// every build, not only on the Mac that nobody here has.
#if !defined(__APPLE__) || defined(_WIN32)
#error "build with __APPLE__ defined and _WIN32 undefined: see sonore_mac_syntax_* in CMakeLists.txt"
#endif

#include "mac_input_selftest.cpp"

// The Apple branches, not the Windows ones: these exist only there.
static_assert(sizeof(sonore::gfx::nsax::MacAccessibilityRoot) > 0, "the macOS bridge was compiled");
static_assert(sizeof(sonore::gfx::cocoa::NSRange2) == 2 * sizeof(unsigned long), "NSRange is two NSUIntegers");
static_assert(sizeof(sonore::gfx::nsax::AxRect) == 4 * sizeof(double), "NSRect is four CGFloats");
