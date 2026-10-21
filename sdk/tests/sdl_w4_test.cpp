// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the synth, compiled the way a new Visual Studio project
// compiles it: /W4 /WX /sdl, and WITHOUT the _CRT_SECURE_NO_WARNINGS the CMake
// target hands its users, so any CRT call the headers make that MSVC calls
// unsafe (C4996) fails this build. A project that does not take the define
// from CMake -- a plain .vcxproj, another build system -- sees exactly this.
#undef _CRT_SECURE_NO_WARNINGS
#include "../examples/synth/plugin.cpp"
