// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: MP3 out, through the user's own LAME.
//
// LAMEEncoderAudioFormat writes an MP3 the way the reference does: the audio
// goes to a temporary 16-bit WAV, the lame command-line encoder the USER
// installed is run on it, and the MP3 it writes is read back and handed over.
//
// ── Why a separate program and not a library ────────────────────────────────
//
// The MP3 patents have expired, but LAME is LGPL. Linked into a plugin it
// brings LGPL obligations into every binary built with this SDK; run as the
// user's own executable it brings none -- the user chose and installed it,
// and the plugin only ever names a path. That is the reference's reason too,
// and it is why the path is always the caller's: nothing here searches PATH
// or guesses where an encoder might live.
//
// ── Launching without a shell ───────────────────────────────────────────────
//
// system() and popen() hand a STRING to a shell, and every space, quote,
// ampersand or dollar sign in a path is then the shell's to interpret. A user
// called O'Brien, or a song title with "&" in it, is enough to run something
// that was never meant to run. So the process is started directly --
// CreateProcessW on Windows, posix_spawn elsewhere -- with the argument list
// as a list.
//
// On POSIX that is the whole story: argv arrives as given. Windows has no
// argv at the system level; CreateProcessW takes one command line and the
// CHILD splits it, by the C runtime's rules (the ones CommandLineToArgvW
// implements). Those rules are odd -- backslashes are literal except in runs
// that precede a quote, where they escape -- so quoteArgument() below is the
// exact inverse of them, and audio_io_test checks it by having a child echo
// back what it received for names with spaces, quotes, trailing backslashes
// and non-ASCII letters.
//
// ── What can go wrong at the edges ──────────────────────────────────────────
//
// An encoder that hangs (waiting on a terminal, a network drive that went
// away) would hang the caller with it, so every run has a timeout after which
// the process is killed and the call fails saying so. The temporary files are
// created EXCLUSIVELY under random names -- never an existing file reused,
// never a predictable name another process could plant first -- and removed
// on every path out, success or not. The child gets no inherited handles: a
// plugin's host has files and pipes open that are none of the encoder's
// business, and on Windows an inherited handle keeps those files locked for
// as long as the child lives.
//
// Offline: this blocks for as long as the encoder runs. Never from process().
#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "path_io.h"
#include "wav.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <crt_externs.h> // _NSGetEnviron: a bundle has no `environ` of its own
#else
extern char** environ;
#endif
#endif

