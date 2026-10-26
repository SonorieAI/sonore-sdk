// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: processes talking to each other.
//
//   NamedPipe                      a local, two-way, connection-oriented
//                                  channel with a name
//   InterprocessConnection         whole messages over a socket or a pipe,
//                                  delivered to callbacks on its own thread
//   InterprocessConnectionServer   accepts socket connections into them
//   ChildProcessCoordinator /      a worker process of the program's choosing,
//   ChildProcessWorker             launched and connected, pinged, and noticed
//                                  when it dies or hangs
//   NetworkServiceDiscovery        "who on this network offers service X":
//                                  UDP broadcast advertiser and listener
//
// ── Why each is built the way it is ─────────────────────────────────────────
//
// NamedPipe on Windows is a named pipe (\\.\pipe\name), overlapped so every
// read and write can be given a timeout and cancelled, and created with
// PIPE_REJECT_REMOTE_CLIENTS: a plugin's pipe is for this machine. On macOS
// and Linux it is a Unix-domain socket in the user's own runtime directory, owner-only --
// NOT the pair of FIFOs the reference uses. A FIFO cannot say whether the
// process at the other end has died (its reader sees end-of-file both before
// the writer arrives and after it is gone, so the reference opens it for
// writing too, and then never sees an end at all), and a write to a FIFO
// whose reader has gone raises SIGPIPE, whose default is to end the process
// -- the host. A Unix socket is two-way, reports the peer's death as an end
// of stream at once, and is written with MSG_NOSIGNAL/SO_NOSIGPIPE.
//
// InterprocessConnection frames each message as the reference does: a 32-bit
// magic number and a 32-bit length, little-endian, then the bytes -- so a
// process built with the reference that uses the same magic over a SOCKET
// can talk to one built with this. A frame with the wrong magic, or longer
// than the connection's limit (64 MB unless set), ends the connection: a
// length field from the wire is never trusted with an allocation.
//
// ChildProcessCoordinator/Worker: the worker is launched (ChildProcess,
// process.h) with "<uniqueID>:<pipeName>" among its arguments, connects to
// the pipe the coordinator created, and both sides ping every quarter of the
// timeout. A worker that crashes is noticed at once (the pipe ends); one that
// hangs is noticed when it has been silent for the timeout. Each message
// carries a type byte in front of the caller's bytes, so pings can never be
// confused with a message that happens to look like one.
//
// NetworkServiceDiscovery sends a small text datagram (not the reference's
// XML: there is no XML parser here to read it with, and a datagram from the
// network is untrusted input -- the parser is fuzzed). Advertisements expire
// when they stop arriving.
//
// ── Threads ─────────────────────────────────────────────────────────────────
//
// Every callback here -- connectionMade, messageReceived, connectionLost,
// the coordinator's and worker's handlers, a service list's change callback
// -- runs on the object's OWN thread, never a UI thread and never the audio
// thread: hand what arrives over (a UiEventQueue, a lock-free FIFO). A
// subclass must call disconnect() / stop() in its own destructor, before its
// members go, because the thread may be inside one of its overrides. Nothing
// here is for the audio thread.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "net.h"
#include "process.h"
#include "uuid.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace sonore {

// ═══════════════════════════════════════════════════════════════════════════
// NamedPipe
// ═══════════════════════════════════════════════════════════════════════════

/**
 * A two-way channel between two processes on this machine, found by name.
 *
 * One side creates it (createNewPipe) and waits for the other to open it
 * (openExisting). The creator serves one client at a time: when that client
 * goes, its next read or write waits for another.
 *
 * read() waits until the buffer is full or the timeout passes, as the
 * reference's does; readSome() returns as soon as anything arrives, which is
 * what a reader of framed messages wants. Both return -1 when the other side
 * has gone (or on an error) with nothing read.
 *
 * One thread may read while another writes. close() from any thread makes a
 * blocked read or write return.
 */
class NamedPipe {
public:
  /** Closed: createNewPipe() or openExisting() opens it. */
  NamedPipe() = default;
  /** close()s it, waiting for a read or write in progress to return. */
  ~NamedPipe() { close(); }
  ///@{
  /** Not copyable: the pipe has one owner. */
  NamedPipe(const NamedPipe&) = delete;
  NamedPipe& operator=(const NamedPipe&) = delete;
  ///@}

  /** Opens a pipe another process created. False if there is none. */
  bool openExisting(const std::string& pipeName) {
    close();
    std::lock_guard<std::mutex> state(stateMutex_);
    name_ = pipeName;
    server_ = false;
#if defined(_WIN32)
    const std::wstring path = windowsPath(pipeName);
    for (int attempt = 0; attempt < 3; ++attempt) {
      pipe_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                          FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
      if (pipe_ != INVALID_HANDLE_VALUE) break;
      // Busy: its one instance is serving somebody else for the moment.
      if (GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipeW(path.c_str(), 1000)) return false;
    }
    if (pipe_ == INVALID_HANDLE_VALUE) return false;
    if (!makeEvents()) {
      closeHandles();
      return false;
    }
#else
    if (!makeWakePipe()) return false;
    // Every failure from here closes what it opened: the wake pipe's two
    // descriptors leaked on each refused attempt, and connectToPipe retries
    // every 10 ms -- some 200 descriptors a second until the process ran out.
    sockaddr_un address;
    if (!socketPath(pipeName, address)) {
      closeHandles();
      return false;
    }
    connection_ = openUnixSocket();
    if (connection_ < 0) {
      closeHandles();
      return false;
    }
    if (::connect(connection_, (const sockaddr*) &address, sizeof(address)) != 0) {
      closeHandles();
      return false;
    }
#endif
    connected_ = true;
    open_ = true;
    cancelled_ = false;
    return true;
  }

