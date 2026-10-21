// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: other processes: launching one, a lock shared between
// processes, a library loaded at run time, and objects deleted at shutdown.
//
//   ChildProcess        run a program with arguments -- never through a
//                       shell -- read its output, wait, kill
//   InterProcessLock    a named lock every process on the machine (well, in
//                       this user's session) sees
//   DynamicLibrary      LoadLibrary / dlopen as an object
//   DeletedAtShutdown   objects a program deletes together at the end
//   currentExecutablePath(), currentModulePath()
//
// ── A plugin that spawns processes ──────────────────────────────────────────
//
// is something users find in a firewall log or a task manager and do not
// forgive, and the reason the feature map called this out of scope for so
// long. It is here because there are honest reasons for it -- an out-of-
// process scanner, a helper that does work a host would crash on, the
// standalone running a converter -- and because doing it WRONG is worse than
// not doing it: a child that inherits every handle the DAW has open, a
// command line pasted into a shell, a process left running after the plugin
// was removed from the track. So:
//
// NO SHELL. Arguments go to CreateProcessW / posix_spawn as a list. A file
// name with a space, a quote or a semicolon in it is one argument, not a
// command. On Windows the list is quoted by the rules the C runtime's own
// argv parser undoes (CommandLineToArgvW agrees; the tests check the round
// trip), because Windows has no argv at the system level. A .bat or .cmd is
// refused on Windows: the system runs it through cmd.exe, which does not
// split a command line by those rules.
//
// NOTHING INHERITED but stdin/stdout/stderr: on Windows the inheritable
// handles are restricted to exactly those three with
// PROC_THREAD_ATTRIBUTE_HANDLE_LIST (without it, every inheritable handle in
// the host -- some other plugin's pipe, a file it has open -- leaks into
// the child and keeps it alive); on POSIX every descriptor is close-on-exec
// and macOS is additionally told POSIX_SPAWN_CLOEXEC_DEFAULT. stdin is the
// null device, never the host's. The child's signal mask and dispositions
// are reset: a host that blocks SIGPIPE must not hand that to a converter.
//
// THE CHILD DIES WITH THE OBJECT. The destructor kills a child that is still
// running. A plugin removed from a track must not leave a process behind.
//
// OUTPUT NEVER STALLS THE CHILD. stdout and stderr are drained by a thread
// each from the moment the child starts, so a child that writes more than a
// pipe holds (64 KB on Linux, 4 KB on some systems) never blocks waiting for
// a reader that is busy in waitForProcessToFinish -- the classic deadlock of
// reading a child's output after waiting for it.
//
// ── Threads ─────────────────────────────────────────────────────────────────
//
// Everything here blocks or does I/O. Never on the audio thread.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "file_types.h"
#include "path_io.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <crt_externs.h>
#include <mach-o/dyld.h>
#else
extern char** environ;
#endif
#endif

