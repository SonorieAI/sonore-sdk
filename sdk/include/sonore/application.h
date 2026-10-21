// SPDX-License-Identifier: Apache-2.0
//
// An application built on the SDK: the object that owns the process, and the
// main() that runs it.
//
// ── Not for plugins, and not the plugin standalone ─────────────────────────
//
// A plugin has no application: the host owns the process, its loop and its
// lifetime, and a plugin that made an application singleton would be a second
// one inside somebody else's. standalone.h's main() is the PLUGIN standalone
// and stays what it is. This is for programs that are applications in their
// own right -- an editor, a librarian, a tool -- in the reference's shape:
// subclass Application, answer initialise() and shutdown(), and
// SONORE_START_APPLICATION(MyApp) writes main (and WinMain) for you.
//
// ── What run() does, in order ──────────────────────────────────────────────
//
//   1. The command line, as UTF-8 (on Windows from GetCommandLineW, because
//      main's argv is in the ANSI code page and loses every name outside it).
//   2. MessageManager::getInstance() on this thread, which makes it the
//      message thread, and the unhandled-exception hook pointed at the app.
//   3. If moreThanOneInstanceAllowed() is false: the single-instance lock.
//      The second instance hands its command line to the first, whose
//      anotherInstanceStarted() receives it, and exits without initialising.
//   4. The platform's quit requests wired to systemRequestedQuit(): Windows'
//      end-of-session query and console Ctrl+C/close, AppKit's
//      applicationShouldTerminate: (Cmd-Q, the Dock, log-out), and SIGINT /
//      SIGTERM on Linux through a self-pipe the loop polls.
//   5. initialise(), the dispatch loop until quit(), shutdown(), and the
//      return value.
//
// ── The single-instance lock and its message ───────────────────────────────
//
// Windows: a named mutex in the session's namespace -- the kernel drops it
// with the last handle, so an instance that crashed cannot leave the lock
// held. macOS and Linux: flock on a file in the temporary directory, which the
// kernel likewise releases with the process. The first instance takes the
// lock only AFTER its message thread can hear (its message window or socket
// exists), so a second instance started a moment later cannot find the lock
// held and nobody listening.
//
// The command line travels tagged with the application's name, so every other
// Sonore program ignores it. On Windows it is WM_COPYDATA sent to the first
// instance's own session window, waiting for as long as its initialise()
// keeps it from answering (detail::app::forwardToFirstInstance); on macOS
// MessageManager::broadcastMessage (a distributed notification); on Linux,
// where broadcastMessage has no transport, an abstract-namespace Unix socket
// the first instance listens on.
//
// ── Unhandled exceptions ───────────────────────────────────────────────────
//
// An exception thrown by a queued callback, by a window procedure in
// document_window.h, by initialise() or shutdown() reaches
// unhandledException() and the program carries on (the loop is re-entered);
// the default prints it to stderr. Nothing unwinds into the OS's loop.
//
// RUN on Windows: the whole lifecycle in a child process, the lock and the
// forwarded command line between two processes, the end-of-session query,
// the exception hook. macOS and Linux are compiled only.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "message_manager.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#if defined(_MSC_VER)
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#endif
#elif defined(__APPLE__)
#include <fcntl.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <sys/file.h>
#include <unistd.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace sonore {

class Application;