  /**
   * Creates the pipe and starts serving it. With mustNotExist, fails if a pipe
   * of this name is already being served; otherwise a stale one (left by a
   * process that crashed) is replaced.
   */
  bool createNewPipe(const std::string& pipeName, bool mustNotExist = false) {
    close();
    std::lock_guard<std::mutex> state(stateMutex_);
    name_ = pipeName;
    server_ = true;
#if defined(_WIN32)
    const std::wstring path = windowsPath(pipeName);
    pipe_ = CreateNamedPipeW(path.c_str(),
                             PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (mustNotExist ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                             PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                             PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, nullptr);
    if (pipe_ == INVALID_HANDLE_VALUE) return false;
    if (!makeEvents()) {
      closeHandles();
      return false;
    }
    if (listen()) connected_ = true;
#else
    if (!makeWakePipe()) return false;
    sockaddr_un address;
    if (!socketPath(pipeName, address)) {
      closeHandles();
      return false;
    }
    // A socket file already there: in use if something answers it, stale
    // (its server crashed) if nothing does.
    const int probe = openUnixSocket();
    if (probe >= 0) {
      const bool live = ::connect(probe, (const sockaddr*) &address, sizeof(address)) == 0;
      ::close(probe);
      if (live && mustNotExist) {
        closeHandles();
        return false;
      }
    }
    ::unlink(address.sun_path);
    listener_ = openUnixSocket();
    // A backlog of more than one. A probe like the one above, from a second
    // createNewPipe(), connects and hangs up, and stays in this queue until
    // it is accepted and skipped. Linux queues backlog + 1 and makes a
    // connect wait for room; macOS refuses a connect to a full queue at once,
    // so with a backlog of 1 the probe took the only place and the real
    // client after it was refused ("openExisting connects to it" failed, the
    // NEXT client worked). Clients are still served one at a time.
    if (listener_ < 0 || ::bind(listener_, (const sockaddr*) &address, sizeof(address)) != 0 ||
        ::listen(listener_, 8) != 0) {
      closeHandles();
      return false;
    }
    ::chmod(address.sun_path, 0600); // this user's processes only
    socketFile_ = address.sun_path;
#endif
    open_ = true;
    cancelled_ = false;
    return true;
  }

  /** Closes it; a creator's pipe stops existing. Wakes blocked readers and writers. */
  void close() {
    {
      std::lock_guard<std::mutex> state(stateMutex_);
      if (!open_ && !hasHandles()) return;
      cancelled_ = true;
#if defined(_WIN32)
      if (cancelEvent_) SetEvent(cancelEvent_);
#else
      if (wake_[1] >= 0) {
        const char byte = 1;
        const ssize_t ignored = ::write(wake_[1], &byte, 1);
        (void) ignored;
      }
#endif
    }
    // Whoever is reading or writing returns now; then the handles can go.
    std::lock_guard<std::mutex> reading(readMutex_);
    std::lock_guard<std::mutex> writing(writeMutex_);
    std::lock_guard<std::mutex> connecting(connectMutex_);
    std::lock_guard<std::mutex> state(stateMutex_);
    closeHandles();
    open_ = false;
    connected_ = false;
  }

  /** True from a successful createNewPipe() or openExisting() until
   *  close(). Open is not connected: see isConnected(). */
  bool isOpen() const {
    std::lock_guard<std::mutex> state(stateMutex_);
    return open_;
  }
  /** For a creator: whether a client is attached now. An opener is connected
   *  until the creator goes. */
  bool isConnected() const {
    std::lock_guard<std::mutex> state(stateMutex_);
    return open_ && connected_;
  }
  /** The name last given to createNewPipe() or openExisting(), as given. */
  std::string getName() const {
    std::lock_guard<std::mutex> state(stateMutex_);
    return name_;
  }

  /** A creator: waits for a client. True once one is attached. */
  bool waitForConnection(int timeOutMilliseconds) {
    if (!isOpen()) return false;
    if (!server_) return isConnected();
    return ensureConnected(timeOutMilliseconds);
  }

  /** Reads until maxBytesToRead have arrived or the timeout (negative: none)
   *  passes. The count read -- possibly short -- or -1 if the other side went
   *  or the pipe failed before anything arrived. */
  int read(void* destBuffer, int maxBytesToRead, int timeOutMilliseconds) {
    if (!destBuffer || maxBytesToRead <= 0) return 0;
    const auto start = std::chrono::steady_clock::now();
    int done = 0;
    while (done < maxBytesToRead) {
      const int left = remaining(start, timeOutMilliseconds);
      if (left == 0 && done > 0) break;
      const int got = readSome(static_cast<char*>(destBuffer) + done, maxBytesToRead - done, left);
      if (got < 0) return done > 0 ? done : -1;
      done += got;
      if (got == 0 && remaining(start, timeOutMilliseconds) == 0) break;
    }
    return done;
  }

  /** Whatever is there, up to maxBytesToRead, waiting up to the timeout for
   *  something: >0 bytes, 0 on timeout, -1 if the other side went. */
  int readSome(void* destBuffer, int maxBytesToRead, int timeOutMilliseconds) {
    if (!destBuffer || maxBytesToRead <= 0) return 0;
    const auto start = std::chrono::steady_clock::now();
    if (server_ && !ensureConnected(timeOutMilliseconds)) return isCancelled() ? -1 : 0;
    std::lock_guard<std::mutex> reading(readMutex_);
    if (isCancelled() || !hasHandles()) return -1;
#if defined(_WIN32)
    for (;;) {
      OVERLAPPED ov;
      std::memset(&ov, 0, sizeof(ov));
      ov.hEvent = readEvent_;
      ResetEvent(readEvent_);
      DWORD got = 0;
      if (!ReadFile(pipe_, destBuffer, (DWORD) maxBytesToRead, nullptr, &ov) && GetLastError() != ERROR_IO_PENDING)
        return broken();
      const int outcome = finish(ov, got, remaining(start, timeOutMilliseconds));
      if (outcome == kTimedOut) return 0;
      if (outcome != kCompleted) return isCancelled() ? -1 : broken();
      if (got > 0) return (int) got;
      // A zero-length write at the other end: nothing to report yet.
      if (remaining(start, timeOutMilliseconds) == 0) return 0;
    }
#else
    pollfd p[2];
    p[0].fd = connection_;
    p[0].events = POLLIN;
    p[1].fd = wake_[0];
    p[1].events = POLLIN;
    for (;;) {
      p[0].revents = p[1].revents = 0;
      const int r = ::poll(p, 2, remaining(start, timeOutMilliseconds));
      if (r < 0 && errno == EINTR) continue;
      if (r < 0 || (p[1].revents & POLLIN)) return -1;
      if (r == 0) return 0;
      const ssize_t got = ::recv(connection_, destBuffer, (size_t) maxBytesToRead, 0);
      if (got < 0 && (errno == EINTR || errno == EAGAIN)) continue;
      if (got <= 0) return broken();
      return (int) got;
    }
#endif
  }

  /** Writes all of it within the timeout (negative: none). The count
   *  written, or -1. After a timeout on Windows, -1 does not promise that
   *  none of it went: a write the reader had begun to take, then cancelled,
   *  is reported as nothing written (measured: -1, with 64 KB of it read). */
  int write(const void* sourceBuffer, int numBytesToWrite, int timeOutMilliseconds) {
    if (numBytesToWrite <= 0) return 0;
    if (!sourceBuffer) return -1;
    const auto start = std::chrono::steady_clock::now();
    if (server_ && !ensureConnected(timeOutMilliseconds)) return -1;
    std::lock_guard<std::mutex> writing(writeMutex_);
    if (isCancelled() || !hasHandles()) return -1;
    const char* data = static_cast<const char*>(sourceBuffer);
    int done = 0;
    while (done < numBytesToWrite) {
#if defined(_WIN32)
      OVERLAPPED ov;
      std::memset(&ov, 0, sizeof(ov));
      ov.hEvent = writeEvent_;
      ResetEvent(writeEvent_);
      DWORD wrote = 0;
      if (!WriteFile(pipe_, data + done, (DWORD) (numBytesToWrite - done), nullptr, &ov) &&
          GetLastError() != ERROR_IO_PENDING) {
        broken();
        return -1;
      }
      const int outcome = finish(ov, wrote, remaining(start, timeOutMilliseconds));
      if (outcome != kCompleted) {
        if (outcome == kFailed && !isCancelled()) broken();
        return done + (int) wrote > 0 ? done + (int) wrote : -1;
      }
      done += (int) wrote;
#else
      pollfd p[2];
      p[0].fd = connection_;
      p[0].events = POLLOUT;
      p[1].fd = wake_[0];
      p[1].events = POLLIN;
      p[0].revents = p[1].revents = 0;
      const int r = ::poll(p, 2, remaining(start, timeOutMilliseconds));
      if (r < 0 && errno == EINTR) continue;
      if (r <= 0 || (p[1].revents & POLLIN)) return done > 0 ? done : -1;
      // MSG_DONTWAIT: the descriptor blocks, and a blocking send waits until
      // ALL of it is in the peer's buffer, past the timeout for as long as
      // the reader is busy. A megabyte to a reader 2 s in a handler was sent
      // after 2 s on Linux, under a 1 s timeout. Now what fits goes, and the
      // poll above waits for room within what is left of the timeout.
#if defined(MSG_NOSIGNAL)
      const ssize_t wrote =
          ::send(connection_, data + done, (size_t) (numBytesToWrite - done), MSG_NOSIGNAL | MSG_DONTWAIT);
#else
      const ssize_t wrote = ::send(connection_, data + done, (size_t) (numBytesToWrite - done), MSG_DONTWAIT);
#endif
      if (wrote < 0 && (errno == EINTR || errno == EAGAIN)) continue;
      if (wrote <= 0) {
        broken();
        return done > 0 ? done : -1;
      }
      done += (int) wrote;
#endif
    }
    return done;
  }

private:
  static int remaining(std::chrono::steady_clock::time_point start, int timeoutMs) {
    if (timeoutMs < 0) return -1;
    const auto spent =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    return spent >= timeoutMs ? 0 : (int) (timeoutMs - spent);
  }
  bool isCancelled() const {
    std::lock_guard<std::mutex> state(stateMutex_);
    return cancelled_;
  }

  /**
   * The other side has gone. An opener is finished. A creator goes back to
   * listening AT ONCE -- on Windows a disconnected instance refuses clients
   * (they see "busy") until it is told to connect again, so waiting for the
   * next read to do that left the next client knocking at a closed door.
   * Only the first to notice acts: the reader and the writer may both see it.
   */
  int broken() {
    std::lock_guard<std::mutex> connecting(connectMutex_);
    std::lock_guard<std::mutex> state(stateMutex_);
    if (!connected_) return -1;
    connected_ = false;
    if (!server_ || cancelled_) return -1;
#if defined(_WIN32)
    DisconnectNamedPipe(pipe_);
    if (listen()) connected_ = true; // a client was already waiting
#else
    // Shut, not closed: a writer on another thread may hold the descriptor,
    // and a closed number can be reused by anything. The next client is
    // put into the same number (see ensureConnected).
    if (connection_ >= 0) ::shutdown(connection_, SHUT_RDWR);
#endif
    return -1;
  }

  bool ensureConnected(int timeoutMs) {
    std::lock_guard<std::mutex> connecting(connectMutex_);
    {
      std::lock_guard<std::mutex> state(stateMutex_);
      if (cancelled_ || !open_) return false;
      if (connected_) return true;
    }
#if defined(_WIN32)
    if (!connectPending_ && !listen()) {
      if (!connectPending_) return false;
    } else if (!connectPending_) {
      std::lock_guard<std::mutex> state(stateMutex_);
      connected_ = true;
      return true;
    }
    // Waiting for a client. On a timeout the connect stays pending -- the
    // pipe keeps listening, and the next call carries on waiting for it.
    HANDLE waits[2] = {connectEvent_, cancelEvent_};
    if (WaitForMultipleObjects(2, waits, FALSE, timeoutMs < 0 ? INFINITE : (DWORD) timeoutMs) != WAIT_OBJECT_0)
      return false;
    DWORD ignored = 0;
    const bool joined = GetOverlappedResult(pipe_, &connectOv_, &ignored, FALSE) != 0;
    connectPending_ = false;
    if (!joined) {
      // A client that came and went before we looked: listen again.
      DisconnectNamedPipe(pipe_);
      listen();
      return false;
    }
#else
    pollfd p[2];
    p[0].fd = listener_;
    p[0].events = POLLIN;
    p[1].fd = wake_[0];
    p[1].events = POLLIN;
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
      p[0].revents = p[1].revents = 0;
      const int r = ::poll(p, 2, remaining(start, timeoutMs));
      if (r < 0 && errno == EINTR) continue;
      if (r <= 0 || (p[1].revents & POLLIN)) return false;
      const int client = ::accept(listener_, nullptr, nullptr);
      if (client < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == ECONNABORTED) continue;
        return false;
      }
      // A client that connected and hung up before it was accepted has
      // nothing to say, and another process's createNewPipe() asking whether
      // this name is served is exactly that. Taken as THE client, its hang-up
      // ended the connection at the first read, and the real client's bytes
      // waited behind it. Skipped, as Windows skips one that came and went.
      pollfd gone;
      gone.fd = client;
      gone.events = POLLIN;
      gone.revents = 0;
      char peek = 0;
      if (::poll(&gone, 1, 0) == 1 && ::recv(client, &peek, 1, MSG_PEEK | MSG_DONTWAIT) == 0) {
        ::close(client);
        continue;
      }
#if defined(SO_NOSIGPIPE)
      const int one = 1;
      ::setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
      std::lock_guard<std::mutex> state(stateMutex_);
      if (connection_ >= 0) {
        // Into the previous client's number, so a thread still holding that
        // number never finds it belonging to something else.
        ::dup2(client, connection_);
        ::close(client);
      } else {
        connection_ = client;
      }
      ::fcntl(connection_, F_SETFD, FD_CLOEXEC); // dup2 clears it
      break;
    }
#endif
    std::lock_guard<std::mutex> state(stateMutex_);
    connected_ = true;
    return true;
  }