namespace sonore {

namespace processdetail {

/**
 * One argument as the Windows C runtime's argv parser (and CommandLineToArgvW)
 * will read it back: bare when it needs nothing, otherwise in quotes, with a
 * run of backslashes doubled where it ends at a quote or at the closing
 * quote, and each quote escaped. Backslashes anywhere else are literal --
 * which is why "C:\\Program Files\\" must become "C:\\Program Files\\\\"
 * in quotes, and a naive "quote it if it has a space" gets that wrong.
 */
inline std::string quoteWindowsArgument(const std::string& arg) {
  if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) return arg;
  std::string out = "\"";
  for (size_t i = 0;; ++i) {
    size_t backslashes = 0;
    while (i < arg.size() && arg[i] == '\\') {
      ++i;
      ++backslashes;
    }
    if (i == arg.size()) {
      out.append(backslashes * 2, '\\');
      break;
    }
    if (arg[i] == '"') {
      out.append(backslashes * 2 + 1, '\\');
      out.push_back('"');
    } else {
      out.append(backslashes, '\\');
      out.push_back(arg[i]);
    }
  }
  out.push_back('"');
  return out;
}

/** Whether Windows would run this program through the command interpreter: a
 *  name ending ".bat" or ".cmd", case ignored, and with the trailing dots and
 *  spaces Windows strips from a file name ignored too ("x.BAT. " is x.bat). */
inline bool isWindowsBatchFile(const std::string& program) {
  size_t end = program.size();
  while (end > 0 && (program[end - 1] == '.' || program[end - 1] == ' ')) --end;
  if (end < 4) return false;
  std::string ext = program.substr(end - 4, 4);
  for (char& c : ext) c = (char) ((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
  return ext == ".bat" || ext == ".cmd";
}

inline std::string windowsCommandLine(const std::vector<std::string>& arguments) {
  std::string line;
  for (size_t i = 0; i < arguments.size(); ++i) {
    if (i) line.push_back(' ');
    line += quoteWindowsArgument(arguments[i]);
  }
  return line;
}

/** A command string split into arguments WITHOUT a shell: whitespace
 *  separates, single or double quotes group (and are removed), and inside
 *  double quotes a backslash escapes a quote or a backslash. Nothing is
 *  expanded -- no variables, no globs, no pipes. */
inline std::vector<std::string> splitCommandLine(const std::string& command) {
  std::vector<std::string> out;
  std::string current;
  bool inToken = false;
  char quote = 0;
  for (size_t i = 0; i < command.size(); ++i) {
    const char c = command[i];
    if (quote) {
      if (c == quote) {
        quote = 0;
      } else if (quote == '"' && c == '\\' && i + 1 < command.size() &&
                 (command[i + 1] == '"' || command[i + 1] == '\\')) {
        current.push_back(command[++i]);
      } else {
        current.push_back(c);
      }
    } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      if (inToken) out.push_back(current);
      current.clear();
      inToken = false;
    } else if (c == '"' || c == '\'') {
      quote = c;
      inToken = true;
    } else {
      current.push_back(c);
      inToken = true;
    }
  }
  if (inToken) out.push_back(current);
  return out;
}

/** A name safe as a file or kernel-object name, unique per original: the
 *  legal characters kept, the rest '_', and a hash of the whole so "a/b"
 *  and "a_b" stay different locks. */
inline std::string legalObjectName(const std::string& name, size_t maxReadableChars = 64) {
  uint64_t h = 14695981039346656037ULL;
  std::string out;
  for (char c : name) {
    h = (h ^ (uint8_t) c) * 1099511628211ULL;
    const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                      c == '_' || c == '.';
    if (out.size() < maxReadableChars) out.push_back(keep ? c : '_');
  }
  char suffix[24];
  std::snprintf(suffix, sizeof(suffix), "-%016llx", (unsigned long long) h);
  return out + suffix;
}

#if !defined(_WIN32)
/** Where a POSIX rendezvous -- a lock file, a pipe's socket -- is made: in
 *  $XDG_RUNTIME_DIR, the user's own directory (0700, on a tmpfs) where there
 *  is one, else in the temporary directory; and with the user id in the name
 *  either way. A bare /tmp/sonore-<name> is shared by every user of the
 *  machine, so one could squat another's name before it was made, or make it
 *  a link to somewhere else. `budget` is the room the caller has for the
 *  whole path (a socket path has about a hundred bytes), and a runtime
 *  directory that would leave too little is passed over. */
inline std::string userRendezvousPrefix(size_t budget) {
  char uid[24];
  std::snprintf(uid, sizeof(uid), "%lu", (unsigned long) ::getuid());
  const char* runtime = std::getenv("XDG_RUNTIME_DIR");
  if (runtime && runtime[0] == '/' && std::strlen(runtime) + 64 < budget)
    return std::string(runtime) + "/sonore-";
  return "/tmp/sonore-" + std::string(uid) + "-";
}
#endif

} // namespace processdetail

/** The running program's own executable -- for a plugin, the HOST's. */
inline std::string currentExecutablePath() {
#if defined(_WIN32)
  std::wstring buffer(MAX_PATH, L'\0');
  for (int attempt = 0; attempt < 6; ++attempt) {
    const DWORD n = GetModuleFileNameW(nullptr, &buffer[0], (DWORD) buffer.size());
    if (n == 0) return std::string();
    if (n < buffer.size()) {
      buffer.resize(n);
      return pathio::narrow(buffer.c_str());
    }
    buffer.resize(buffer.size() * 2);
  }
  return std::string();
#elif defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string path(size + 1, '\0');
  if (_NSGetExecutablePath(&path[0], &size) != 0) return std::string();
  path.resize(std::strlen(path.c_str()));
  char resolved[4096];
  return realpath(path.c_str(), resolved) ? std::string(resolved) : path;
#else
  char buffer[4096];
  const ssize_t n = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  return n > 0 ? std::string(buffer, (size_t) n) : std::string();
#endif
}

/** The binary this code is in: the plugin's .dll / .so / bundle executable
 *  when called from a plugin, the program otherwise. Where a plugin finds a
 *  helper it ships beside itself. */
inline std::string currentModulePath() {
#if defined(_WIN32)
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(reinterpret_cast<const void*>(&currentModulePath)), &module))
    return std::string();
  std::wstring buffer(MAX_PATH, L'\0');
  for (int attempt = 0; attempt < 6; ++attempt) {
    const DWORD n = GetModuleFileNameW(module, &buffer[0], (DWORD) buffer.size());
    if (n == 0) return std::string();
    if (n < buffer.size()) {
      buffer.resize(n);
      return pathio::narrow(buffer.c_str());
    }
    buffer.resize(buffer.size() * 2);
  }
  return std::string();
