// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: where a test puts the files it writes.
//
// Every artifact a test wrote used to be a BARE RELATIVE NAME, so it landed in
// whatever directory the suite happened to be run from. Three of them
// (sonore-stream-16.wav, sonore-stream-24.wav, sonore-stream-test.aiff) were
// found sitting in the working tree of the repository that vendors this SDK,
// committed by nobody and explained by nothing.
//
// Why only those three survived is the part worth keeping: each is removed by a
// `std::remove(path)` that runs while an AudioFileReader still has the file
// OPEN. POSIX unlink drops the directory entry and lets the inode live until
// the last close, so the cleanup works perfectly on Linux; Windows refuses to
// delete an open file, and std::remove's return value is ignored. The leak
// therefore existed only on the platform whose test output nobody diffed
// against a clean checkout, and the gcc leg, which is the one that runs under
// sanitizers, could never have shown it.
//
// So: one private directory per run under the system temp, swept when the
// process exits. That also stops two suites running side by side from fighting
// over the same file names, which is its own intermittent failure: this
// session hit exactly that when a verify run and a hand-run test shared an
// output directory.
#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace sonoretest {

/** The one directory this process writes its artifacts into. */
inline const std::string& tempDir() {
  static const std::string dir = [] {
    namespace fs = std::filesystem;
    const fs::path base = fs::temp_directory_path();
    // create_directory is atomic and answers FALSE for one that already exists,
    // so two suites started at the same moment land in different directories
    // without needing a process id, a lock, or a random name that could collide
    // anyway.
    for (int i = 0; i < 4096; ++i) {
      const fs::path candidate = base / ("sonore-tests-" + std::to_string(i));
      std::error_code ec;
      if (fs::create_directory(candidate, ec) && !ec) return candidate.string();
    }
    return base.string(); // 4096 stale directories: litter temp rather than fail
  }();
  return dir;
}

/** A path for a file this test writes.
 *
 *  Returns a pointer that stays valid for the life of the process, so a call
 *  site keeps whatever `const char*` API it already had: the alternative was
 *  a std::string and a .c_str() at forty use sites, which is forty chances to
 *  get it wrong for no gain. The strings are deliberately never freed: there
 *  are a few dozen, and a test binary that outlives them has bigger problems. */
inline const char* tempPath(const char* leaf) {
  // Constructed AFTER tempDir()'s string, because the call on this line runs
  // first, so `sweeper` is destroyed BEFORE it, and the sweep reads a path
  // that is still alive. (This is also why it is a static object rather than
  // std::atexit: a handler registered before that string was constructed would
  // run after its destructor.)
  static const std::string dir = tempDir();
  struct Sweeper {
    ~Sweeper() {
      std::error_code ec;
      // By now every reader in every test has gone out of scope, so the files
      // Windows refused to delete while they were open are deletable.
      std::filesystem::remove_all(tempDir(), ec);
    }
  };
  static Sweeper sweeper;
  (void) sweeper;

  static std::vector<std::unique_ptr<std::string>> kept;
  kept.push_back(std::make_unique<std::string>((std::filesystem::path(dir) / leaf).string()));
  return kept.back()->c_str();
}

} // namespace sonoretest
