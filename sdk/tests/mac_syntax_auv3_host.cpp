// SPDX-License-Identifier: Apache-2.0
// The AUv3 self-test's host side (tests/auv3_selftest.cpp), compiled on
// Windows against tests/au_shim: it runs on CI's Mac and in the iOS
// simulator, and this is the reader it has between those runs.
#if !defined(__APPLE__) || defined(_WIN32)
#error "build with __APPLE__ defined and _WIN32 undefined: see sonore_mac_syntax_* in CMakeLists.txt"
#endif

#include "auv3_selftest.cpp"