#if defined(_WIN32)
  static std::wstring windowsPath(const std::string& name) {
    std::string n = name;
    std::replace(n.begin(), n.end(), '\\', '/');
    return pathio::widen(("\\\\.\\pipe\\" + n).c_str());
  }

  bool makeEvents() {
    readEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    writeEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    connectEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    cancelEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    return readEvent_ && writeEvent_ && connectEvent_ && cancelEvent_;
  }

  /** Starts an overlapped ConnectNamedPipe; true if a client is already
   *  connected. The caller holds connectMutex_ (or is creating the pipe). */
  bool listen() {
    std::memset(&connectOv_, 0, sizeof(connectOv_));
    connectOv_.hEvent = connectEvent_;
    ResetEvent(connectEvent_);
    connectPending_ = false;
    if (ConnectNamedPipe(pipe_, &connectOv_)) return true;
    const DWORD e = GetLastError();
    if (e == ERROR_PIPE_CONNECTED) return true;
    if (e == ERROR_IO_PENDING) {
      connectPending_ = true;
      return false;
    }
    if (e == ERROR_NO_DATA) { // came and went already
      DisconnectNamedPipe(pipe_);
      if (ConnectNamedPipe(pipe_, &connectOv_)) return true;
      const DWORD again = GetLastError();
      connectPending_ = again == ERROR_IO_PENDING;
      return again == ERROR_PIPE_CONNECTED;
    }
    return false;
  }

  static constexpr int kCompleted = 1, kTimedOut = 0, kFailed = -1;

  /** Waits for an overlapped read or write, the timeout, or close(). On the
   *  latter two it is cancelled AND waited for -- the OVERLAPPED is on the
   *  caller's stack, and the system must be done with it before that
   *  returns. Bytes that arrived anyway are in `transferred`. */
  int finish(OVERLAPPED& ov, DWORD& transferred, int timeoutMs) {
    HANDLE waits[2] = {ov.hEvent, cancelEvent_};
    const DWORD r = WaitForMultipleObjects(2, waits, FALSE, timeoutMs < 0 ? INFINITE : (DWORD) timeoutMs);
    if (r == WAIT_OBJECT_0) return GetOverlappedResult(pipe_, &ov, &transferred, FALSE) ? kCompleted : kFailed;
    CancelIoEx(pipe_, &ov);
    if (GetOverlappedResult(pipe_, &ov, &transferred, TRUE)) return kCompleted;
    return r == WAIT_TIMEOUT && GetLastError() == ERROR_OPERATION_ABORTED ? kTimedOut : kFailed;
  }

  bool hasHandles() const { return pipe_ != INVALID_HANDLE_VALUE; }

  void closeHandles() {
    if (pipe_ != INVALID_HANDLE_VALUE) {
      if (connectPending_) {
        // The pending connect writes into connectOv_: done with it first.
        DWORD ignored = 0;
        CancelIoEx(pipe_, &connectOv_);
        GetOverlappedResult(pipe_, &connectOv_, &ignored, TRUE);
        connectPending_ = false;
      }
      if (server_) DisconnectNamedPipe(pipe_);
      CloseHandle(pipe_);
    }
    pipe_ = INVALID_HANDLE_VALUE;
    for (HANDLE* h : {&readEvent_, &writeEvent_, &connectEvent_, &cancelEvent_}) {
      if (*h) CloseHandle(*h);
      *h = nullptr;
    }
  }

  HANDLE pipe_ = INVALID_HANDLE_VALUE;
  HANDLE readEvent_ = nullptr, writeEvent_ = nullptr, connectEvent_ = nullptr, cancelEvent_ = nullptr;
  OVERLAPPED connectOv_ = {};
  bool connectPending_ = false; // under connectMutex_
#else
  /** For a bare name, the user's own: $XDG_RUNTIME_DIR/sonore-<legal name>,
   *  or /tmp/sonore-<uid>-<legal name> (processdetail::userRendezvousPrefix);
   *  a name with a '/' is a path. Short, because a socket path has about a
   *  hundred bytes. */
  static bool socketPath(const std::string& name, sockaddr_un& out) {
    std::memset(&out, 0, sizeof(out));
    out.sun_family = AF_UNIX;
#if defined(__ANDROID__)
    // Android: a bare name goes in the ABSTRACT namespace -- a leading NUL, no
    // file. SELinux refuses socket files almost everywhere (an adb shell's
    // /data/local/tmp, an app's anything outside its own folder), and an
    // abstract name needs none; the user id in it keeps one user's from
    // another's, as the file's 0600 does elsewhere. Gone with its listener,
    // so there is never a stale one to clear.
    if (name.find('/') == std::string::npos) {
      char uid[24];
      std::snprintf(uid, sizeof(uid), "%lu", (unsigned long) ::getuid());
      const std::string abstractName =
          "sonore-" + std::string(uid) + "-" + processdetail::legalObjectName(name, 40);
      if (abstractName.size() + 1 >= sizeof(out.sun_path)) return false;
      std::memcpy(out.sun_path + 1, abstractName.c_str(), abstractName.size());
      return true;
    }
#endif
    const std::string path = name.find('/') != std::string::npos
                                 ? name
                                 : processdetail::userRendezvousPrefix(sizeof(out.sun_path)) +
                                       processdetail::legalObjectName(name, 40);
    if (path.size() >= sizeof(out.sun_path)) return false;
    std::memcpy(out.sun_path, path.c_str(), path.size() + 1);
    return true;
  }

  static int openUnixSocket() {
#if defined(SOCK_CLOEXEC)
    const int s = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
#else
    const int s = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (s >= 0) ::fcntl(s, F_SETFD, FD_CLOEXEC);
#endif
#if defined(SO_NOSIGPIPE)
    if (s >= 0) {
      const int one = 1;
      ::setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    }
#endif
    return s;
  }

  bool makeWakePipe() {
    if (::pipe(wake_) != 0) {
      wake_[0] = wake_[1] = -1;
      return false;
    }
    for (int fd : wake_) {
      ::fcntl(fd, F_SETFD, FD_CLOEXEC);
      ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    }
    return true;
  }

  // The wake pipe counts: close() returned early when only it was open, and
  // the next open made a new one over it.
  bool hasHandles() const {
    return connection_ >= 0 || listener_ >= 0 || wake_[0] >= 0 || wake_[1] >= 0;
  }

  void closeHandles() {
    for (int* fd : {&connection_, &listener_, &wake_[0], &wake_[1]}) {
      if (*fd >= 0) ::close(*fd);
      *fd = -1;
    }
    if (!socketFile_.empty()) ::unlink(socketFile_.c_str());
    socketFile_.clear();
  }

  int connection_ = -1;
  int listener_ = -1;
  int wake_[2] = {-1, -1};
  std::string socketFile_;
