// SPDX-License-Identifier: Apache-2.0
// The system layer's macOS and Linux halves -- BSD sockets, NSURLSession
// through the Objective-C runtime, libcurl through dlopen, posix_spawn,
// Unix-domain pipes, flock, mmap -- compiled on Windows with _WIN32
// undefined, against tests/au_shim (Apple's declarations) and
// tests/au_shim/msvc (the POSIX names MSVC lacks).
//
//   macOS: /U_WIN32 /D__APPLE__=1 /DSONORE_APPLE_SYNTAX_CHECK=1
//          /FI tests/au_shim/msvc/posix_compat.h, include dirs tests/au_shim,
//          tests/au_shim/win, tests/au_shim/msvc
//   Linux: /U_WIN32 /D__linux__=1, the same posix_compat.h, tests/au_shim/win
//          and tests/au_shim/msvc, plus a <linux/if_packet.h> stub
//
// What that proves is what au_shim/README.md says: every name exists and
// every call fits its declaration -- not that the declarations match the
// platform's ABI, and nothing here runs. The static_asserts below fail if the
// branch a platform was meant to take was not the one compiled.
#if defined(_WIN32)
#error "build with _WIN32 undefined: see the header"
#endif

#include <sonore/file_types.h>
#include <sonore/ipc.h>
#include <sonore/net.h>
#include <sonore/process.h>
#include <sonore/streams.h>

#if defined(__APPLE__)
static_assert(sizeof(sonore::netdetail::NsUrlBackend) > 0, "the NSURLSession backend was compiled");
#elif defined(__linux__)
static_assert(sizeof(sonore::netdetail::CurlBackend) > 0, "the libcurl backend was compiled");
#endif

// The templates only a use instantiates.
struct SyntaxDialogFilter {
  std::string label, patterns;
};

int sonoreSystemSyntaxCheck() {
  int n = 0;
  const sonore::WildcardFileFilter filter = sonore::WildcardFileFilter::fromDialogFilter(SyntaxDialogFilter());
  n += filter.isFileSuitable("a.wav") ? 1 : 0;
  sonore::ipcdetail::FrameDecoder decoder(1, 16);
  const uint8_t bytes[8] = {1, 0, 0, 0, 0, 0, 0, 0};
  decoder.feed(bytes, sizeof(bytes), [&n](const std::vector<uint8_t>& m) { n += (int) m.size(); });
  sonore::WebInputStream stream(sonore::URL("http://example.com/"));
  n += stream.getStatusCode();
  return n;
}