namespace detail {
namespace app {

inline Application*& current() {
  static Application* instance = nullptr;
  return instance;
}

/** The command line as the arguments arrived, written the way Windows'
 *  own argv parser (CommandLineToArgvW) reads it back: quoted where an
 *  argument has a space or a quote in it, each quote escaped, and a run of
 *  backslashes doubled where it ends at a quote or at the closing quote.
 *  Backslashes anywhere else are literal. Doubling only the run before a
 *  quote is what keeps "C:\My Dir\" from reading its closing quote as an
 *  escaped one and swallowing every argument after it. */
inline std::string joinArguments(const std::vector<std::string>& args) {
  std::string out;
  for (size_t i = 0; i < args.size(); ++i) {
    if (i) out += ' ';
    const std::string& a = args[i];
    const bool quote = a.empty() || a.find_first_of(" \t\"") != std::string::npos;
    if (!quote) {
      out += a;
      continue;
    }
    out += '"';
    for (size_t k = 0;; ++k) {
      size_t backslashes = 0;
      while (k < a.size() && a[k] == '\\') {
        ++k;
        ++backslashes;
      }
      if (k == a.size()) {
        out.append(backslashes * 2, '\\');
        break;
      }
      if (a[k] == '"') {
        out.append(backslashes * 2 + 1, '\\');
      } else {
        out.append(backslashes, '\\');
      }
      out += a[k];
    }
    out += '"';
  }
  return out;
}

/** The inverse of joinArguments, by the same rules: spaces separate, double
 *  quotes group, and a run of backslashes is literal unless a quote follows
 *  it -- then each pair is one backslash, and an odd one left over makes
 *  the quote a literal one. */
inline std::vector<std::string> splitArguments(const std::string& line) {
  std::vector<std::string> out;
  std::string current;
  bool inQuotes = false, have = false;
  for (size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (c == '\\') {
      size_t backslashes = 0;
      while (i < line.size() && line[i] == '\\') {
        ++i;
        ++backslashes;
      }
      have = true;
      if (i < line.size() && line[i] == '"') {
        current.append(backslashes / 2, '\\');
        if (backslashes % 2) {
          current += '"';
          continue; // the quote was this run's: the loop moves past it
        }
      } else {
        current.append(backslashes, '\\');
      }
      --i; // what follows the run is read on its own
    } else if (c == '"') {
      inQuotes = !inQuotes;
      have = true;
    } else if ((c == ' ' || c == '\t') && !inQuotes) {
      if (have) out.push_back(current);
      current.clear();
      have = false;
    } else {
      current += c;
      have = true;
    }
  }
  if (have) out.push_back(current);
  return out;
}

/** A name every OS accepts in a mutex, a file or a socket. */
inline std::string safeName(const std::string& name) {
  std::string out;
  for (char c : name) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '_' || c == '.';
    out += ok ? c : '_';
  }
  return out.empty() ? std::string("app") : out;
}

/** The forwarded command line: tag, name, line, separated by 0x1F (a byte no
 *  application name or UTF-8 text contains by accident). */
constexpr const char* kForwardTag = "SonoreApplication";

inline std::string forwardMessage(const std::string& appName, const std::string& line) {
  return std::string(kForwardTag) + '\x1f' + appName + '\x1f' + line;
}

/** The line, if `message` is a forward for `appName`. */
inline bool parseForward(const std::string& message, const std::string& appName,
                         std::string* line) {
  const std::string head = std::string(kForwardTag) + '\x1f' + appName + '\x1f';
  if (message.compare(0, head.size(), head) != 0) return false;
  *line = message.substr(head.size());
  return true;
}

/**
 * The single-instance lock. Held from acquire() until release() or the
 * destructor -- and by the kernel only for as long as the process lives, so
 * a crash cannot leave it taken.
 */
class InstanceLock {
public:
  explicit InstanceLock(const std::string& appName) : name_(safeName(appName)) {}
  ~InstanceLock() { release(); }
  InstanceLock(const InstanceLock&) = delete;
  InstanceLock& operator=(const InstanceLock&) = delete;

  /** True when this process now holds it; false when another one does. */
  bool acquire() {
    if (held_) return true;
#if defined(_WIN32)
    // Local\: one lock per log-in session, as one instance per user means.
    const std::wstring wide = L"Local\\SonoreApplication-" + widen(name_);
    HANDLE m = CreateMutexW(nullptr, FALSE, wide.c_str());
    if (!m) return false;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
      // Somebody else's: our handle would keep the mutex alive after they
      // exit, and then nobody would ever get it.
      CloseHandle(m);
      return false;
    }
    mutex_ = m;
#else
    const char* tmp = std::getenv("TMPDIR");
    std::string dir = tmp && *tmp ? tmp : "/tmp";
    if (dir.back() != '/') dir += '/';
    path_ = dir + ".sonore-app-" + name_ + "-" + std::to_string((long) getuid()) + ".lock";
    fd_ = open(path_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd_ < 0) return false;
    if (flock(fd_, LOCK_EX | LOCK_NB) != 0) {
      close(fd_);
      fd_ = -1;
      return false;
    }
#endif
    held_ = true;
    return true;
  }

  void release() {
    if (!held_) return;
    held_ = false;
#if defined(_WIN32)
    CloseHandle(mutex_);
    mutex_ = nullptr;
#else
    flock(fd_, LOCK_UN);
    close(fd_);
    fd_ = -1;
#endif
  }