namespace sonore {
namespace lamedetail {

/** What happened to a child process. */
struct ProcessResult {
  bool started = false;
  bool timedOut = false;
  int exitCode = -1; // -1 when it did not exit on its own
  std::string error;
};

#if defined(_WIN32)

/**
 * One argument, quoted so the child's C runtime splits it back into exactly
 * these characters.
 *
 * The rules being inverted: inside quotes, 2n backslashes before a quote are
 * n backslashes and the quote ENDS the argument; 2n+1 backslashes before a
 * quote are n backslashes and a literal quote; backslashes anywhere else are
 * literal. So a run of backslashes is doubled only when a quote follows it --
 * including the closing quote this function adds, which is why "C:\dir\"
 * needs its trailing backslash doubled and "C:\dir\file" does not.
 */
inline std::wstring quoteArgument(const std::wstring& arg) {
  if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
  std::wstring out = L"\"";
  for (size_t i = 0;; ++i) {
    size_t slashes = 0;
    while (i < arg.size() && arg[i] == L'\\') {
      ++slashes;
      ++i;
    }
    if (i == arg.size()) {
      out.append(slashes * 2, L'\\');
      break;
    }
    if (arg[i] == L'"') {
      out.append(slashes * 2 + 1, L'\\');
      out.push_back(L'"');
    } else {
      out.append(slashes, L'\\');
      out.push_back(arg[i]);
    }
  }
  out.push_back(L'"');
  return out;
}

/** The program name is parsed by different rules -- quotes delimit, nothing
 *  escapes -- and a Windows path cannot contain a quote, so it is always
 *  quoted and never escaped. */
inline std::wstring buildCommandLine(const std::wstring& exe, const std::vector<std::string>& args) {
  std::wstring line = L"\"" + exe + L"\"";
  for (const std::string& a : args) {
    line.push_back(L' ');
    line += quoteArgument(pathio::widen(a.c_str()));
  }
  return line;
}

#endif

/**
 * Run `exe` with `args` and wait for it, at most `timeoutSeconds` (zero or
 * less waits forever). No shell, no PATH search, no inherited handles, no
 * console window. The child's output goes nowhere: an encoder's progress
 * meter is not something a plugin has a place to show.
 */
inline ProcessResult runProcess(const std::string& exe, const std::vector<std::string>& args,
                                double timeoutSeconds) {
  ProcessResult result;
  if (exe.empty()) {
    result.error = "no executable was given";
    return result;
  }
#if defined(_WIN32)
  const std::wstring program = pathio::widen(exe.c_str());
  std::wstring line = buildCommandLine(program, args);
  STARTUPINFOW startup;
  std::memset(&startup, 0, sizeof(startup));
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION info;
  std::memset(&info, 0, sizeof(info));
  // lpApplicationName given, so the program is exactly that file: no search
  // of the current directory, the application's directory or PATH. The
  // command line must be writable -- CreateProcessW may modify it in place.
  if (!CreateProcessW(program.c_str(), &line[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                      nullptr, nullptr, &startup, &info)) {
    result.error = "the encoder could not be started (Windows error " +
                   std::to_string((unsigned long) GetLastError()) + ")";
    return result;
  }
  result.started = true;
  CloseHandle(info.hThread);
  DWORD wait = INFINITE;
  if (timeoutSeconds > 0.0) {
    const double ms = timeoutSeconds * 1000.0;
    wait = ms >= 4.0e9 ? (DWORD) 4000000000u : (DWORD) (ms + 0.5);
  }
  if (WaitForSingleObject(info.hProcess, wait) == WAIT_TIMEOUT) {
    TerminateProcess(info.hProcess, 1);
    // Reaped before returning, so the temporary files it held open can be
    // deleted: Windows refuses to remove a file another process has open.
    WaitForSingleObject(info.hProcess, 10000);
    result.timedOut = true;
    result.error = "the encoder did not finish in time and was stopped";
  } else {
    DWORD code = 0;
    GetExitCodeProcess(info.hProcess, &code);
    result.exitCode = (int) code;
  }
  CloseHandle(info.hProcess);
  return result;
#else
  std::vector<char*> argv;
  argv.push_back(const_cast<char*>(exe.c_str()));
  for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
#if defined(__APPLE__)
  char** env = *_NSGetEnviron();
#else
  char** env = environ;
#endif
  pid_t pid = 0;
  // posix_spawn, not posix_spawnp: the path is used as given, never searched.
  // Descriptors the host opened without FD_CLOEXEC are still inherited here;
  // stdio, which is what an encoder would write to, is not.
  const int rc = posix_spawn(&pid, exe.c_str(), &actions, nullptr, argv.data(), env);
  posix_spawn_file_actions_destroy(&actions);
  if (rc != 0) {
    result.error = "the encoder could not be started (errno " + std::to_string(rc) + ")";
    return result;
  }
  result.started = true;
  // A timeout past thirty years is no timeout, and is not converted: the
  // multiplication would overflow the duration.
  const bool bounded = timeoutSeconds > 0.0 && timeoutSeconds < 1.0e9;
  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::milliseconds(bounded ? (long long) (timeoutSeconds * 1000.0) : 0LL);
  int status = 0;
  for (;;) {
    const pid_t done = waitpid(pid, &status, WNOHANG);
    if (done == pid) break;
    if (done < 0) {
      result.error = "the encoder could not be waited for";
      return result;
    }
    if (bounded && std::chrono::steady_clock::now() >= deadline) {
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0); // reaped: no zombie left in the host
      result.timedOut = true;
      result.error = "the encoder did not finish in time and was stopped";
      return result;
    }
    struct timespec pause = {0, 5 * 1000 * 1000};
    nanosleep(&pause, nullptr);
  }
  if (WIFEXITED(status)) result.exitCode = WEXITSTATUS(status);
  else result.error = "the encoder was killed by a signal";
  return result;
#endif
}

/** The system's directory for temporary files, as UTF-8. */
inline std::string tempDirectory() {
#if defined(_WIN32)
  wchar_t buffer[MAX_PATH + 2] = {0};
  const DWORD n = GetTempPathW(MAX_PATH + 1, buffer);
  if (n == 0 || n > MAX_PATH + 1) return ".";
  return pathio::narrow(buffer);
#else
  const std::string fromEnv = pathio::environment("TMPDIR");
  return fromEnv.empty() ? std::string("/tmp") : fromEnv;
#endif
}

/** Create a file that did not exist before, or fail. The exclusivity is the
 *  point: a name that already exists -- left by a crash, or planted -- is
 *  never written through. */
inline bool createExclusive(const std::string& path) {
#if defined(_WIN32)
  HANDLE h = CreateFileW(pathio::widenPath(path.c_str()).c_str(), GENERIC_WRITE, 0, nullptr,
                         CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  CloseHandle(h);
  return true;
#else
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0) return false;
  ::close(fd);
  return true;
#endif
}

/** A temporary file that is removed when this goes out of scope, on every
 *  path -- including the ones where the encoder failed or timed out. */
class TempFile {
public:
  TempFile() = default;
  ~TempFile() { release(); }
  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;