#else
  Dl_info info;
  std::memset(&info, 0, sizeof(info));
  if (!dladdr(reinterpret_cast<const void*>(&currentModulePath), &info) || !info.dli_fname) return std::string();
  return info.dli_fname;
#endif
}

/**
 * A program started with arguments, its output captured.
 *
 *   ChildProcess p;
 *   if (p.start({"/usr/bin/flac", "-d", path})) {
 *     p.waitForProcessToFinish(60000);
 *     if (p.getExitCode() != 0) report(p.getStandardError());
 *   }
 *
 * The first argument is the program: a path, or a bare name looked up the
 * way the platform looks one up (PATH; on Windows also the system folders,
 * with ".exe" implied). See the header for what the child does and does not
 * inherit, and why the destructor kills it.
 */
class ChildProcess {
public:
  /** Which of the child's output streams start() captures; OR them. */
  enum StreamFlags {
    wantStdOut = 1, ///< capture standard output
    wantStdErr = 2  ///< capture standard error
  };

  /** Not started: start() runs the program. */
  ChildProcess() = default;
  /** Kills the child if it is still running (see the header), then stops
   *  reading its output. */
  ~ChildProcess() {
    if (started_ && isRunning()) kill();
    stopReaders();
#if defined(_WIN32)
    if (process_) CloseHandle(process_);
#endif
  }
  ///@{
  /** Not copyable: the process and its output readers have one owner. */
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ///@}

  /**
   * Starts arguments[0] with the rest as its arguments. streamFlags says
   * which of stdout and stderr to capture; what is not captured goes to the
   * null device. False when the program could not be started (and a
   * ChildProcess can be started only once).
   */
  bool start(const std::vector<std::string>& arguments, int streamFlags = wantStdOut | wantStdErr) {
    if (started_ || arguments.empty() || arguments[0].empty()) return false;
    return launch(arguments, streamFlags);
  }
  /** The same, from one string split WITHOUT a shell (processdetail::splitCommandLine:
   *  quotes group, nothing is expanded). */
  bool start(const std::string& command, int streamFlags = wantStdOut | wantStdErr) {
    return start(processdetail::splitCommandLine(command), streamFlags);
  }

  /** True while the child has not exited; false before start() and after
   *  it ends. Does not wait. */
  bool isRunning() {
    std::lock_guard<std::mutex> lock(processMutex_);
    return pollRunning();
  }

  /** Waits for the child to exit: true if it did within timeoutMs (negative:
   *  wait for ever). */
  bool waitForProcessToFinish(int timeoutMs) {
    if (!started_) return false;
#if defined(_WIN32)
    const DWORD r = WaitForSingleObject(process_, timeoutMs < 0 ? INFINITE : (DWORD) timeoutMs);
    std::lock_guard<std::mutex> lock(processMutex_);
    return r == WAIT_OBJECT_0 && !pollRunning();
#else
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs < 0 ? 0 : timeoutMs);
    int sleepMs = 1;
    for (;;) {
      if (!isRunning()) return true;
      if (timeoutMs >= 0 && std::chrono::steady_clock::now() >= deadline) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
      if (sleepMs < 10) ++sleepMs;
    }
#endif
  }

  /** The exit code once it has exited: what it returned, or 128 + the signal
   *  that ended it on POSIX. 137 after kill(), on every platform. -1 while it
   *  runs, or if it never started. */
  int getExitCode() {
    std::lock_guard<std::mutex> lock(processMutex_);
    pollRunning();
    return exitCode_;
  }

  /** Ends the child at once (TerminateProcess / SIGKILL) and reaps it. */
  bool kill() {
    std::lock_guard<std::mutex> lock(processMutex_);
    if (!started_ || !pollRunning()) return false;
#if defined(_WIN32)
    if (!TerminateProcess(process_, 137)) return false;
    WaitForSingleObject(process_, 5000);
#else
    if (::kill(pid_, SIGKILL) != 0) return false;
    int status = 0;
    if (waitpid(pid_, &status, 0) == pid_) recordStatus(status);
#endif
    pollRunning();
    return true;
  }

  /** The system's id for the child, or 0. */
  int64_t getProcessId() const noexcept { return (int64_t) pid_; }

  /**
   * Up to numBytes of captured output -- stdout and stderr together, in the
   * order the drain threads received them. Blocks until there is some, or
   * until the child has closed both streams (then 0).
   */
  int readProcessOutput(void* dest, int numBytes) {
    if (!dest || numBytes <= 0) return 0;
    std::unique_lock<std::mutex> lock(outputMutex_);
    outputChanged_.wait(lock, [this] { return mergedRead_ < merged_.size() || openStreams_ == 0; });
    const size_t n = (std::min)((size_t) numBytes, merged_.size() - mergedRead_);
    std::memcpy(dest, merged_.data() + mergedRead_, n);
    mergedRead_ += n;
    return (int) n;
  }

  /** Everything not yet read by readProcessOutput, waiting until the child
   *  has closed its output (normally: until it exits). */
  std::string readAllProcessOutput() {
    std::unique_lock<std::mutex> lock(outputMutex_);
    outputChanged_.wait(lock, [this] { return openStreams_ == 0; });
    std::string out = merged_.substr(mergedRead_);
    mergedRead_ = merged_.size();
    return out;
  }

  /** Everything captured from stdout so far (unaffected by readProcessOutput). */
  std::string getStandardOutput() const {
    std::lock_guard<std::mutex> lock(outputMutex_);
    return out_;
  }
  /** Everything captured from stderr so far. */
  std::string getStandardError() const {
    std::lock_guard<std::mutex> lock(outputMutex_);
    return err_;
  }

  /** Output past this many bytes per stream is drained and dropped, so a
   *  child that prints for ever cannot fill the host's memory. */
  static constexpr size_t kMaxCapturedBytes = 64u << 20;