  bool isHeld() const { return held_; }

private:
#if defined(_WIN32)
  static std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int) utf8.size(), nullptr, 0);
    std::wstring out((size_t) (n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int) utf8.size(), &out[0], n);
    return out;
  }
  HANDLE mutex_ = nullptr;
#else
  std::string path_;
  int fd_ = -1;
#endif
  std::string name_;
  bool held_ = false;
};

inline void forwardException(const std::exception* e);

} // namespace app
} // namespace detail

/**
 * The application: one per process, made by SONORE_START_APPLICATION (or
 * handed to Application::run) and destroyed after shutdown().
 *
 * Everything here runs on the message thread -- the main thread.
 */
class Application {
public:
  /** Made by the Factory given to run(), on the message thread, after the
   *  MessageManager exists. */
  Application() = default;
  /** Runs after shutdown(), on the message thread, while the MessageManager
   *  still exists. */
  virtual ~Application() = default;
  ///@{
  /** Not copyable: there is one per process. */
  Application(const Application&) = delete;
  Application& operator=(const Application&) = delete;
  ///@}

  /** The running application, or null (a plugin, or before run()). */
  static Application* getInstance() noexcept { return detail::app::current(); }

  // ── What a subclass answers ───────────────────────────────────────────────

  /** The name, which is also what a single-instance application is told
   *  apart by (the instance lock, the forwarded command line): keep it the
   *  same from one version to the next. */
  virtual const std::string getApplicationName() = 0;
  /** The version as text, for the application's own use -- an About box, a
   *  log line. Nothing here reads it. */
  virtual const std::string getApplicationVersion() = 0;

  /** False makes the application single-instance: a second launch hands
   *  its command line to this one (anotherInstanceStarted) and exits. True
   *  by default, as in the reference. */
  virtual bool moreThanOneInstanceAllowed() { return true; }

  /** The application starts: make its windows here. The command line is
   *  every argument after the program's name, re-joined by the Windows argv
   *  rules (quoted where an argument has a space or a quote in it), so
   *  CommandLineToArgvW splits it back into the same arguments.
   *  getCommandLineParameterArray() has them already split. */
  virtual void initialise(const std::string& commandLineParameters) = 0;

  /** The loop has ended: close windows and release everything here. */
  virtual void shutdown() = 0;

  /** The OS asked the application to quit -- the session is ending, Cmd-Q,
   *  Ctrl+C, SIGTERM. The default quits. */
  virtual void systemRequestedQuit() { quit(); }

  /** A second launch of a single-instance application, with its command
   *  line -- usually a file somebody opened from the desktop. */
  virtual void anotherInstanceStarted(const std::string& commandLine) { (void) commandLine; }

  /** Something threw where nothing caught it -- a queued callback, a window
   *  procedure, initialise or shutdown. `e` is null for something that is not
   *  a std::exception. The program carries on; the default prints it. */
  virtual void unhandledException(const std::exception* e, const std::string& sourceFilename,
                                  int lineNumber) {
    std::fprintf(stderr, "%s: unhandled exception: %s", getApplicationName().c_str(),
                 e ? e->what() : "(not a std::exception)");
    if (!sourceFilename.empty()) std::fprintf(stderr, " (%s:%d)", sourceFilename.c_str(), lineNumber);
    std::fprintf(stderr, "\n");
  }

  ///@{
  /** Mobile life-cycle calls in the reference; a desktop never makes them. */
  virtual void suspended() {}
  virtual void resumed() {}
  virtual void memoryWarningReceived() {}
  ///@}

  // ── What it can ask ──────────────────────────────────────────────────────

  /** End the dispatch loop, after whatever is already queued. Any thread. */
  static void quit() {
    if (MessageManager* mm = MessageManager::getInstanceWithoutCreating()) mm->stopDispatchLoop();
  }

  /** What run() -- and so main -- returns. 0 unless set. */
  void setApplicationReturnValue(int value) noexcept { returnValue_ = value; }
  /** What setApplicationReturnValue() set; 0 by default. */
  int getApplicationReturnValue() const noexcept { return returnValue_; }

