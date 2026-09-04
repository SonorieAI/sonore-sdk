// SPDX-License-Identifier: Apache-2.0
//
// What machine this is.
//
// ── Why a plugin wants it ───────────────────────────────────────────────────
//
// logger.h gives a user something to send you. This is the first line of it.
// "It crashes on my machine" and "it crashes on Windows 11 24H2, Ryzen 7,
// 32 GB, no AVX-512" are different bug reports, and only one of them can be
// acted on without a conversation.
//
// It is also the honest answer to "why is this slow on my laptop": core count
// and CPU model explain most of it, and neither is something a user can be
// asked to look up.
//
// ── Runtime, not compile time ───────────────────────────────────────────────
//
// simd.h decides at COMPILE time which instruction set to use, which is right
// for it -- a plugin ships one binary and its DSP is whatever it was built
// with. This asks the CPU what it actually IS, which is a different question:
// a binary built for SSE2 running on a machine with AVX2 is a fact worth
// knowing when somebody reports the CPU meter reading twice what it should.
//
// ── Nothing is guessed ──────────────────────────────────────────────────────
//
// Anything a platform will not say comes back empty or zero. A fabricated OS
// version in a bug report is worse than a blank one: it sends whoever reads it
// looking for a bug on a system nobody was using.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <unistd.h>
#else
#include <sys/utsname.h>
#include <unistd.h>
#endif

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#define SONORE_HAVE_CPUID 1
#elif (defined(__x86_64__) || defined(__i386__)) && !defined(__EMSCRIPTEN__)
#include <cpuid.h>
#define SONORE_HAVE_CPUID 1
#else
#define SONORE_HAVE_CPUID 0
#endif

namespace sonore {

/** What the CPU can do, as it is RIGHT NOW rather than as the binary was
 *  built. See the header for why the two are different questions. */
struct CpuFeatures {
  bool sse2 = false;
  bool sse41 = false;
  bool avx = false;
  bool avx2 = false;
  bool fma = false;
  bool neon = false;
};

namespace statsdetail {

#if SONORE_HAVE_CPUID
inline void cpuid(int leaf, int subleaf, int out[4]) {
#if defined(_MSC_VER)
  __cpuidex(out, leaf, subleaf);
#else
  unsigned int a = 0, b = 0, c = 0, d = 0;
  __get_cpuid_count((unsigned int) leaf, (unsigned int) subleaf, &a, &b, &c, &d);
  out[0] = (int) a;
  out[1] = (int) b;
  out[2] = (int) c;
  out[3] = (int) d;
#endif
}
#endif

} // namespace statsdetail

inline CpuFeatures cpuFeatures() {
  CpuFeatures out;
#if defined(__ARM_NEON) || defined(__aarch64__) || defined(_M_ARM64)
  // Every arm64 processor has NEON -- it is part of the base architecture, not
  // an extension -- so there is nothing to detect and nothing that could be
  // absent.
  out.neon = true;
#endif
#if SONORE_HAVE_CPUID
  int info[4] = {0, 0, 0, 0};
  statsdetail::cpuid(0, 0, info);
  const int highestLeaf = info[0];

  if (highestLeaf >= 1) {
    statsdetail::cpuid(1, 0, info);
    out.sse2 = (info[3] & (1 << 26)) != 0;
    out.sse41 = (info[2] & (1 << 19)) != 0;
    out.fma = (info[2] & (1 << 12)) != 0;
    // AVX needs the OS to have enabled the wider registers as well as the CPU
    // to have them: OSXSAVE, then XCR0's low two bits. A plugin that used AVX
    // on the strength of the CPU bit alone would fault on a machine whose
    // kernel does not save those registers, which is rare and real.
    const bool osxsave = (info[2] & (1 << 27)) != 0;
    const bool cpuAvx = (info[2] & (1 << 28)) != 0;
    if (osxsave && cpuAvx) {
#if defined(_MSC_VER)
      const unsigned long long xcr0 = _xgetbv(0);
#else
      unsigned int lo = 0, hi = 0;
      __asm__ __volatile__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
      const unsigned long long xcr0 = ((unsigned long long) hi << 32) | lo;
#endif
      out.avx = (xcr0 & 0x6) == 0x6;
    }
  }
  if (highestLeaf >= 7 && out.avx) {
    statsdetail::cpuid(7, 0, info);
    out.avx2 = (info[1] & (1 << 5)) != 0;
  }
#endif
  return out;
}

/** "GenuineIntel", "AuthenticAMD", or empty where the question does not
 *  apply. */
inline std::string cpuVendor() {
#if SONORE_HAVE_CPUID
  int info[4] = {0, 0, 0, 0};
  statsdetail::cpuid(0, 0, info);
  char text[13] = {0};
  std::memcpy(text + 0, &info[1], 4);
  std::memcpy(text + 4, &info[3], 4);
  std::memcpy(text + 8, &info[2], 4);
  return text;
#else
  return {};
#endif
}

/** The processor's own name string, or empty. */
inline std::string cpuModel() {
#if SONORE_HAVE_CPUID
  int info[4] = {0, 0, 0, 0};
  statsdetail::cpuid((int) 0x80000000, 0, info);
  if ((unsigned) info[0] < 0x80000004u) return {};
  char text[49] = {0};
  for (int i = 0; i < 3; ++i) {
    statsdetail::cpuid((int) (0x80000002u + (unsigned) i), 0, info);
    std::memcpy(text + i * 16, info, 16);
  }
  // The string is space-padded on most processors and not on others.
  std::string out = text;
  while (!out.empty() && out.back() == ' ') out.pop_back();
  size_t start = 0;
  while (start < out.size() && out[start] == ' ') ++start;
  return out.substr(start);
#else
  return {};
#endif
}

/** Logical processors -- what a thread pool would divide by. Zero where the
 *  platform will not say. */
inline int numLogicalCpus() {
#if defined(_WIN32)
  SYSTEM_INFO info{};
  GetNativeSystemInfo(&info);
  return (int) info.dwNumberOfProcessors;
#else
  const long n = sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? (int) n : 0;
#endif
}

/** Physical memory in megabytes, or 0. */
inline int memorySizeMb() {
#if defined(_WIN32)
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof(status);
  if (!GlobalMemoryStatusEx(&status)) return 0;
  return (int) (status.ullTotalPhys / (1024ull * 1024ull));
#elif defined(__APPLE__)
  uint64_t bytes = 0;
  size_t size = sizeof(bytes);
  if (sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) != 0) return 0;
  return (int) (bytes / (1024ull * 1024ull));
#else
  const long pages = sysconf(_SC_PHYS_PAGES);
  const long pageSize = sysconf(_SC_PAGE_SIZE);
  if (pages <= 0 || pageSize <= 0) return 0;
  return (int) (((uint64_t) pages * (uint64_t) pageSize) / (1024ull * 1024ull));
#endif
}