#endif

  mutable std::mutex stateMutex_;
  std::mutex readMutex_, writeMutex_, connectMutex_;
  std::string name_;
  bool server_ = false;
  bool open_ = false;
  bool connected_ = false;
  bool cancelled_ = false;
};

// ═══════════════════════════════════════════════════════════════════════════
// InterprocessConnection
// ═══════════════════════════════════════════════════════════════════════════

namespace ipcdetail {

/** One frame: magic, length (both 32-bit little-endian), payload. */
inline std::vector<uint8_t> encodeFrame(uint32_t magic, const void* data, size_t size) {
  std::vector<uint8_t> out(8 + size);
  for (int i = 0; i < 4; ++i) {
    out[(size_t) i] = (uint8_t) (magic >> (8 * i));
    out[(size_t) (4 + i)] = (uint8_t) ((uint32_t) size >> (8 * i));
  }
  if (size) std::memcpy(out.data() + 8, data, size);
  return out;
}

/**
 * Bytes in, whole messages out, however the bytes were split on the way.
 * After a protocol error (a wrong magic number, a length past the limit) it
 * refuses everything: the stream has lost its framing and nothing after that
 * point can be trusted to be where a header says.
 */
class FrameDecoder {
public:
  FrameDecoder(uint32_t magic, size_t maxMessageSize) : magic_(magic), max_(maxMessageSize) {}

  template <typename Sink>
  bool feed(const uint8_t* data, size_t n, Sink&& sink) {
    if (failed_) return false;
    while (n > 0) {
      if (header_ < 8) {
        const size_t take = (std::min)(n, (size_t) 8 - header_);
        std::memcpy(headerBytes_ + header_, data, take);
        header_ += take;
        data += take;
        n -= take;
        if (header_ < 8) break;
        uint32_t magic = 0, size = 0;
        for (int i = 0; i < 4; ++i) {
          magic |= (uint32_t) headerBytes_[i] << (8 * i);
          size |= (uint32_t) headerBytes_[4 + i] << (8 * i);
        }
        if (magic != magic_ || size > max_) {
          failed_ = true;
          return false;
        }
        expected_ = size;
        body_.clear();
        body_.reserve((std::min)((size_t) size, (size_t) 1 << 20)); // grows with what arrives
      }
      const size_t take = (std::min)(n, expected_ - body_.size());
      body_.insert(body_.end(), data, data + take);
      data += take;
      n -= take;
      // (A zero-length message completes here too, with its header.)
      if (body_.size() == expected_) {
        sink(body_);
        header_ = 0;
        body_.clear();
      }
    }
    return true;
  }

  bool hasFailed() const noexcept { return failed_; }

private:
  uint32_t magic_;
  size_t max_;
  uint8_t headerBytes_[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  size_t header_ = 0;
  size_t expected_ = 0;
  std::vector<uint8_t> body_;
  bool failed_ = false;
};

} // namespace ipcdetail

/**
 * Whole messages between two processes, over a TCP socket or a NamedPipe.
 *
 * Subclass it and implement the three callbacks; they run on the
 * connection's own thread (see the header), which starts when the connection
 * does. connectionMade() comes first -- for a created pipe, when a client
 * arrives -- then messageReceived() per message, then connectionLost() once,
 * whether the other side went or disconnect() was called. Call disconnect()
 * in the subclass's destructor.
 *
 * sendMessage may be called from any thread, including from inside
 * messageReceived, and so may disconnect(). connectToSocket, connectToPipe
 * and createPipe may not: from inside a callback they return false, and a
 * client that reconnects when the link drops does it from a thread of its
 * own.
 */
class InterprocessConnection {
public:
  /** The flag is the reference's: callbacks here are always on the
   *  connection's own thread, whatever it says. The magic number must match
   *  at both ends. */
  explicit InterprocessConnection(bool callbacksOnMessageThread = false, uint32_t magicMessageHeaderNumber = 0xf2b49e2cu)
      : magic_(magicMessageHeaderNumber) {
    (void) callbacksOnMessageThread;
  }
  /** Disconnects without calling connectionLost(): by now the subclass is
   *  gone, which is why it must disconnect() in its own destructor. */
  virtual ~InterprocessConnection() { disconnect(-1, false); }
  ///@{
  /** Not copyable: the connection's thread holds a pointer to this
   *  object. */
  InterprocessConnection(const InterprocessConnection&) = delete;
  InterprocessConnection& operator=(const InterprocessConnection&) = delete;
  ///@}

  /** Connects, ending any connection this had. Not from inside one of this
   *  connection's callbacks -- a reconnect from connectionLost, say -- where
   *  it returns false: see restartableHere(). */
  bool connectToSocket(const std::string& hostName, int portNumber, int timeOutMillisecs) {
    if (!restartableHere()) return false;
    disconnect();
    auto socket = std::make_unique<StreamingSocket>();
    if (!socket->connect(hostName, portNumber, timeOutMillisecs)) return false;
    socket_ = std::move(socket);
    start();
    return true;
  }