private:
#if defined(_WIN32)
  bool launch(const std::vector<std::string>& args, int flags) {
    // A batch file is not started: CreateProcessW hands it to cmd.exe, which
    // splits the command line by ITS rules, not the C runtime's that
    // quoteWindowsArgument inverts -- an argument would no longer be sure to
    // stay one argument, the promise at the top of this header. A caller who
    // wants the interpreter names cmd.exe itself, and owns its quoting.
    if (processdetail::isWindowsBatchFile(args[0])) return false;
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = nullptr;
    sa.bInheritHandle = TRUE;
    HANDLE outRead = nullptr, outWrite = nullptr, errRead = nullptr, errWrite = nullptr;
    auto closeAll = [&] {
      for (HANDLE h : {outRead, outWrite, errRead, errWrite})
        if (h) CloseHandle(h);
    };
    if ((flags & wantStdOut) && !makePipe(sa, outRead, outWrite)) return false;
    if ((flags & wantStdErr) && !makePipe(sa, errRead, errWrite)) {
      closeAll();
      return false;
    }
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, 0, nullptr);
    if (nul == INVALID_HANDLE_VALUE) {
      closeAll();
      return false;
    }
    STARTUPINFOEXW si;
    std::memset(&si, 0, sizeof(si));
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul;
    si.StartupInfo.hStdOutput = outWrite ? outWrite : nul;
    si.StartupInfo.hStdError = errWrite ? errWrite : nul;
    // Exactly these handles are inherited, each once (the list refuses
    // duplicates).
    HANDLE inherit[3];
    DWORD count = 0;
    for (HANDLE h : {nul, outWrite, errWrite})
      if (h) inherit[count++] = h;
    SIZE_T attributeSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    std::vector<uint64_t> attributeBuffer(attributeSize / sizeof(uint64_t) + 1);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeBuffer.data());
    bool haveList = InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize) &&
                    UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                              count * sizeof(HANDLE), nullptr, nullptr);
    si.lpAttributeList = haveList ? attributes : nullptr;
    std::wstring line = pathio::widen(processdetail::windowsCommandLine(args).c_str());
    line.push_back(L'\0'); // CreateProcessW may write into the buffer
    PROCESS_INFORMATION pi;
    std::memset(&pi, 0, sizeof(pi));
    const BOOL ok = haveList && CreateProcessW(nullptr, &line[0], nullptr, nullptr, TRUE,
                                               CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT |
                                                   CREATE_UNICODE_ENVIRONMENT,
                                               nullptr, nullptr, &si.StartupInfo, &pi);
    if (haveList) DeleteProcThreadAttributeList(attributes);
    CloseHandle(nul);
    if (outWrite) CloseHandle(outWrite); // the child has its own now
    if (errWrite) CloseHandle(errWrite);
    outWrite = errWrite = nullptr;
    if (!ok) {
      closeAll();
      return false;
    }
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    pid_ = pi.dwProcessId;
    started_ = true;
    startReaders(outRead, errRead);
    return true;
  }

  static bool makePipe(SECURITY_ATTRIBUTES& sa, HANDLE& readEnd, HANDLE& writeEnd) {
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) return false;
    // Our end is ours alone.
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    return true;
  }

  void startReaders(HANDLE outRead, HANDLE errRead) {
    openStreams_ = (outRead ? 1 : 0) + (errRead ? 1 : 0);
    if (outRead) outThread_ = std::thread([this, outRead] { drain(outRead, false); });
    if (errRead) errThread_ = std::thread([this, errRead] { drain(errRead, true); });
  }

  void drain(HANDLE h, bool isErr) {
    char buffer[4096];
    for (;;) {
      DWORD got = 0;
      if (!ReadFile(h, buffer, sizeof(buffer), &got, nullptr) || got == 0) break;
      append(buffer, got, isErr);
    }
    CloseHandle(h);
    (isErr ? errDone_ : outDone_) = true;
    streamClosed();
  }

  /** The readers end when the child (and anything it started that kept the
   *  pipes) closes them. If something keeps them open two seconds past the
   *  child's death -- a grandchild that inherited them -- their blocking reads
   *  are cancelled, again until they notice, since a cancel that lands before
   *  the read starts cancels nothing. */
  void stopReaders() {
    const auto giveUp = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    for (int which = 0; which < 2; ++which) {
      std::thread& t = which ? errThread_ : outThread_;
      const std::atomic<bool>& done = which ? errDone_ : outDone_;
      if (!t.joinable()) continue;
      while (!done.load()) {
        if (std::chrono::steady_clock::now() > giveUp) CancelSynchronousIo((HANDLE) t.native_handle());
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      t.join();
    }
  }

  bool pollRunning() {
    if (!started_) return false;
    if (exitCode_ != -1) return false;
    DWORD code = 0;
    if (!GetExitCodeProcess(process_, &code)) return false;
    if (code == STILL_ACTIVE && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) return true;
    exitCode_ = (int) code;
    return false;
  }

  HANDLE process_ = nullptr;
  DWORD pid_ = 0;
  std::atomic<bool> outDone_{false}, errDone_{false};
#else
  bool launch(const std::vector<std::string>& args, int flags) {
    int outPipe[2] = {-1, -1}, errPipe[2] = {-1, -1};
    auto closeAll = [&] {
      for (int fd : {outPipe[0], outPipe[1], errPipe[0], errPipe[1]})
        if (fd >= 0) ::close(fd);
    };
    if ((flags & wantStdOut) && !makePipe(outPipe)) return false;
    if ((flags & wantStdErr) && !makePipe(errPipe)) {
      closeAll();
      return false;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    if (outPipe[1] >= 0) posix_spawn_file_actions_adddup2(&actions, outPipe[1], 1);
    else posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
    if (errPipe[1] >= 0) posix_spawn_file_actions_adddup2(&actions, errPipe[1], 2);
    else posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);

    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    sigset_t noSignals, defaults;
    sigemptyset(&noSignals);
    sigemptyset(&defaults);
    for (int s : {SIGPIPE, SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGCHLD, SIGALRM, SIGUSR1, SIGUSR2})
      sigaddset(&defaults, s);
    posix_spawnattr_setsigmask(&attributes, &noSignals);
    posix_spawnattr_setsigdefault(&attributes, &defaults);
    short spawnFlags = (short) (POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
#if defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
    spawnFlags = (short) (spawnFlags | POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif
    posix_spawnattr_setflags(&attributes, spawnFlags);

    std::vector<char*> argv;
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
#if defined(__APPLE__)
    char** envp = *_NSGetEnviron(); // `environ` is not reachable from a bundle
#else
    char** envp = environ;
#endif
    pid_t pid = 0;
    const int r = args[0].find('/') != std::string::npos
                      ? posix_spawn(&pid, args[0].c_str(), &actions, &attributes, argv.data(), envp)
                      : posix_spawnp(&pid, args[0].c_str(), &actions, &attributes, argv.data(), envp);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    if (outPipe[1] >= 0) ::close(outPipe[1]);
    if (errPipe[1] >= 0) ::close(errPipe[1]);
    outPipe[1] = errPipe[1] = -1;
    if (r != 0) {
      closeAll();
      return false;
    }
    pid_ = pid;
    started_ = true;
    startReaders(outPipe[0], errPipe[0]);
    return true;
  }

  static bool makePipe(int fds[2]) {
#if defined(__linux__)
    return pipe2(fds, O_CLOEXEC) == 0;
#else
    if (pipe(fds) != 0) return false;
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return true;
#endif
  }

  void startReaders(int outRead, int errRead) {
    openStreams_ = (outRead >= 0 ? 1 : 0) + (errRead >= 0 ? 1 : 0);
    if (outRead >= 0) outThread_ = std::thread([this, outRead] { drain(outRead, false); });
    if (errRead >= 0) errThread_ = std::thread([this, errRead] { drain(errRead, true); });
  }

  /** poll() with a timeout rather than a blocking read, so stopReaders can
   *  end a reader whose pipe something else is keeping open. */
  void drain(int fd, bool isErr) {
    char buffer[4096];
    for (;;) {
      if (stopReading_.load()) break;
      pollfd p;
      p.fd = fd;
      p.events = POLLIN;
      p.revents = 0;
      const int ready = ::poll(&p, 1, 50);
      if (ready < 0 && errno != EINTR) break;
      if (ready <= 0) continue;
      const ssize_t got = ::read(fd, buffer, sizeof(buffer));
      if (got < 0 && (errno == EINTR || errno == EAGAIN)) continue;
      if (got <= 0) break;
      append(buffer, (size_t) got, isErr);
    }
    ::close(fd);
    streamClosed();
  }

  void stopReaders() {
    for (std::thread* t : {&outThread_, &errThread_}) {
      if (!t->joinable()) continue;
      std::unique_lock<std::mutex> lock(outputMutex_);
      if (!outputChanged_.wait_for(lock, std::chrono::seconds(2), [this] { return openStreams_ == 0; }))
        stopReading_ = true;
      lock.unlock();
      t->join();
    }
  }

  void recordStatus(int status) {
    if (WIFEXITED(status)) exitCode_ = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) exitCode_ = 128 + WTERMSIG(status);
    else exitCode_ = 0;
  }

  bool pollRunning() {
    if (!started_ || exitCode_ != -1 || reaped_) return false;
    int status = 0;
    const pid_t r = waitpid(pid_, &status, WNOHANG);
    if (r == 0) return true;
    reaped_ = true;
    if (r == pid_) recordStatus(status);
    // -1: somebody else reaped it (a host with SIGCHLD ignored does). It has
    // gone; what it returned is not ours to know.
    return false;
  }

  pid_t pid_ = 0;
  bool reaped_ = false;
  std::atomic<bool> stopReading_{false};
#endif

  void append(const char* data, size_t n, bool isErr) {
    {
      std::lock_guard<std::mutex> lock(outputMutex_);
      std::string& own = isErr ? err_ : out_;
      if (own.size() < kMaxCapturedBytes) own.append(data, (std::min)(n, kMaxCapturedBytes - own.size()));
      if (merged_.size() - mergedRead_ < kMaxCapturedBytes) {
        if (mergedRead_ > (1u << 20) && mergedRead_ * 2 > merged_.size()) {
          merged_.erase(0, mergedRead_); // what has been read is not kept
          mergedRead_ = 0;
        }
        merged_.append(data, n);
      }
    }
    outputChanged_.notify_all();
  }
  void streamClosed() {
    {
      std::lock_guard<std::mutex> lock(outputMutex_);
      --openStreams_;
    }
    outputChanged_.notify_all();
  }

  bool started_ = false;
  int exitCode_ = -1;
  std::mutex processMutex_;
  mutable std::mutex outputMutex_;
  std::condition_variable outputChanged_;
  std::string out_, err_, merged_;
  size_t mergedRead_ = 0;
  int openStreams_ = 0;
  std::thread outThread_, errThread_;
};

/**
 * A lock with a name, held by one process (one holder) at a time.
 *
 *   InterProcessLock lock("com.vendor.plugin.cache");
 *   InterProcessLock::ScopedLockType hold(lock);
 *   if (hold.isLocked()) rewriteTheSharedCache();
 *
 * The scope is the user's session: a named mutex in Windows' Local\
 * namespace, and an flock() on a file in the user's temporary folder
 * elsewhere -- so two plugin instances in two hosts, or a plugin and its
 * standalone, exclude each other, and two users on one machine do not.
 *
 * enter() nests within one object (each needs its exit()). A process that
 * dies holding the lock releases it: the system does that for both kinds.
 * On Windows the lock belongs to the THREAD that entered it, and only that
 * thread can exit it.
 *
 * Two objects with ONE name in one process are two holders, and the systems
 * part ways there: on Windows a thread that holds one gets the other at once
 * (the named mutex counts per thread), and elsewhere each excludes the other
 * even on one thread, so entering the second from the thread that holds the
 * first waits out its time-out. Within a process, share one object rather
 * than nest two.
 */
class InterProcessLock {
public:
  /** A lock called `name`, any text: characters a system name cannot hold
   *  are replaced, and a hash of the whole name keeps two names apart that
   *  differ only in those. Nothing is taken until enter(). */
  explicit InterProcessLock(const std::string& name) : name_(processdetail::legalObjectName(name)) {}
  /** Releases the lock if this object holds it, however deeply entered. */
  ~InterProcessLock() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count_ > 0) release();
  }
  ///@{
  /** Not copyable: the held lock has one owner. */
  InterProcessLock(const InterProcessLock&) = delete;
  InterProcessLock& operator=(const InterProcessLock&) = delete;
  ///@}

  /** True once held. timeOutMillisecs negative: wait for ever; 0: try once. */
  bool enter(int timeOutMillisecs = -1) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count_ > 0) {
      ++count_;
      return true;
    }