  /** Reserve a fresh name in `directory` ending in `suffix`. */
  bool create(const std::string& directory, const char* suffix) {
    release();
    // Random enough that a name cannot be predicted from the outside, and the
    // exclusive create makes any collision a retry rather than a reuse.
    std::random_device device;
    std::mt19937_64 rng(((uint64_t) device() << 32) ^ (uint64_t) device() ^
                        (uint64_t) std::chrono::steady_clock::now().time_since_epoch().count());
    for (int attempt = 0; attempt < 64; ++attempt) {
      char name[64];
      std::snprintf(name, sizeof(name), "sonore-lame-%016llx%s", (unsigned long long) rng(),
                    suffix);
      std::string candidate = directory;
      if (!candidate.empty() && candidate.back() != '/' && candidate.back() != '\\')
        candidate.push_back('/');
      candidate += name;
      if (createExclusive(candidate)) {
        path_ = candidate;
        return true;
      }
    }
    return false;
  }

  void release() {
    if (!path_.empty()) pathio::remove(path_.c_str());
    path_.clear();
  }

  const std::string& path() const { return path_; }

private:
  std::string path_;
};

/** 16-bit PCM, because it is the one WAV every LAME build reads: float input
 *  arrived in different releases under different flags. TPDF dither is not
 *  added -- the encoder's own quantisation is coarser by orders of
 *  magnitude. */
inline bool writePcm16Wav(const std::string& path, const float* interleaved, size_t frames,
                          uint16_t channels, uint32_t rate) {
  const uint64_t dataBytes = (uint64_t) frames * channels * 2u;
  if (dataBytes > 0xFFFFFFFFull - 44ull) return false;
  std::vector<uint8_t> out;
  out.reserve((size_t) dataBytes + 44);
  auto u32 = [&](uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back((uint8_t) ((v >> (8 * i)) & 0xff));
  };
  auto u16 = [&](uint16_t v) {
    out.push_back((uint8_t) (v & 0xff));
    out.push_back((uint8_t) (v >> 8));
  };
  out.insert(out.end(), {'R', 'I', 'F', 'F'});
  u32((uint32_t) (36 + dataBytes));
  out.insert(out.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  u32(16);
  u16(1);
  u16(channels);
  u32(rate);
  u32(rate * channels * 2u);
  u16((uint16_t) (channels * 2u));
  u16(16);
  out.insert(out.end(), {'d', 'a', 't', 'a'});
  u32((uint32_t) dataBytes);
  const size_t total = frames * channels;
  for (size_t i = 0; i < total; ++i) {
    float v = interleaved[i];
    if (!(v == v)) v = 0.0f; // NaN would otherwise become whatever lround makes of it
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    const long q = std::lround((double) v * 32767.0);
    u16((uint16_t) (int16_t) q);
  }
  std::FILE* f = pathio::open(path.c_str(), "wb");
  if (!f) return false;
  const bool wrote = std::fwrite(out.data(), 1, out.size(), f) == out.size();
  const bool closed = std::fclose(f) == 0;
  return wrote && closed;
}

inline bool readWholeFile(const std::string& path, std::vector<uint8_t>* out) {
  std::FILE* f = pathio::open(path.c_str(), "rb");
  if (!f) return false;
  out->clear();
  uint8_t chunk[65536];
  for (;;) {
    const size_t n = std::fread(chunk, 1, sizeof(chunk), f);
    out->insert(out->end(), chunk, chunk + n);
    if (n < sizeof(chunk)) break;
  }
  const bool ok = std::ferror(f) == 0;
  std::fclose(f);
  return ok;
}

} // namespace lamedetail

/**
 * MP3 through an external LAME executable. Mono or stereo, any rate LAME
 * takes (it resamples what MP3 cannot carry).
 */
class LAMEEncoderAudioFormat {
public:
  /** What LAME is asked for: a VBR quality, or a bitrate for CBR and ABR,
   *  plus the encoder's effort, how long it may take and the ID3 tags. Only
   *  the field the mode uses matters, and out-of-range numbers are clamped
   *  by arguments() rather than handed to LAME to refuse. */
  struct Options {
    /** How the bitrate is chosen. */
    enum class Mode {
      vbr, ///< Variable bitrate at vbrQuality: the size follows the material.
      cbr, ///< Constant bitrate at bitrateKbps.
      abr  ///< Average bitrate: varies, but averages bitrateKbps.
    };
    Mode mode = Mode::vbr; ///< VBR unless told otherwise.
    /** -V 0..9 for VBR, 0 the best. LAME's own recommendation is 2. */
    int vbrQuality = 2;
    /** kbps, for CBR and ABR: 8..320. */
    int bitrateKbps = 192;
    /** -q 0..9, the algorithm's effort; -1 leaves LAME's default. */
    int algorithmQuality = -1;
    /** Seconds the encoder may run before it is stopped. */
    double timeoutSeconds = 300.0;
    /** ID3 tags, written by LAME. Empty ones are left out. */
    std::string title, ///< The track's title (`--tt`).
        artist,        ///< The artist (`--ta`).
        album,         ///< The album (`--tl`).
        year,          ///< The year, as text (`--ty`).
        comment,       ///< A comment (`--tc`).
        track,         ///< The track number, "3" or "3/12" (`--tn`).
        genre;         ///< A genre name or number, as LAME takes it (`--tg`).
  };