  /** Opens a pipe another process created, retrying for up to timeoutMs
   *  while it does not exist yet. False from inside a callback, as
   *  connectToSocket. */
  bool connectToPipe(const std::string& pipeName, int timeoutMs) {
    if (!restartableHere()) return false;
    disconnect();
    auto pipe = std::make_unique<NamedPipe>();
    const auto start = std::chrono::steady_clock::now();
    while (!pipe->openExisting(pipeName)) {
      if (timeoutMs >= 0 && std::chrono::steady_clock::now() - start > std::chrono::milliseconds(timeoutMs)) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    pipe_ = std::move(pipe);
    this->start();
    return true;
  }

  /** Creates a pipe and waits (on the connection's thread) for a process to
   *  open it; connectionMade() says when one has. timeoutMs bounds each
   *  write, and a message that cannot be written whole within it ends the
   *  connection (see sendMessage). False from inside a callback, as
   *  connectToSocket. */
  bool createPipe(const std::string& pipeName, int timeoutMs, bool mustNotExist = false) {
    if (!restartableHere()) return false;
    disconnect();
    auto pipe = std::make_unique<NamedPipe>();
    if (!pipe->createNewPipe(pipeName, mustNotExist)) return false;
    pipe_ = std::move(pipe);
    if (timeoutMs > 0) writeTimeoutMs_ = timeoutMs;
    start();
    return true;
  }

  /** Ends the connection and waits for its thread. connectionLost() is called
   *  (on this thread) if the connection was made and notify is true. */
  void disconnect(int timeoutMs = -1, bool notify = true) {
    (void) timeoutMs; // the thread notices within one poll interval
    stop_ = true;
    // A pipe can be closed under its reader and writer -- both return at
    // once. A socket is not closed until its reader has stopped: a closed
    // descriptor's number can be reused before the reader looks again.
    if (pipe_) pipe_->close();
    {
      // So it is shut instead, and BEFORE the connection's thread is waited
      // for. A sender blocked on a peer that stopped reading holds
      // writeMutex_ for as long as the peer's buffer stays full -- for ever,
      // if it has hung -- and that sender may be the connection's own
      // thread, replying from messageReceived: joined first, it was waited
      // for for ever. Shut, its send fails at once and a read in progress
      // ends, and the number stays ours until the socket is released below.
      std::lock_guard<std::mutex> lock(transportMutex_);
      if (socket_ && socket_->getRawSocketHandle() != -1)
        netdetail::shutdownAndWake((netdetail::SocketHandle) socket_->getRawSocketHandle());
    }
    if (thread_.joinable()) {
      if (isCallbackThread()) {
        // From inside a callback: the thread ends when the callback returns,
        // and is joined by whoever disconnects or destroys this next.
        if (notify) notifyLost();
        return;
      }
      thread_.join();
    }
    if (notify) notifyLost();
    std::lock_guard<std::mutex> writing(writeMutex_); // no send is using them
    std::lock_guard<std::mutex> lock(transportMutex_);
    socket_.reset();
    pipe_.reset();
    connected_ = false;
  }

  /** True from just before connectionMade() until the connection ends. Any
   *  thread. */
  bool isConnected() const noexcept { return connected_.load(); }

  /** Whether the calling thread is this connection's own (inside a callback). */
  bool isCallbackThread() const { return threadId_.load() == std::this_thread::get_id(); }

  /** The peer's host for a socket connection; empty for a pipe. */
  std::string getConnectedHostName() const {
    std::lock_guard<std::mutex> lock(transportMutex_);
    return socket_ ? socket_->getHostName() : std::string();
  }

  /** Sends one message; false if not connected or the write failed. */
  bool sendMessage(const void* data, size_t size) {
    if (size > 0xFFFFFFFFu || size > maxMessageSize_) return false;
    const std::vector<uint8_t> frame = ipcdetail::encodeFrame(magic_, data, size);
    // Held for the whole write: disconnect() takes it before it releases the
    // pipe or socket, so neither goes away under a sender.
    std::lock_guard<std::mutex> writing(writeMutex_);
    StreamingSocket* socket;
    NamedPipe* pipe;
    {
      std::lock_guard<std::mutex> lock(transportMutex_);
      if (!connected_.load()) return false;
      socket = socket_.get();
      pipe = pipe_.get();
    }
    const int n = (int) frame.size();
    if (socket) return socket->write(frame.data(), n) == n;
    if (pipe) {
      if (pipe->write(frame.data(), n, writeTimeoutMs_) == n) return true;
      // Not the whole frame -- and not necessarily none of it: a write that
      // timed out after the peer had taken the start of it says nothing was
      // written (see NamedPipe::write). The peer then reads whatever is
      // written next as the rest of the frame: the next message, and the
      // next, each reported sent here, delivered there as one megabyte
      // nobody sent. Framing cannot be repaired, so the connection ends at
      // both ends: closed, the peer sees the pipe go, and this side's thread
      // reports the loss.
      connected_ = false;
      pipe->close();
      return false;
    }
    return false;
  }
  /** The same for a vector's bytes. */
  bool sendMessage(const std::vector<uint8_t>& message) { return sendMessage(message.data(), message.size()); }

  /** Messages longer than this end the connection. 64 MB unless changed. */
  void setMaximumMessageSize(size_t bytes) noexcept { maxMessageSize_ = bytes; }

  /** The pipe or socket underneath, or null. */
  StreamingSocket* getSocket() const noexcept { return socket_.get(); }
  /** The pipe, or null for a socket connection or none. Owned by the
   *  connection and released by disconnect(). */
  NamedPipe* getPipe() const noexcept { return pipe_.get(); }

  /** The other end is there, on the connection's thread. For a created
   *  pipe, when a client opens it. */
  virtual void connectionMade() = 0;
  /** The connection has ended, once, on the connection's thread -- or on
   *  the thread calling disconnect(). Only after connectionMade(). */
  virtual void connectionLost() = 0;
  /** One whole message, on the connection's thread. The reference is valid
   *  only during the call. */
  virtual void messageReceived(const std::vector<uint8_t>& message) = 0;

protected:
  /** Ends the connection as LOST, from any thread, without waiting for
   *  anything: the connection's thread notices within a poll interval and
   *  calls connectionLost() itself. What a watchdog calls -- it must not
   *  join the thread it is watching, which may be waiting on it. */
  void abandon() noexcept { abandoned_ = true; }

private:
  friend class InterprocessConnectionServer;

  void initialiseWithSocket(std::unique_ptr<StreamingSocket> socket) {
    disconnect();
    socket_ = std::move(socket);
    start();
  }

  /** False inside a callback: on this connection's own thread, or in a
   *  connectionLost that disconnect() is calling on its caller's. A client
   *  that reconnected in connectionLost crashed the host either way. On the
   *  connection's thread, disconnect() could not wait for the thread it was
   *  running on and left it joinable, and start() assigned a new thread over
   *  it: std::terminate. On the caller's, the reconnect started a new
   *  thread on a new socket, and the disconnect() that had called
   *  connectionLost then went on to release "its" socket -- the new one,
   *  under the new thread. (The thread is asked about only while unjoined:
   *  a joined thread's id may be reused.) */
  bool restartableHere() const {
    return !(thread_.joinable() && isCallbackThread()) && losingOn_.load() != std::this_thread::get_id();
  }

  void start() {
    stop_ = false;
    abandoned_ = false;
    lostNotified_ = false;
    madeNotified_ = false;
    thread_ = std::thread([this] {
      threadId_ = std::this_thread::get_id();
      run();
    });
  }

  void notifyLost() {
    connected_ = false;
    if (madeNotified_.load() && !lostNotified_.exchange(true)) {
      losingOn_ = std::this_thread::get_id();
      connectionLost();
      losingOn_ = std::thread::id();
    }
  }

  void run() {
    // A created pipe has no peer yet: wait for one, stoppably.
    if (pipe_) {
      while (!stop_.load() && !pipe_->waitForConnection(100)) {
        if (!pipe_->isOpen()) return;
      }
      if (stop_.load()) return;
    }
    connected_ = true;
    madeNotified_ = true;
    connectionMade();
    ipcdetail::FrameDecoder decoder(magic_, maxMessageSize_);
    std::vector<uint8_t> buffer(65536);
    bool lost = false;
    while (!stop_.load()) {
      if (abandoned_.load()) {
        lost = true;
        break;
      }
      int got;
      if (socket_) {
        const int ready = socket_->waitUntilReady(true, 100);
        if (ready == 0) continue;
        got = ready < 0 ? -1 : socket_->read(buffer.data(), (int) buffer.size(), false);
        if (got == 0) got = -1; // the peer closed
      } else {
        got = pipe_->readSome(buffer.data(), (int) buffer.size(), 100);
        if (got == 0) continue;
      }
      if (got < 0) {
        lost = true;
        break;
      }
      const bool ok = decoder.feed(buffer.data(), (size_t) got, [this](const std::vector<uint8_t>& message) {
        if (!stop_.load()) messageReceived(message);
      });
      if (!ok) {
        lost = true; // the framing is gone: so is the connection
        break;
      }
    }
    if (lost && !stop_.load()) notifyLost();
    connected_ = false;
  }

  uint32_t magic_;
  size_t maxMessageSize_ = 64u << 20;
  int writeTimeoutMs_ = 10000;
  mutable std::mutex transportMutex_;
  std::mutex writeMutex_;
  std::unique_ptr<StreamingSocket> socket_;
  std::unique_ptr<NamedPipe> pipe_;
  std::thread thread_;
  std::atomic<std::thread::id> threadId_{std::thread::id()};
  std::atomic<std::thread::id> losingOn_{std::thread::id()}; // the thread inside connectionLost
  std::atomic<bool> stop_{false};
  std::atomic<bool> abandoned_{false};
  std::atomic<bool> connected_{false};
  std::atomic<bool> madeNotified_{false};
  std::atomic<bool> lostNotified_{false};
};

/**
 * Listens on a TCP port and hands each connection that arrives to a new
 * InterprocessConnection from createConnectionObject(). The server does not
 * own those connections: the subclass keeps them (and deletes them). Call
 * stop() in the subclass's destructor.
 */
class InterprocessConnectionServer {
public:
  /** Not listening: beginWaitingForSocket() starts it. */
  InterprocessConnectionServer() = default;
  /** stop()s it. The subclass stops it in its own destructor too, since the
   *  thread calls its createConnectionObject(). */
  virtual ~InterprocessConnectionServer() { stop(); }
  ///@{
  /** Not copyable: the server's thread holds a pointer to this object. */
  InterprocessConnectionServer(const InterprocessConnectionServer&) = delete;
  InterprocessConnectionServer& operator=(const InterprocessConnectionServer&) = delete;
  ///@}