  /** The command line run() was given (without the program's name). */
  static std::string getCommandLineParameters() {
    return detail::app::joinArguments(arguments());
  }
  /** The same arguments one per string, UTF-8 -- on Windows converted from
   *  the UTF-16 command line, so any file name arrives intact. */
  static std::vector<std::string> getCommandLineParameterArray() { return arguments(); }

  /** True inside initialise(). */
  bool isInitialising() const noexcept { return initialising_; }

  /** True while an Application is running in this process, false in a
   *  plugin -- where getInstance() is null. */
  static bool isStandaloneApp() noexcept { return getInstance() != nullptr; }

  // ── Running one ──────────────────────────────────────────────────────────

  /** Makes the application with `new`: `[]() -> Application* { return new
   *  MyApp; }`. run() owns what it returns; null makes run() return 1. */
  using Factory = Application* (*) ();

  /**
   * The whole life of an application (see the header), from main's
   * arguments. On Windows argv is not used: the command line is read again
   * as UTF-16 and converted, so a file name in any script arrives intact.
   */
  static int main(int argc, char** argv, Factory create) {
    std::vector<std::string> args;
#if defined(_WIN32)
    (void) argc;
    (void) argv;
    int count = 0;
    if (LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count)) {
      for (int i = 1; i < count; ++i) args.push_back(narrow(wide[i]));
      LocalFree(wide);
    }
    enablePerMonitorDpi();
#else
    for (int i = 1; i < argc; ++i) args.push_back(argv[i] ? argv[i] : "");
#endif
    return run(create, args);
  }

  /** The same, with the arguments given: for a test, or a host program that
   *  parsed its own. */
  static int run(Factory create, const std::vector<std::string>& args);

private:
  static std::vector<std::string>& arguments() {
    static std::vector<std::string> args;
    return args;
  }

#if defined(_WIN32)
  static std::string narrow(const wchar_t* wide) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string out((size_t) n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, &out[0], n, nullptr, nullptr);
    return out;
  }

  /** Per-monitor DPI awareness (v2) for an application that declared none in
   *  its manifest, as the reference does: without it Windows bitmap-scales
   *  every window on a scaled display. Resolved at run time (Windows 10
   *  1703+); a manifest that said otherwise is left alone. */
  static void enablePerMonitorDpi() {
    typedef BOOL(WINAPI * SetContextFn)(HANDLE);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return;
    auto set = (SetContextFn) (void*) GetProcAddress(user32, "SetProcessDpiAwarenessContext");
    if (set) set((HANDLE) (LONG_PTR) -4 /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */);
  }
#endif

  int returnValue_ = 0;
  bool initialising_ = false;
};

