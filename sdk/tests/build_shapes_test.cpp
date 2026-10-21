// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the headers, included the way a user's project includes them.
//
// Two shapes nothing else in the build had:
//
//   * <windows.h> FULL, before the SDK. rpcndr.h, which it pulls in, defines
//     `small` as a macro for `char`, and number_text.h had a buffer called
//     small -- so json.h, state_bag.h or anything that reached number_text.h
//     after file_dialog.h or webview_win32.h failed to compile.
//
//   * plugin_cache.h WITHOUT the VST3 SDK on the include path, the CLAP-only
//     host host.h promises compiles. stampOf called a VST3-only helper on
//     Windows, so it did not.
//
// The target is built without third_party/vst3 for exactly that reason. The
// test passes by compiling; running it checks one parse and one stamp.
#if defined(_WIN32)
#include <windows.h> // all of it, as an application includes it
#endif

#include <sonore/json.h>
#include <sonore/number_text.h>
#include <sonore/plugin_cache.h>
#include <sonore/state_bag.h>

#include <cstdio>
#include <string>

#if defined(SONORE_HOST_VST3)
#error "build_shapes_test must be built without the VST3 SDK on its include path"
#endif

int main() {
  int failures = 0;
  double value = 0.0;
  const bool parsed = sonore::numtext::parse("1.5", 3, &value) == 3 && value == 1.5;
  std::printf("  %-4s number_text parses after a full <windows.h>\n", parsed ? "ok" : "FAIL");
  failures += parsed ? 0 : 1;

  // A file under a name with an accent, stamped through the UTF-8 path the
  // scanner hands over. The ANSI call stamped it {0, 0}: never up to date, so
  // the plugin was loaded on every scan.
  const std::string path = "build_shapes_caf\xC3\xA9.bin";
  if (std::FILE* f = sonore::pathio::open(path.c_str(), "wb")) {
    std::fputs("stamp me", f);
    std::fclose(f);
  }
  const sonore::host::FileStamp stamp = sonore::host::stampOf(path);
  const bool stamped = stamp.valid() && stamp.size == 8;
  std::printf("  %-4s a file whose name has an accent gets a real stamp (size %llu)\n",
              stamped ? "ok" : "FAIL", (unsigned long long) stamp.size);
  failures += stamped ? 0 : 1;
  sonore::pathio::remove(path.c_str());

  std::printf("\n%d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}