#if defined(_WIN32)
    const std::wstring wide = pathio::widen(("Local\\sonore-ipl-" + name_).c_str());
    handle_ = CreateMutexW(nullptr, FALSE, wide.c_str());
    if (!handle_) return false;
    const DWORD r = WaitForSingleObject(handle_, timeOutMillisecs < 0 ? INFINITE : (DWORD) timeOutMillisecs);
    // ABANDONED: the last holder died holding it. Ours now, as it should be.
    if (r != WAIT_OBJECT_0 && r != WAIT_ABANDONED) {
      CloseHandle(handle_);
      handle_ = nullptr;
      return false;
    }
#else
    // Per user, and never through a link: see userRendezvousPrefix.
    const std::string path = processdetail::userRendezvousPrefix(4096) + "ipl-" + name_ + ".lock";
#if defined(O_NOFOLLOW)
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
#else
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
#endif
    if (fd_ < 0) return false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeOutMillisecs < 0 ? 0 : timeOutMillisecs);
    int sleepMs = 1;
    while (flock(fd_, LOCK_EX | LOCK_NB) != 0) {
      if (errno != EWOULDBLOCK && errno != EINTR) {
        ::close(fd_);
        fd_ = -1;
        return false;
      }
      if (timeOutMillisecs >= 0 && std::chrono::steady_clock::now() >= deadline) {
        ::close(fd_);
        fd_ = -1;
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
      if (sleepMs < 10) ++sleepMs;
    }
#endif
    count_ = 1;
    return true;
  }

  /** Undoes one enter(); the last one releases the lock for other
   *  processes. Does nothing when not held. On Windows, call it on the
   *  thread that entered. */
  void exit() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count_ > 0 && --count_ == 0) release();
  }

  /** True while this object holds the lock. */
  bool isLocked() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_ > 0;
  }

  /** Enters in its constructor (with a timeout, if given) and exits in its
   *  destructor. isLocked() says whether it got the lock. */
  class ScopedLockType {
  public:
    /** Calls lock.enter(timeOutMillisecs); `lock` must outlive this. */
    explicit ScopedLockType(InterProcessLock& lock, int timeOutMillisecs = -1)
        : lock_(lock), locked_(lock.enter(timeOutMillisecs)) {}
    /** Calls exit() if the enter succeeded. */
    ~ScopedLockType() {
      if (locked_) lock_.exit();
    }
    ///@{
    /** Not copyable: one enter, one exit. */
    ScopedLockType(const ScopedLockType&) = delete;
    ScopedLockType& operator=(const ScopedLockType&) = delete;
    ///@}
    /** Whether the constructor's enter() got the lock. */
    bool isLocked() const noexcept { return locked_; }

  private:
    InterProcessLock& lock_;
    bool locked_;
  };