namespace detail {
namespace app {

/** MessageManager's hook, pointed at the running application. */
inline void forwardException(const std::exception* e) {
  if (Application* a = current()) a->unhandledException(e, std::string(), 0);
}

/** Hears the forwarded command lines (Windows and macOS). */
class ForwardListener : public MessageManager::BroadcastListener {
public:
  explicit ForwardListener(std::string name) : name_(std::move(name)) {}
  void broadcastMessageReceived(const std::string& message) override {
    std::string line;
    if (!parseForward(message, name_, &line)) return;
    if (Application* a = current()) a->anotherInstanceStarted(line);
  }

private:
  std::string name_;
};

#if defined(_WIN32)

/**
 * A hidden top-level window, because the session's end is told only to
 * top-level windows -- the message manager's window is message-only and never
 * hears it. Its title names the application, which is how a test (or an
 * installer) finds it.
 */
class SessionWindow {
public:
  explicit SessionWindow(const std::string& appName) {
    const wchar_t* cls = windowClass();
    if (!cls) return;
    const std::string title = "SonoreApplication:" + appName;
    const int n = MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, nullptr, 0);
    std::wstring wide((size_t) (n > 0 ? n : 1), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, &wide[0], n);
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, cls, wide.c_str(), WS_POPUP, 0, 0, 0, 0, nullptr,
                            nullptr, module(), nullptr);
  }
  ~SessionWindow() {
    if (hwnd_) DestroyWindow(hwnd_);
  }
  SessionWindow(const SessionWindow&) = delete;
  SessionWindow& operator=(const SessionWindow&) = delete;

private:
  static HMODULE module() {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR) &SessionWindow::module, &m);
    return m;
  }

  static const wchar_t* windowClass() {
    static std::wstring name;
    static ATOM atom = 0;
    if (atom) return name.c_str();
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &SessionWindow::proc;
    wc.hInstance = module();
    for (int attempt = 0; attempt < 64 && !atom; ++attempt) {
      wchar_t buffer[96];
      std::swprintf(buffer, 96, L"SonoreAppSession_%p_%d", (void*) module(), attempt);
      name = buffer;
      wc.lpszClassName = name.c_str();
      atom = RegisterClassExW(&wc);
      if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) break;
    }
    return atom ? name.c_str() : nullptr;
  }

  static LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_COPYDATA) {
      // A second instance's command line (forwardToFirstInstance). Copied and
      // queued, as the message window does with a broadcast: the sender is
      // blocked until this returns.
      const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lp);
      if (data && data->dwData == detail::msgloop::kBroadcastTag) {
        const size_t size = (std::min)((size_t) data->cbData, (size_t) 1 << 20);
        const std::string text(data->lpData ? static_cast<const char*>(data->lpData) : "",
                               data->lpData ? size : 0);
        MessageManager::callAsync([text]() {
          Application* a = current();
          std::string line;
          if (a && parseForward(text, a->getApplicationName(), &line)) a->anotherInstanceStarted(line);
        });
        return TRUE;
      }
    }
    if (message == WM_QUERYENDSESSION) {
      // The reference's answer: ask the application, and agree to end only
      // if it has decided to quit.
      try {
        if (Application* a = current()) a->systemRequestedQuit();
      } catch (const std::exception& e) {
        forwardException(&e);
      } catch (...) {
        forwardException(nullptr);
      }
      MessageManager* mm = MessageManager::getInstanceWithoutCreating();
      return (mm && mm->hasStopMessageBeenSent()) ? TRUE : FALSE;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
  }

  HWND hwnd_ = nullptr;
};

/** How long a second instance waits for the first to take its command line. */
constexpr int kForwardWaitMs = 30000;

/**
 * Hand a second instance's command line to the first one ITSELF, and wait
 * for it to take it.
 *
 * broadcastMessage gives every recipient two seconds and skips one that looks
 * hung -- right for a broadcast, wrong here: the first instance is often still
 * in initialise(), loading the file it was opened with, and does not pump
 * until that is over. Two files opened together from Explorer start two
 * processes at once, and the second file was dropped. So the first
 * instance's session window is found by its title (it appears just after the
 * lock is taken, so a moment's absence is waited out) and sent the line, with
 * no hung-window shortcut, for as long as kForwardWaitMs: a hidden process
 * that has nothing else to do waits rather than lose the file.
 */
inline bool forwardToFirstInstance(const std::string& appName, const std::string& message) {
  const std::string title = "SonoreApplication:" + appName;
  const int n = MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, nullptr, 0);
  std::wstring wide((size_t) (n > 0 ? n : 1), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, &wide[0], n);
  COPYDATASTRUCT data{};
  data.dwData = detail::msgloop::kBroadcastTag;
  data.cbData = (DWORD) message.size();
  data.lpData = (void*) message.data();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kForwardWaitMs);
  for (;;) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                          deadline - std::chrono::steady_clock::now())
                          .count();
    if (left <= 0) return false;
    if (HWND first = FindWindowW(nullptr, wide.c_str())) {
      DWORD_PTR taken = 0;
      if (SendMessageTimeoutW(first, WM_COPYDATA, 0, (LPARAM) &data, SMTO_NORMAL, (UINT) left,
                              &taken) != 0 &&
          taken == TRUE)
        return true;
    }
    Sleep(50);
  }
}

/** How long a closed console's handler holds the process open for shutdown.
 *  Windows itself ends it after five seconds; this is only the backstop. */
constexpr int kConsoleCloseGraceMs = 10000;

/** What a closed console's handler waits for: run() past shutdown() and the
 *  application's destruction. Reset by each run(). */
struct ConsoleCloseWait {
  std::mutex mutex;
  std::condition_variable changed;
  bool finished = false;

  void set(bool value) {
    std::lock_guard<std::mutex> lock(mutex);
    finished = value;
    changed.notify_all();
  }
};

inline ConsoleCloseWait& consoleCloseWait() {
  static ConsoleCloseWait wait;
  return wait;
}

/** Ctrl+C, Ctrl+Break and closing the console arrive on a thread of the
 *  system's; the request is queued for the message thread. */
