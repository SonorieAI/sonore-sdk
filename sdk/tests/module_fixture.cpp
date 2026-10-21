// SPDX-License-Identifier: Apache-2.0
// Sonore SDK test fixture: a VST3 module that counts its entry and exit calls.
//
// A VST3 binary's module entry (InitDll / bundleEntry / ModuleEntry) and exit
// (ExitDll / bundleExit / ModuleExit) come in pairs: an exit answers an entry
// that succeeded, and nothing else. sonore/host.h called ExitDll after an
// InitDll that had refused, and on macOS and Linux never called the exit at
// all. This module counts both in the process environment, which it shares
// with the test that loads it, and refuses its entry when
// SONORE_MODULE_FIXTURE_REFUSE is "1". It publishes no factory: entry and exit
// are all it is for.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define FIXTURE_EXPORT extern "C" __declspec(dllexport)
#else
#define FIXTURE_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

int read(const char* name) {
  char text[32] = {0};
#if defined(_WIN32)
  GetEnvironmentVariableA(name, text, (DWORD) sizeof(text));
#else
  if (const char* v = std::getenv(name)) std::snprintf(text, sizeof(text), "%s", v);
#endif
  return std::atoi(text);
}

void bump(const char* name) {
  char text[32];
  std::snprintf(text, sizeof(text), "%d", read(name) + 1);
#if defined(_WIN32)
  SetEnvironmentVariableA(name, text);
#else
  setenv(name, text, 1);
#endif
}

bool enter() {
  bump("SONORE_MODULE_FIXTURE_ENTRIES");
  return read("SONORE_MODULE_FIXTURE_REFUSE") != 1;
}

bool leave() {
  bump("SONORE_MODULE_FIXTURE_EXITS");
  return true;
}

} // namespace

#if defined(_WIN32)
FIXTURE_EXPORT bool InitDll() { return enter(); }
FIXTURE_EXPORT bool ExitDll() { return leave(); }
#elif defined(__APPLE__)
FIXTURE_EXPORT bool bundleEntry(void*) { return enter(); }
FIXTURE_EXPORT bool bundleExit() { return leave(); }
#else
FIXTURE_EXPORT bool ModuleEntry(void* handle) {
  // The library's own handle, which the VST3 SDK keeps to find its resources.
  if (!handle) bump("SONORE_MODULE_FIXTURE_NULL_HANDLE");
  return enter();
}
FIXTURE_EXPORT bool ModuleExit() { return leave(); }
#endif

FIXTURE_EXPORT void* GetPluginFactory() { return nullptr; }