private:
  void release() {
#if defined(_WIN32)
    if (handle_) {
      ReleaseMutex(handle_);
      CloseHandle(handle_);
      handle_ = nullptr;
    }
#else
    if (fd_ >= 0) {
      flock(fd_, LOCK_UN);
      ::close(fd_);
      fd_ = -1;
    }
#endif
    count_ = 0;
  }

  std::string name_;
  mutable std::mutex mutex_;
  int count_ = 0;
#if defined(_WIN32)
  HANDLE handle_ = nullptr;
#else
  int fd_ = -1;
#endif
};

/**
 * A library opened at run time: how every optional backend in this SDK is
 * reached (JACK, WebKitGTK, libcurl, the ASIO drivers), as an object. The
 * name is a UTF-8 path, or a bare name the system's loader searches for. A
 * full path on Windows has its own folder searched for ITS dependencies
 * (LOAD_WITH_ALTERED_SEARCH_PATH), which is what a plugin loading a library
 * it ships beside itself needs.
 *
 * Closing a library whose functions are still in use is a crash; keep the
 * object alive as long as anything might call into it.
 */
class DynamicLibrary {
public:
  /** Nothing open. */
  DynamicLibrary() = default;
  /** open()s `name`; isOpen() says whether it worked. */
  explicit DynamicLibrary(const std::string& name) { open(name); }
  /** close()s the library: see the class comment. */
  ~DynamicLibrary() { close(); }
  ///@{
  /** Not copyable: two owners would close the library twice. */
  DynamicLibrary(const DynamicLibrary&) = delete;
  DynamicLibrary& operator=(const DynamicLibrary&) = delete;
  ///@}
  /** Takes over `other`'s library; `other` is left with none. */
  DynamicLibrary(DynamicLibrary&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
  /** Closes this one's library, then takes over `other`'s. */
  DynamicLibrary& operator=(DynamicLibrary&& other) noexcept {
    if (this != &other) {
      close();
      handle_ = other.handle_;
      other.handle_ = nullptr;
    }
    return *this;
  }

  /** Closes any library this had, then loads `name` (see the class comment
   *  for how it is searched). False when it cannot be loaded -- missing, or
   *  one of its own dependencies is. */
  bool open(const std::string& name) {
    close();
    if (name.empty()) return false;
#if defined(_WIN32)
    const bool absolute = (name.size() > 2 && name[1] == ':') || (name.size() > 1 && name[0] == '\\' && name[1] == '\\');
    handle_ = (void*) LoadLibraryExW(pathio::widenPath(name.c_str()).c_str(), nullptr,
                                     absolute ? LOAD_WITH_ALTERED_SEARCH_PATH : 0);
#else
    handle_ = dlopen(name.c_str(), RTLD_LOCAL | RTLD_NOW);
#endif
    return handle_ != nullptr;
  }

  /** Releases the library; the system unloads it when nothing else holds
   *  it. Pointers from getFunction() may dangle afterwards. */
  void close() {
    if (!handle_) return;
#if defined(_WIN32)
    FreeLibrary((HMODULE) handle_);
#else
    dlclose(handle_);
#endif
    handle_ = nullptr;
  }

  /** A symbol's address, or null. Cast it to the function's real type. */
  void* getFunction(const std::string& functionName) const noexcept {
    if (!handle_ || functionName.empty()) return nullptr;
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress((HMODULE) handle_, functionName.c_str()));
#else
    return dlsym(handle_, functionName.c_str());
#endif
  }