  /** Starts listening: on the loopback unless bindAddress says otherwise (see
   *  net.h); port 0 lets the system choose, and getBoundPort() says which.
   *  False from inside createConnectionObject, which cannot restart the
   *  thread it is running on. */
  bool beginWaitingForSocket(int portNumber, const std::string& bindAddress = std::string()) {
    if (onServerThread()) return false;
    stop();
    if (!socket_.createListener(portNumber, bindAddress)) return false;
    stop_ = false;
    threadId_ = std::thread::id(); // not the last thread's, until the new one says
    thread_ = std::thread([this] {
      threadId_ = std::this_thread::get_id();
      while (!stop_.load()) {
        std::unique_ptr<StreamingSocket> client = socket_.waitForNextConnection(100);
        if (!client) continue;
        if (InterprocessConnection* connection = createConnectionObject())
          connection->initialiseWithSocket(std::move(client));
      }
    });
    return true;
  }

  /** Stops listening and waits for the server's thread -- except from
   *  createConnectionObject, a server that takes one client and no more:
   *  there the listener closes at once, and the thread, which ends when
   *  createConnectionObject returns, is joined by the next stop() or the
   *  destructor. It joined itself, which throws, out of a thread: the
   *  process ended in std::terminate. */
  void stop() {
    stop_ = true;
    if (thread_.joinable() && !onServerThread()) thread_.join();
    socket_.close();
  }

  /** The port being listened on, the system's choice for port 0; -1 when
   *  not listening. */
  int getBoundPort() const { return socket_.getBoundPort(); }

protected:
  /** A new connection object for a client that has just connected, or null
   *  to refuse it. Called on the server's thread. */
  virtual InterprocessConnection* createConnectionObject() = 0;

private:
  /** Asked only while the thread is unjoined: a joined thread's id may be reused. */
  bool onServerThread() const { return thread_.joinable() && threadId_.load() == std::this_thread::get_id(); }

  StreamingSocket socket_;
  std::thread thread_;
  std::atomic<std::thread::id> threadId_{std::thread::id()};
  std::atomic<bool> stop_{false};
};

// ═══════════════════════════════════════════════════════════════════════════
// ChildProcessCoordinator / ChildProcessWorker
// ═══════════════════════════════════════════════════════════════════════════

namespace ipcdetail {

const uint32_t kCoordinatorMagic = 0x712a3e4au;
const uint8_t kUserMessage = 0, kPingMessage = 1, kKillMessage = 2;
const int kDefaultWorkerTimeoutMs = 8000;

/**
 * The connection both halves use: a type byte in front of every message,
 * pings every quarter of the timeout, and a watchdog that drops the
 * connection when nothing -- not even a ping -- has arrived for the timeout.
 */
class KeepAliveConnection final : public InterprocessConnection {
public:
  std::function<void()> onMade, onLost, onKill;
  std::function<void(const std::vector<uint8_t>&)> onMessage;

  KeepAliveConnection() : InterprocessConnection(false, kCoordinatorMagic) {}
  ~KeepAliveConnection() override {
    stopPinging();
    disconnect(-1, false);
  }

  void startPinging(int timeoutMs) {
    stopPinging();
    timeoutMs_ = timeoutMs > 0 ? timeoutMs : kDefaultWorkerTimeoutMs;
    heard();
    {
      std::lock_guard<std::mutex> lock(pingMutex_);
      pingStop_ = false;
    }
    pinger_ = std::thread([this] {
      pingerId_ = std::this_thread::get_id();
      std::unique_lock<std::mutex> lock(pingMutex_);
      for (;;) {
        pingWake_.wait_for(lock, std::chrono::milliseconds((std::max)(10, timeoutMs_ / 4)), [this] { return pingStop_; });
        if (pingStop_) return;
        lock.unlock();
        const uint8_t ping = kPingMessage;
        sendMessage(&ping, 1);
        const auto silent = std::chrono::steady_clock::now() - lastHeard();
        if (isConnected() && silent > std::chrono::milliseconds(timeoutMs_)) {
          // It has hung. The connection's own thread reports the loss; this
          // one must not wait for that thread, which may be waiting for it.
          abandon();
          return;
        }
        lock.lock();
      }
    });
  }

  /** Stops the pings and, unless called from the pinging thread itself
   *  (whoever destroys this joins it later), waits for that thread. */
  void stopPinging() {
    {
      std::lock_guard<std::mutex> lock(pingMutex_);
      pingStop_ = true;
    }
    pingWake_.notify_all();
    if (pinger_.joinable() && pingerId_.load() != std::this_thread::get_id()) pinger_.join();
  }

  /** Whether the caller is one of this connection's threads -- inside a
   *  callback, where it must not be destroyed. */
  bool isOwnThread() const { return isCallbackThread() || pingerId_.load() == std::this_thread::get_id(); }

  bool send(uint8_t type, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> m;
    m.reserve(payload.size() + 1);
    m.push_back(type);
    m.insert(m.end(), payload.begin(), payload.end());
    return sendMessage(m);
  }

  void connectionMade() override {
    heard();
    if (onMade) onMade();
  }
  void connectionLost() override {
    if (onLost) onLost();
  }
  void messageReceived(const std::vector<uint8_t>& message) override {
    heard();
    if (message.empty()) return;
    if (message[0] == kUserMessage && onMessage) onMessage(std::vector<uint8_t>(message.begin() + 1, message.end()));
    else if (message[0] == kKillMessage && onKill) onKill();
  }

private:
  void heard() { lastHeard_.store(std::chrono::steady_clock::now().time_since_epoch().count()); }
  std::chrono::steady_clock::time_point lastHeard() const {
    return std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(lastHeard_.load()));
  }

  std::atomic<std::chrono::steady_clock::rep> lastHeard_{0};
  int timeoutMs_ = kDefaultWorkerTimeoutMs;
  std::thread pinger_;
  std::atomic<std::thread::id> pingerId_{std::thread::id()};
  std::mutex pingMutex_;
  std::condition_variable pingWake_;
  bool pingStop_ = false;
};

} // namespace ipcdetail

/**
 * Runs a worker process and talks to it.
 *
 *   struct Scanner : ChildProcessCoordinator {
 *     void handleMessageFromWorker(const std::vector<uint8_t>& m) override {...}
 *     void handleConnectionLost() override { ...it crashed or hung... }
 *     ~Scanner() override { killWorkerProcess(); }
 *   };
 *   scanner.launchWorkerProcess(currentModulePath() + "-scanner", "sonore-scan");
 *
 * The worker is any executable that calls
 * ChildProcessWorker::initialiseFromCommandLine with the same unique id --
 * often the same program, started in a "worker" mode by that argument.
 * handleConnectionLost is for the UNEXPECTED: the worker exiting, crashing,
 * or going silent past the timeout. killWorkerProcess does not call it.
 */
class ChildProcessCoordinator {
public:
  /** No worker yet: launchWorkerProcess() starts one. */
  ChildProcessCoordinator() = default;
  /** Kills the worker (see killWorkerProcess). A subclass calls
   *  killWorkerProcess() in its own destructor, before its handlers go. */
  virtual ~ChildProcessCoordinator() {
    killWorkerProcess();
    std::lock_guard<std::mutex> lock(mutex_);
    retired_.clear();
  }
  ///@{
  /** Not copyable: the worker and its connection have one owner. */
  ChildProcessCoordinator(const ChildProcessCoordinator&) = delete;
  ChildProcessCoordinator& operator=(const ChildProcessCoordinator&) = delete;
  ///@}

  /**
   * Starts executableToLaunch with "<commandLineUniqueID>:<pipe>" (and any
   * extra arguments) and waits up to timeoutMs (0: eight seconds) for it to
   * connect. False -- and no process left running -- if it does not.
   * streamFlags as ChildProcess::start; 0 sends its output nowhere.
   */
  bool launchWorkerProcess(const std::string& executableToLaunch, const std::string& commandLineUniqueID,
                           int timeoutMs = 0, int streamFlags = 0,
                           const std::vector<std::string>& extraArguments = std::vector<std::string>()) {
    killWorkerProcess();
    {
      // Connections retired from inside their own callbacks have finished by
      // now -- unless this is still one of their threads.
      std::lock_guard<std::mutex> lock(mutex_);
      retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
                                    [](const std::unique_ptr<ipcdetail::KeepAliveConnection>& c) {
                                      return !c->isOwnThread();
                                    }),
                     retired_.end());
    }
    const int timeout = timeoutMs > 0 ? timeoutMs : ipcdetail::kDefaultWorkerTimeoutMs;
    const std::string pipeName = "sonore-ipc-" + uuidToString(makeUuid());
    auto connection = std::make_unique<ipcdetail::KeepAliveConnection>();
    std::mutex madeMutex;
    std::condition_variable madeSignal;
    bool made = false;
    connection->onMade = [&] {
      std::lock_guard<std::mutex> lock(madeMutex);
      made = true;
      madeSignal.notify_all();
    };
    connection->onMessage = [this](const std::vector<uint8_t>& m) { handleMessageFromWorker(m); };
    connection->onLost = [this] {
      if (!killing_.load()) handleConnectionLost();
    };
    if (!connection->createPipe(pipeName, timeout, true)) return false;