inline BOOL WINAPI consoleHandler(DWORD type) {
  if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT && type != CTRL_CLOSE_EVENT) return FALSE;
  const bool queued = MessageManager::callAsync([]() {
    if (Application* a = current()) a->systemRequestedQuit();
  });
  // A Ctrl+C is over when this returns and the process carries on. A CLOSE
  // is not: Windows ends the process as soon as the handler returns, and
  // returning at once ended it before the loop had even run the request --
  // no shutdown(), no destructors, every unsaved setting lost. So the
  // handler waits for run() to be done with the application. Windows holds
  // a lock while a handler runs that SetConsoleCtrlHandler also takes, so
  // run() raises the flag BEFORE it removes this handler, never after.
  if (queued && type == CTRL_CLOSE_EVENT) {
    ConsoleCloseWait& wait = consoleCloseWait();
    std::unique_lock<std::mutex> lock(wait.mutex);
    wait.changed.wait_for(lock, std::chrono::milliseconds(kConsoleCloseGraceMs),
                          [&wait]() { return wait.finished; });
  }
  return queued ? TRUE : FALSE;
}

#elif defined(__APPLE__)

template <typename Ret, typename... Args>
Ret objc(id target, const char* selector, Args... args) {
  using Fn = Ret (*)(id, SEL, Args...);
  return reinterpret_cast<Fn>(objc_msgSend)(target, sel_registerName(selector), args...);
}

/** applicationShouldTerminate: -- Cmd-Q, the Dock's Quit, log-out. The
 *  application decides (systemRequestedQuit); AppKit is told to cancel, and
 *  the loop ends through quit() as everywhere else. NSTerminateCancel is 0. */
inline unsigned long appDelegateShouldTerminate(id, SEL, id) {
  if (Application* a = current()) a->systemRequestedQuit();
  return 0ul;
}

/** application:openFile: -- a document opened from the Finder while running. */
inline BOOL appDelegateOpenFile(id, SEL, id, id file) {
  if (!file) return NO;
  const char* utf8 = objc<const char*>(file, "UTF8String");
  if (Application* a = current()) a->anotherInstanceStarted(utf8 ? utf8 : "");
  return YES;
}

inline Class appDelegateClass() {
  static Class klass = [] {
    char name[96];
    std::snprintf(name, sizeof(name), "SonoreAppDelegate_%llx",
                  (unsigned long long) (uintptr_t) (void*) &appDelegateShouldTerminate);
    if (Class existing = objc_getClass(name)) return existing;
    Class c = objc_allocateClassPair(objc_getClass("NSObject"), name, 0);
    if (!c) return objc_getClass(name);
    class_addMethod(c, sel_registerName("applicationShouldTerminate:"),
                    (IMP) appDelegateShouldTerminate, "L@:@");
    class_addMethod(c, sel_registerName("application:openFile:"), (IMP) appDelegateOpenFile,
                    "c@:@@");
    objc_registerClassPair(c);
    return c;
  }();
  return klass;
}

#else

/** The write end of the self-pipe a signal handler may touch: write() is
 *  async-signal-safe, callAsync is not. */
inline std::atomic<int>& signalPipeWrite() {
  static std::atomic<int> fd{-1};
  return fd;
}

inline void quitSignalHandler(int) {
  const int fd = signalPipeWrite().load();
  if (fd >= 0) {
    const char byte = 1;
    const ssize_t ignored = write(fd, &byte, 1);
    (void) ignored;
  }
}

/** The abstract-namespace address for an application's forwarding socket:
 *  no file to clean up, and gone with the process. */
inline socklen_t forwardAddress(const std::string& appName, sockaddr_un* address) {
  std::memset(address, 0, sizeof(*address));
  address->sun_family = AF_UNIX;
  const std::string name =
      "sonore-app-" + safeName(appName) + "-" + std::to_string((long) getuid());
  const size_t room = sizeof(address->sun_path) - 1;
  const size_t length = name.size() < room ? name.size() : room;
  std::memcpy(address->sun_path + 1, name.data(), length); // sun_path[0] = 0: abstract
  return (socklen_t) (offsetof(sockaddr_un, sun_path) + 1 + length);
}

#endif

} // namespace app
} // namespace detail