  /** The HMODULE on Windows, the dlopen() handle elsewhere; null when not
   *  open. Still owned by this object. */
  void* getNativeHandle() const noexcept { return handle_; }
  /** True after a successful open(), until close(). */
  bool isOpen() const noexcept { return handle_ != nullptr; }

private:
  void* handle_ = nullptr;
};

/**
 * A base class for objects that live until the program ends and are deleted
 * together then -- caches, registries, a shared font -- by one call to
 * deleteAll() at shutdown, newest first.
 *
 * Who calls deleteAll(): the standalone's main() after its loop, or a plugin
 * as its module unloads (its entry point's exit/terminate function). Not from
 * a static destructor: by then the objects' own dependencies may already be
 * gone. An object deleted earlier by its owner simply leaves the list.
 *
 * Objects created while deleteAll() runs (one destructor making another
 * object) are deleted too; the list is emptied until it stays empty.
 */
class DeletedAtShutdown {
public:
  /** Deletes every DeletedAtShutdown still alive, newest first. */
  static void deleteAll() {
    for (int round = 0; round < 64; ++round) {
      std::vector<DeletedAtShutdown*> now;
      {
        std::lock_guard<std::mutex> lock(registry().mutex);
        now = registry().objects;
      }
      if (now.empty()) return;
      for (auto it = now.rbegin(); it != now.rend(); ++it) {
        bool stillAlive;
        {
          // One destructor may have deleted a later object already.
          std::lock_guard<std::mutex> lock(registry().mutex);
          const auto& list = registry().objects;
          stillAlive = std::find(list.begin(), list.end(), *it) != list.end();
        }
        if (stillAlive) delete *it;
      }
    }
  }