inline std::string osName() {
#if defined(_WIN32)
  return "Windows";
#elif defined(__APPLE__)
  return "macOS";
#elif defined(__linux__)
  return "Linux";
#else
  return "Unknown";
#endif
}

/**
 * The OS version, as a string, or empty.
 *
 * On Windows through RtlGetVersion rather than GetVersionEx. That one LIES by
 * design: since Windows 8.1 it reports 6.2 to any process without a matching
 * compatibility manifest, and a plugin is loaded into a host whose manifest is
 * not ours to control. So a plugin using it reports "Windows 8" on every
 * machine, forever, and the bug report is worthless.
 */
inline std::string osVersion() {
#if defined(_WIN32)
  typedef LONG(WINAPI * RtlGetVersionFn)(PRTL_OSVERSIONINFOW);
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (!ntdll) return {};
  RtlGetVersionFn get = (RtlGetVersionFn) GetProcAddress(ntdll, "RtlGetVersion");
  if (!get) return {};
  RTL_OSVERSIONINFOW info{};
  info.dwOSVersionInfoSize = sizeof(info);
  if (get(&info) != 0) return {};
  char text[64];
  // Windows 11 reports itself as 10.0 with a high build number, which is what
  // the build is for -- 22000 and up is 11. Reported as the number rather
  // than translated, because the translation changes and the number does not.
  std::snprintf(text, sizeof(text), "%lu.%lu build %lu", (unsigned long) info.dwMajorVersion,
                (unsigned long) info.dwMinorVersion, (unsigned long) info.dwBuildNumber);
  return text;
#else
  struct utsname info{};
  if (uname(&info) != 0) return {};
  return std::string(info.release);
#endif
}

/**
 * One line for a bug report or the top of a log.
 *
 * Everything that fits on a line and nothing that does not. A user pasting
 * this into an email is the entire point, so it has to survive being pasted
 * into an email.
 */
inline std::string machineDescription() {
  std::string out = osName();
  const std::string version = osVersion();
  if (!version.empty()) out += " " + version;

  const std::string model = cpuModel();
  out += " | ";
  out += model.empty() ? cpuVendor() : model;

  char text[96];
  std::snprintf(text, sizeof(text), " | %d cpu | %d MB", numLogicalCpus(), memorySizeMb());
  out += text;

  const CpuFeatures features = cpuFeatures();
  std::string flags;
  if (features.sse2) flags += " sse2";
  if (features.sse41) flags += " sse4.1";
  if (features.avx) flags += " avx";
  if (features.avx2) flags += " avx2";
  if (features.fma) flags += " fma";
  if (features.neon) flags += " neon";
  if (!flags.empty()) out += " |" + flags;
  return out;
}

} // namespace sonore