inline int Application::run(Factory create, const std::vector<std::string>& args) {
  if (!create) return 1;
  arguments() = args;
  const std::string commandLine = detail::app::joinArguments(args);

  MessageManager* mm = MessageManager::getInstance();
  std::unique_ptr<Application> app(create());
  if (!app) {
    MessageManager::deleteInstance();
    return 1;
  }
  detail::app::current() = app.get();
  MessageManager::setUnhandledExceptionHandler(&detail::app::forwardException);
  const std::string name = app->getApplicationName();

  auto finish = [&app](int code) {
    detail::app::current() = nullptr;
    MessageManager::setUnhandledExceptionHandler(nullptr);
    app.reset();
    MessageManager::deleteInstance();
    return code;
  };

  // ── One instance ──────────────────────────────────────────────────────────
  detail::app::InstanceLock lock(name);
#if defined(_WIN32) || defined(__APPLE__)
  detail::app::ForwardListener listener(name);
#else
  int listenFd = -1;
#endif
  if (!app->moreThanOneInstanceAllowed()) {
#if defined(_WIN32) || defined(__APPLE__)
    // Listening BEFORE the lock is taken: see the header.
    mm->registerBroadcastListener(&listener);
    if (!lock.acquire()) {
      mm->deregisterBroadcastListener(&listener);
#if defined(_WIN32)
      // To the first instance's session window, waiting for it; the listener
      // above stays for a second instance of an older build, which broadcasts.
      detail::app::forwardToFirstInstance(name, detail::app::forwardMessage(name, commandLine));
#else
      MessageManager::broadcastMessage(detail::app::forwardMessage(name, commandLine));
#endif
      return finish(0);
    }
#else
    if (lock.acquire()) {
      sockaddr_un address;
      const socklen_t length = detail::app::forwardAddress(name, &address);
      listenFd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
      if (listenFd >= 0 && (bind(listenFd, (const sockaddr*) &address, length) != 0 ||
                            listen(listenFd, 8) != 0)) {
        close(listenFd);
        listenFd = -1;
      }
      if (listenFd >= 0)
        mm->registerFdCallback(listenFd, [](int fd) {
          const int peer = accept(fd, nullptr, nullptr);
          if (peer < 0) return;
          timeval wait{1, 0}; // a sender that stalls must not stall us
          setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait));
          std::string line;
          char buffer[4096];
          for (;;) {
            const ssize_t got = read(peer, buffer, sizeof(buffer));
            if (got <= 0 || line.size() > (1u << 20)) break;
            line.append(buffer, (size_t) got);
          }
          close(peer);
          if (Application* a = detail::app::current()) a->anotherInstanceStarted(line);
        });
    } else {
      // The first instance may be between taking the lock and listening:
      // a few tries over a second and a half.
      sockaddr_un address;
      const socklen_t length = detail::app::forwardAddress(name, &address);
      for (int attempt = 0; attempt < 15; ++attempt) {
        const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) break;
        if (connect(fd, (const sockaddr*) &address, length) == 0) {
          size_t sent = 0;
          while (sent < commandLine.size()) {
            const ssize_t n = write(fd, commandLine.data() + sent, commandLine.size() - sent);
            if (n <= 0) break;
            sent += (size_t) n;
          }
          close(fd);
          break;
        }
        close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      return finish(0);
    }
#endif
  }

  // ── The OS's quit requests ───────────────────────────────────────────────
#if defined(_WIN32)
  detail::app::SessionWindow session(name);
  detail::app::consoleCloseWait().set(false);
  SetConsoleCtrlHandler(&detail::app::consoleHandler, TRUE);
#elif defined(__APPLE__)
  id nsApp = detail::app::objc<id>((id) objc_getClass("NSApplication"), "sharedApplication");
  id delegate = nullptr;
  if (nsApp) {
    // NSApplicationActivationPolicyRegular: a Dock icon and a menu bar.
    detail::app::objc<BOOL>(nsApp, "setActivationPolicy:", (long) 0);
    delegate = detail::app::objc<id>(
        detail::app::objc<id>((id) detail::app::appDelegateClass(), "alloc"), "init");
    if (delegate) detail::app::objc<void>(nsApp, "setDelegate:", delegate);
    detail::app::objc<void>(nsApp, "finishLaunching");
  }