  /** How many are alive -- for a test, or a leak report at exit. */
  static size_t getNumAlive() {
    std::lock_guard<std::mutex> lock(registry().mutex);
    return registry().objects.size();
  }

protected:
  /** Joins the list deleteAll() deletes. Any thread. */
  DeletedAtShutdown() {
    std::lock_guard<std::mutex> lock(registry().mutex);
    registry().objects.push_back(this);
  }
  /** Leaves the list, whoever deletes the object. */
  virtual ~DeletedAtShutdown() {
    std::lock_guard<std::mutex> lock(registry().mutex);
    auto& list = registry().objects;
    list.erase(std::remove(list.begin(), list.end(), this), list.end());
  }
  ///@{
  /** Not copyable: each object is on the list once. */
  DeletedAtShutdown(const DeletedAtShutdown&) = delete;
  DeletedAtShutdown& operator=(const DeletedAtShutdown&) = delete;
  ///@}

private:
  /** Every live DeletedAtShutdown, oldest first, under one mutex: the
   *  constructor appends, the destructor removes, deleteAll() walks a copy
   *  backwards. */
  struct Registry {
    std::mutex mutex;                        ///< guards `objects`
    std::vector<DeletedAtShutdown*> objects; ///< oldest first; not owned until deleteAll()
  };
  /** Never destroyed: an object deleted during static destruction must
   *  still find the list. */
  static Registry& registry() {
    static Registry* r = new Registry();
    return *r;
  }
};

} // namespace sonore