  /** An encoder that runs the LAME executable at `lameExecutable`, a UTF-8
   *  path. Nothing is checked until canEncode() or an encode. */
  explicit LAMEEncoderAudioFormat(std::string lameExecutable)
      : exe_(std::move(lameExecutable)) {}

  /** The path given to the constructor. */
  const std::string& executable() const { return exe_; }

  /** The executable exists as a file. Not a promise that it is LAME. */
  bool canEncode() const {
    if (exe_.empty()) return false;
    std::FILE* f = pathio::open(exe_.c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
  }

  /** The bitrates MPEG-1 Layer III defines, for a settings menu. */
  static std::vector<int> cbrBitrates() {
    return {32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
  }

  /** The arguments LAME will be run with, for these files. Public so a
   *  caller (and the test) can see exactly what is asked for. Out-of-range
   *  settings are clamped rather than passed through for LAME to refuse. */
  std::vector<std::string> arguments(const Options& options, const std::string& wavIn,
                                     const std::string& mp3Out) const {
    std::vector<std::string> args = {"--quiet"};
    auto clamp = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
    switch (options.mode) {
      case Options::Mode::vbr:
        args.push_back("-V");
        args.push_back(std::to_string(clamp(options.vbrQuality, 0, 9)));
        break;
      case Options::Mode::cbr:
        args.push_back("--cbr");
        args.push_back("-b");
        args.push_back(std::to_string(clamp(options.bitrateKbps, 8, 320)));
        break;
      case Options::Mode::abr:
        args.push_back("--abr");
        args.push_back(std::to_string(clamp(options.bitrateKbps, 8, 320)));
        break;
    }
    if (options.algorithmQuality >= 0) {
      args.push_back("-q");
      args.push_back(std::to_string(clamp(options.algorithmQuality, 0, 9)));
    }
    const std::pair<const char*, const std::string*> tags[] = {
        {"--tt", &options.title},   {"--ta", &options.artist}, {"--tl", &options.album},
        {"--ty", &options.year},    {"--tc", &options.comment}, {"--tn", &options.track},
        {"--tg", &options.genre}};
    for (const auto& tag : tags) {
      if (tag.second->empty()) continue;
      args.push_back(tag.first);
      args.push_back(*tag.second);
    }
    // Both file names after the options, so a name beginning with '-' could
    // only ever be read as a file. The temporary names here never do anyway.
    args.push_back(wavIn);
    args.push_back(mp3Out);
    return args;
  }

  /** Encode to MP3 bytes in memory. */
  bool encode(const float* interleaved, size_t numFrames, uint16_t numChannels,
              uint32_t sampleRate, const Options& options, std::vector<uint8_t>* mp3) {
    error_.clear();
    if (!interleaved || !mp3 || numFrames == 0) return failWith("there is no audio to encode");
    if (numChannels < 1 || numChannels > 2) return failWith("MP3 carries mono or stereo only");
    if (sampleRate < 8000 || sampleRate > 192000)
      return failWith("the sample rate is outside what LAME accepts");
    if (!canEncode()) return failWith("the LAME executable was not found at the given path");

    const std::string dir = lamedetail::tempDirectory();
    lamedetail::TempFile wav, out;
    if (!wav.create(dir, ".wav") || !out.create(dir, ".mp3"))
      return failWith("temporary files could not be created");
    if (!lamedetail::writePcm16Wav(wav.path(), interleaved, numFrames, numChannels, sampleRate))
      return failWith("the temporary WAV could not be written");

    const lamedetail::ProcessResult run =
        lamedetail::runProcess(exe_, arguments(options, wav.path(), out.path()),
                               options.timeoutSeconds);
    if (!run.started || run.timedOut || !run.error.empty()) return failWith(run.error.c_str());
    if (run.exitCode != 0)
      return failWith(("the encoder failed with exit code " + std::to_string(run.exitCode)).c_str());
    if (!lamedetail::readWholeFile(out.path(), mp3) || mp3->empty())
      return failWith("the encoder finished but wrote nothing");
    return true;
  }

  // Overloads, not `= Options()`: a default argument of a nested type is
  // read while the class is still incomplete, and gcc and clang refuse
  // Options' member initializers there. MSVC accepts it, which is how
  // this went unnoticed.
  /** Encode to an MP3 file at a UTF-8 path. */
  bool write(const char* mp3Path, const float* interleaved, size_t numFrames,
             uint16_t numChannels, uint32_t sampleRate) {
    return write(mp3Path, interleaved, numFrames, numChannels, sampleRate, Options());
  }
  /** Encode with `options` to an MP3 file at a UTF-8 path. False, with
   *  lastError() saying why, if the encode fails or the file cannot be written;
   *  a half-written file is removed. Blocks for as long as LAME runs. */
  bool write(const char* mp3Path, const float* interleaved, size_t numFrames,
             uint16_t numChannels, uint32_t sampleRate, const Options& options) {
    if (!mp3Path) return failWith("no destination path");
    std::vector<uint8_t> bytes;
    if (!encode(interleaved, numFrames, numChannels, sampleRate, options, &bytes)) return false;
    std::FILE* f = pathio::open(mp3Path, "wb");
    if (!f) return failWith("the destination could not be opened for writing");
    const bool wrote = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    const bool closed = std::fclose(f) == 0;
    if (!wrote || !closed) {
      pathio::remove(mp3Path); // a half-written MP3 is worse than none
      return failWith("the destination could not be written");
    }
    return true;
  }

  /** Why the last encode or write failed; empty after a success. */
  const std::string& lastError() const { return error_; }

private:
  bool failWith(const char* why) {
    error_ = why ? why : "";
    return false;
  }

  std::string exe_;
  std::string error_;
};

} // namespace sonore
