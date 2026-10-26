// SPDX-License-Identifier: Apache-2.0
// A plugin's AUv3 build -- auv3_wrapper.h, the AUAudioUnit subclass and its
// view controller -- compiled on Windows, as the Mac and the iPhone would
// read it. Nothing here runs; the compiler reading it is the test, against
// tests/au_shim, with what au_shim/README.md says that proves.
//
// Built twice by sonore_mac_syntax_auv3 and sonore_ios_syntax_auv3: the Mac
// halves, and with SONORE_IOS_SYNTAX_CHECK the UIKit ones. The synth, because
// it reads MIDI and has the most parameters; the saturator's effect path is
// read by the AUv2 check beside this one and by the AUv3 self-test on CI.
#if !defined(__APPLE__) || defined(_WIN32)
#error "build with __APPLE__ defined and _WIN32 undefined: see sonore_mac_syntax_* in CMakeLists.txt"
#endif

#define SONORE_BUILD_AUV3 1
#include "../examples/synth/plugin.cpp"