    std::vector<std::string> arguments{executableToLaunch, commandLineUniqueID + ":" + pipeName};
    arguments.insert(arguments.end(), extraArguments.begin(), extraArguments.end());
    auto child = std::make_unique<ChildProcess>();
    if (!child->start(arguments, streamFlags)) return false;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
    bool connected = false;
    {
      std::unique_lock<std::mutex> lock(madeMutex);
      while (!made && std::chrono::steady_clock::now() < deadline) {
        madeSignal.wait_for(lock, std::chrono::milliseconds(50));
        if (!made && !child->isRunning()) break; // it exited without connecting
      }
      connected = made;
    }
    if (!connected) {
      killing_ = true;
      connection.reset();
      child->kill();
      killing_ = false;
      return false;
    }
    connection->onMade = nullptr; // the locals it captured are going
    connection->startPinging(timeout);
    std::lock_guard<std::mutex> lock(mutex_);
    connection_ = std::move(connection);
    child_ = std::move(child);
    return true;
  }

  /** Tells the worker to quit, then makes sure: a worker that has not gone
   *  within a second is killed. Does not call handleConnectionLost. */
  void killWorkerProcess() {
    std::unique_ptr<ipcdetail::KeepAliveConnection> connection;
    std::unique_ptr<ChildProcess> child;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      connection = std::move(connection_);
      child = std::move(child_);
    }
    killing_ = true;
    if (connection) {
      connection->send(ipcdetail::kKillMessage, std::vector<uint8_t>());
      connection->stopPinging();
      connection->disconnect(-1, false);
      if (connection->isOwnThread()) {
        // Called from handleConnectionLost or a message handler: this very
        // thread is the connection's, and is still inside it. It is kept,
        // finished, and destroyed by the next launch or the destructor.
        std::lock_guard<std::mutex> lock(mutex_);
        retired_.push_back(std::move(connection));
      }
    }
    if (child && !child->waitForProcessToFinish(1000)) child->kill();
    killing_ = false;
  }

  /** Sends one message, delivered whole to the worker's
   *  handleMessageFromCoordinator(). False when no worker is connected or
   *  the write failed. Any thread. */
  bool sendMessageToWorker(const std::vector<uint8_t>& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    return connection_ && connection_->send(ipcdetail::kUserMessage, message);
  }

  /** The worker's process: its output (if streamFlags captured it) and, once
   *  it has gone, its exit code. Null when none is running. */
  ChildProcess* getWorkerProcess() {
    std::lock_guard<std::mutex> lock(mutex_);
    return child_.get();
  }

  /** On the connection's thread. */
  virtual void handleMessageFromWorker(const std::vector<uint8_t>& message) { (void) message; }
  /** On the connection's (or the watchdog's) thread: the worker exited,
   *  crashed, or stopped answering. */
  virtual void handleConnectionLost() {}

private:
  std::mutex mutex_;
  std::unique_ptr<ipcdetail::KeepAliveConnection> connection_;
  std::vector<std::unique_ptr<ipcdetail::KeepAliveConnection>> retired_;
  std::unique_ptr<ChildProcess> child_;
  std::atomic<bool> killing_{false};
};

/**
 * The worker half: in the worker process's main(),
 *
 *   MyWorker worker;
 *   if (worker.initialiseFromCommandLine(commandLine, "sonore-scan")) {
 *     ...run until handleConnectionLost says the coordinator has gone...
 *   }
 *
 * handleConnectionLost is called when the coordinator dies, hangs, or calls
 * killWorkerProcess -- the worker should then exit.
 */
class ChildProcessWorker {
public:
  /** Not connected: initialiseFromCommandLine() connects. */
  ChildProcessWorker() = default;
  /** Disconnects without calling handleConnectionLost(). */
  virtual ~ChildProcessWorker() {
    if (connection_) {
      connection_->stopPinging();
      connection_->disconnect(-1, false);
    }
  }
  ///@{
  /** Not copyable: the connection's thread calls this object's handlers. */
  ChildProcessWorker(const ChildProcessWorker&) = delete;
  ChildProcessWorker& operator=(const ChildProcessWorker&) = delete;
  ///@}

  /** Finds "<commandLineUniqueID>:<pipe>" in the command line and connects.
   *  False when it is not there (this process is not a worker) or the
   *  coordinator cannot be reached. */
  bool initialiseFromCommandLine(const std::string& commandLine, const std::string& commandLineUniqueID,
                                 int timeoutMs = 0) {
    return initialiseFromArguments(processdetail::splitCommandLine(commandLine), commandLineUniqueID, timeoutMs);
  }
  /** The same, given the arguments already split (main's argv, say). The
   *  last argument with the prefix wins; `timeoutMs` (0: eight seconds)
   *  bounds the connection and sets the silence that counts as a hang. */
  bool initialiseFromArguments(const std::vector<std::string>& arguments, const std::string& commandLineUniqueID,
                               int timeoutMs = 0) {
    const std::string prefix = commandLineUniqueID + ":";
    std::string pipeName;
    for (const std::string& a : arguments)
      if (a.size() > prefix.size() && a.compare(0, prefix.size(), prefix) == 0) pipeName = a.substr(prefix.size());
    if (pipeName.empty()) return false;
    const int timeout = timeoutMs > 0 ? timeoutMs : ipcdetail::kDefaultWorkerTimeoutMs;
    connection_ = std::make_unique<ipcdetail::KeepAliveConnection>();
    connection_->onMade = [this] { handleConnectionMade(); };
    connection_->onMessage = [this](const std::vector<uint8_t>& m) { handleMessageFromCoordinator(m); };
    connection_->onLost = [this] { lostOnce(); };
    // The coordinator closes its end right after; the loss is reported now.
    connection_->onKill = [this] { lostOnce(); };
    if (!connection_->connectToPipe(pipeName, timeout)) {
      connection_.reset();
      return false;
    }
    connection_->startPinging(timeout);
    return true;
  }

  /** Sends one message, delivered whole to the coordinator's
   *  handleMessageFromWorker(). False when not connected or the write
   *  failed. */
  bool sendMessageToCoordinator(const std::vector<uint8_t>& message) {
    return connection_ && connection_->send(ipcdetail::kUserMessage, message);
  }

  /** One message from sendMessageToWorker(), on the connection's thread. */
  virtual void handleMessageFromCoordinator(const std::vector<uint8_t>& message) { (void) message; }
  /** Connected to the coordinator, on the connection's thread. */
  virtual void handleConnectionMade() {}
  /** Called once when the coordinator has gone, hung, or asked this worker
   *  to quit -- the worker should exit. On the connection's (or the
   *  watchdog's) thread. */
  virtual void handleConnectionLost() {}

private:
  void lostOnce() {
    if (!lost_.exchange(true)) handleConnectionLost();
  }

  std::unique_ptr<ipcdetail::KeepAliveConnection> connection_;
  std::atomic<bool> lost_{false};
};

// ═══════════════════════════════════════════════════════════════════════════
// NetworkServiceDiscovery
// ═══════════════════════════════════════════════════════════════════════════