#else
  int signalPipe[2] = {-1, -1};
  if (pipe(signalPipe) == 0) {
    for (int fd : signalPipe) fcntl(fd, F_SETFD, FD_CLOEXEC);
    fcntl(signalPipe[1], F_SETFL, fcntl(signalPipe[1], F_GETFL, 0) | O_NONBLOCK);
    detail::app::signalPipeWrite().store(signalPipe[1]);
    const int readEnd = signalPipe[0];
    mm->registerFdCallback(readEnd, [readEnd](int) {
      char drain[16];
      const ssize_t got = read(readEnd, drain, sizeof(drain)); // a (void) cast does not
      (void) got;                                              // satisfy warn_unused_result
      if (Application* a = detail::app::current()) a->systemRequestedQuit();
    });
    std::signal(SIGINT, &detail::app::quitSignalHandler);
    std::signal(SIGTERM, &detail::app::quitSignalHandler);
  }
#endif

  // ── The application ──────────────────────────────────────────────────────
  app->initialising_ = true;
  try {
    app->initialise(commandLine);
  } catch (const std::exception& e) {
    app->unhandledException(&e, __FILE__, __LINE__);
  } catch (...) {
    app->unhandledException(nullptr, __FILE__, __LINE__);
  }
  app->initialising_ = false;

  // Re-entered after anything that escapes it: the program carries on, as
  // the header promises, until quit().
  for (;;) {
    try {
      mm->runDispatchLoop();
      break;
    } catch (const std::exception& e) {
      app->unhandledException(&e, __FILE__, __LINE__);
    } catch (...) {
      app->unhandledException(nullptr, __FILE__, __LINE__);
    }
  }

  try {
    app->shutdown();
  } catch (const std::exception& e) {
    app->unhandledException(&e, __FILE__, __LINE__);
  } catch (...) {
    app->unhandledException(nullptr, __FILE__, __LINE__);
  }
  const int code = app->getApplicationReturnValue();

#if defined(_WIN32)
  mm->deregisterBroadcastListener(&listener);
#elif defined(__APPLE__)
  mm->deregisterBroadcastListener(&listener);
  if (nsApp && delegate) {
    detail::app::objc<void>(nsApp, "setDelegate:", (id) nullptr);
    detail::app::objc<void>(delegate, "release");
  }
#else
  if (signalPipe[0] >= 0) {
    std::signal(SIGINT, SIG_DFL);
    std::signal(SIGTERM, SIG_DFL);
    detail::app::signalPipeWrite().store(-1);
    mm->unregisterFdCallback(signalPipe[0]);
    close(signalPipe[0]);
    close(signalPipe[1]);
  }
  if (listenFd >= 0) {
    mm->unregisterFdCallback(listenFd);
    close(listenFd);
  }
#endif
  lock.release();
#if defined(_WIN32)
  const int result = finish(code);
  // Only now, with the application destroyed, is a closed console's handler
  // let go -- and before it is removed, which would wait for it: see
  // consoleHandler.
  detail::app::consoleCloseWait().set(true);
  SetConsoleCtrlHandler(&detail::app::consoleHandler, FALSE);
  return result;
#else
  return finish(code);
#endif
}

} // namespace sonore

/**
 * main (and, on Windows, WinMain) for an application class:
 *
 *   class MyApp : public sonore::Application { ... };
 *   SONORE_START_APPLICATION(MyApp)
 *
 * Both entry points are defined on Windows; the linker's subsystem picks one
 * (/SUBSYSTEM:WINDOWS for a GUI program with no console, WinMain), and either
 * reads the command line as UTF-16.
 */
#if defined(_WIN32)
#define SONORE_START_APPLICATION(AppClass)                                                       \
  static ::sonore::Application* sonoreCreateApplication() { return new AppClass(); }           \
  int main(int argc, char** argv) {                                                              \
    return ::sonore::Application::main(argc, argv, &sonoreCreateApplication);                   \
  }                                                                                              \
  int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {                                        \
    return ::sonore::Application::main(0, nullptr, &sonoreCreateApplication);                   \
  }
#else
#define SONORE_START_APPLICATION(AppClass)                                                       \
  static ::sonore::Application* sonoreCreateApplication() { return new AppClass(); }           \
  int main(int argc, char** argv) {                                                              \
    return ::sonore::Application::main(argc, argv, &sonoreCreateApplication);                   \
  }
#endif