namespace ipcdetail {

/** An advertisement as it travels:
 *
 *    SONORE-NSD 1
 *    type=<service type>
 *    id=<instance id>
 *    port=<connection port>
 *    name=<description>
 *
 *  Lines ending in "\n"; any control character inside a value becomes a
 *  space, and the parser refuses one -- so what a listener accepts is
 *  exactly what an advertiser could have sent (the fuzzer found an id with a
 *  carriage return in it that no advertiser would ever announce again). */
inline std::string encodeAdvertisement(const std::string& type, const std::string& instanceID, int port,
                                       const std::string& description) {
  auto clean = [](std::string s) {
    for (char& c : s)
      if ((unsigned char) c < 0x20 || c == 0x7F) c = ' ';
    if (s.size() > 256) s.resize(256);
    return s;
  };
  return "SONORE-NSD 1\ntype=" + clean(type) + "\nid=" + clean(instanceID) + "\nport=" + std::to_string(port) +
         "\nname=" + clean(description) + "\n";
}

struct Advertisement {
  std::string type, instanceID, description;
  int port = 0;
};

/** Untrusted bytes from the network to an advertisement. False for anything
 *  that is not exactly one: the header, the type, an id, a port in range. */
inline bool parseAdvertisement(const uint8_t* data, size_t size, Advertisement& out) {
  if (!data || size < 13 || size > 1400) return false;
  const std::string text((const char*) data, size);
  if (text.compare(0, 13, "SONORE-NSD 1\n") != 0) return false;
  Advertisement a;
  bool haveType = false, haveId = false, havePort = false;
  size_t start = 13;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    const std::string line = text.substr(start, end - start);
    start = end + 1;
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
    if (value.size() > 256) return false;
    for (char c : line)
      if ((unsigned char) c < 0x20 || c == 0x7F) return false;
    if (key == "type") {
      a.type = value;
      haveType = !value.empty();
    } else if (key == "id") {
      a.instanceID = value;
      haveId = !value.empty();
    } else if (key == "name") {
      a.description = value;
    } else if (key == "port") {
      if (value.empty() || value.size() > 5) return false;
      int p = 0;
      for (char c : value) {
        if (c < '0' || c > '9') return false;
        p = p * 10 + (c - '0');
      }
      if (p <= 0 || p > 65535) return false;
      a.port = p;
      havePort = true;
    }
  }
  if (!haveType || !haveId || !havePort) return false;
  out = a;
  return true;
}

} // namespace ipcdetail

/** Finding a service on the local network without knowing its address: an
 *  Advertiser broadcasts, an AvailableServiceList listens. */
struct NetworkServiceDiscovery {
  /** One service instance, as last heard. */
  struct Service {
    /** The advertiser's unique id: tells two instances on one machine
     *  apart. */
    std::string instanceID;
    /** The advertiser's description, for a person; untrusted text from the
     *  network. */
    std::string description;
    /** Where the advertisement came from: connect here, on `port`. */
    IPAddress address;
    /** The port the service accepts connections on, 1 to 65535. */
    int port = 0;
    /** When its last advertisement arrived. */
    std::chrono::steady_clock::time_point lastSeen;
  };

  /**
   * Broadcasts "service <type> is here, on <port>" every
   * minTimeBetweenBroadcastsMs on its own thread, until destroyed. The
   * broadcast address is 255.255.255.255 unless another is given -- a
   * subnet's own, or for a test, the loopback.
   */
  class Advertiser {
  public:
    /** Starts broadcasting at once, to `broadcastPort` at `broadcastAddress`,
     *  announcing `connectionPort`. The interval is at least 50 ms. Control
     *  characters in the type and description become spaces, and each is
     *  cut to 256 bytes. */
    Advertiser(const std::string& serviceTypeUID, const std::string& serviceDescription, int broadcastPort,
               int connectionPort, int minTimeBetweenBroadcastsMs = 1500,
               const IPAddress& broadcastAddress = IPAddress::broadcast())
        : socket_(true), instanceID_(uuidToString(makeUuid())), port_(broadcastPort), target_(broadcastAddress) {
      message_ = ipcdetail::encodeAdvertisement(serviceTypeUID, instanceID_, connectionPort, serviceDescription);
      const int interval = (std::max)(50, minTimeBetweenBroadcastsMs);
      thread_ = std::thread([this, interval] {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stop_) {
          lock.unlock();
          socket_.write(target_, port_, message_.data(), (int) message_.size());
          lock.lock();
          wake_.wait_for(lock, std::chrono::milliseconds(interval), [this] { return stop_; });
        }
      });
    }
    /** Stops broadcasting and waits for the thread. Listeners drop this
     *  instance when its advertisements stop arriving. */
    ~Advertiser() {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
      }
      wake_.notify_all();
      if (thread_.joinable()) thread_.join();
    }
    ///@{
    /** Not copyable: the thread holds a pointer to this object. */
    Advertiser(const Advertiser&) = delete;
    Advertiser& operator=(const Advertiser&) = delete;
    ///@}

    /** This instance's id, as listeners will see it. */
    const std::string& getInstanceID() const noexcept { return instanceID_; }

  private:
    DatagramSocket socket_;
    std::string instanceID_;
    std::string message_;
    int port_;
    IPAddress target_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stop_ = false;
  };

  /**
   * The instances of one service type currently advertising on broadcastPort.
   * Listens on every interface (bindAddress "0.0.0.0" -- broadcasts do not
   * arrive at a loopback-bound socket), sharing the port with any other
   * listener on the machine. An instance not heard from for serviceTimeoutMs
   * is dropped. The change callback runs on the list's own thread.
   */
  class AvailableServiceList {
  public:
    /** Starts listening at once; isListening() says whether the port could
     *  be bound. Only advertisements of `serviceTypeUID` are kept. */
    AvailableServiceList(const std::string& serviceTypeUID, int broadcastPort,
                         const std::string& bindAddress = "0.0.0.0", int serviceTimeoutMs = 5000)
        : type_(serviceTypeUID), timeoutMs_(serviceTimeoutMs > 0 ? serviceTimeoutMs : 5000) {
      socket_.setEnablePortReuse(true);
      listening_ = socket_.bindToPort(broadcastPort, bindAddress);
      if (listening_) thread_ = std::thread([this] { run(); });
    }
    /** Stops listening and waits for the thread, which may be inside the
     *  change callback. */
    ~AvailableServiceList() {
      stop_ = true;
      if (thread_.joinable()) thread_.join();
    }
    ///@{
    /** Not copyable: the thread holds a pointer to this object. */
    AvailableServiceList(const AvailableServiceList&) = delete;
    AvailableServiceList& operator=(const AvailableServiceList&) = delete;
    ///@}

    /** False if the port could not be bound. */
    bool isListening() const noexcept { return listening_; }

    /** A copy of the services heard within the timeout. Any thread. */
    std::vector<Service> getServices() const {
      std::lock_guard<std::mutex> lock(mutex_);
      return services_;
    }

    /** Called on the list's thread whenever a service appears, changes or
     *  goes. Safe to set at any time. */
    void setChangeCallback(std::function<void()> callback) {
      std::lock_guard<std::mutex> lock(mutex_);
      onChange_ = std::move(callback);
    }

  private:
    void run() {
      std::vector<uint8_t> buffer(2048);
      while (!stop_.load()) {
        bool changed = false;
        if (socket_.waitUntilReady(true, 100) == 1) {
          IPAddress sender;
          int senderPort = 0;
          const int got = socket_.read(buffer.data(), (int) buffer.size(), false, sender, senderPort);
          ipcdetail::Advertisement ad;
          if (got > 0 && ipcdetail::parseAdvertisement(buffer.data(), (size_t) got, ad) && ad.type == type_)
            changed = record(ad, IPAddress::convertIPv4MappedAddressToIPv4(sender));
        }
        changed = expire() || changed;
        if (changed) {
          std::function<void()> callback;
          {
            std::lock_guard<std::mutex> lock(mutex_);
            callback = onChange_;
          }
          if (callback) callback();
        }
      }
    }

    bool record(const ipcdetail::Advertisement& ad, const IPAddress& from) {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto now = std::chrono::steady_clock::now();
      for (Service& s : services_) {
        if (s.instanceID != ad.instanceID) continue;
        const bool changed = s.address != from || s.port != ad.port || s.description != ad.description;
        s.address = from;
        s.port = ad.port;
        s.description = ad.description;
        s.lastSeen = now;
        return changed;
      }
      if (services_.size() >= 1024) return false; // a flood is not a thousand services
      Service s;
      s.instanceID = ad.instanceID;
      s.description = ad.description;
      s.address = from;
      s.port = ad.port;
      s.lastSeen = now;
      services_.push_back(s);
      return true;
    }

    bool expire() {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto oldest = std::chrono::steady_clock::now() - std::chrono::milliseconds(timeoutMs_);
      const size_t before = services_.size();
      services_.erase(std::remove_if(services_.begin(), services_.end(),
                                     [&](const Service& s) { return s.lastSeen < oldest; }),
                      services_.end());
      return services_.size() != before;
    }

    std::string type_;
    int timeoutMs_;
    DatagramSocket socket_;
    bool listening_ = false;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    mutable std::mutex mutex_;
    std::vector<Service> services_;
    std::function<void()> onChange_;
  };
};

} // namespace sonore
