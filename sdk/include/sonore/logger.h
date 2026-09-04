// SPDX-License-Identifier: Apache-2.0
//
// A log a user can send you.
//
// ── Why a plugin needs one more than an application does ───────────────────
//
// An application that misbehaves can be run from a terminal. A plugin cannot:
// it is loaded into somebody else's process, on somebody else's machine, and
// the only channel back is the user. "It crashes sometimes when I load a
// preset" is the entire bug report you get, and without a log it is also the
// entire evidence.
//
// So this exists to be READ BY SOMEBODY ELSE: appended to a file the user can
// find, capped so it cannot fill a disk, and readable back so a plugin can put
// a "copy diagnostics" button in its editor.
//
// ── The audio thread, which is the whole design problem ────────────────────
//
// The interesting failures are in process(). That is exactly where a logger
// must not be used: opening a file, formatting a string and taking a mutex are
// each enough to blow a buffer deadline, and the dropout would be caused by the
// diagnostic rather than by the bug.
//
// The conventional logger has no answer to that -- a virtual call to a
// subclass that writes a file, and calling it from an audio callback is simply a
// mistake. Which means the one place you most want a trace is the one place you
// cannot have one.
//
// So there are two doors. write() is for any ordinary thread and does the
// obvious thing. writeFromAudioThread() copies at most kMaxLine bytes into a
// fixed ring and returns: no allocation, no lock, no syscall, nothing that can
// block. drain() moves whatever accumulated to the file, and is called from the
// editor's clock alongside everything else that crosses that boundary.
//
// A message that arrives while the ring is full is DROPPED and counted. Dropped
// with a number attached is a fact; dropped silently is a lie, and blocking
// until there is room is the dropout this whole arrangement exists to avoid.
#pragma once

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "files.h"
#include "time.h"
#include "user_settings.h"

namespace sonore {

class Logger {
public:
  /** The longest one message from the audio thread may be. Fixed, because the
   *  ring has to be fixed, because allocating is the thing being avoided. */
  static constexpr size_t kMaxLine = 120;
  /** How many audio-thread messages can be waiting. 256 is a lot of lines for
   *  33 milliseconds; anything that produces more is logging per SAMPLE and has
   *  a different problem. */
  static constexpr size_t kRingLines = 256;

  /** One per process. A plugin loaded twelve times shares it, which is what
   *  makes the file readable -- twelve interleaved logs in one file with the
   *  instance named on each line beats twelve files nobody can correlate. */
  static Logger& get() {
    static Logger instance;
    return instance;
  }

  /**
   * Where to write. Empty stops writing to a file but keeps the recent buffer,
   * which is what a plugin with no permission to write anywhere still wants for
   * its diagnostics button.
   *
   * The directory is created if it is missing -- a log that silently did
   * nothing because a folder was absent is the failure this class is for.
   */
  void setFile(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    path_ = path;
    if (path_.empty()) return;
    const std::string parent = parentPath(path_);
    if (!parent.empty() && !isDirectory(parent)) ensureDirectory(parent);
  }

  const std::string& file() const { return path_; }

  /** Past this many bytes the file is started again. A log that grows without
   *  limit is one that eventually fills a user's disk, and they will remember
   *  which plugin did it. */
  void setMaxBytes(size_t bytes) { maxBytes_ = bytes; }

  /** A prefix on every line -- the plugin's name and instance. Twelve
   *  instances share one file and the lines are useless without it. */
  void setPrefix(std::string prefix) {
    std::lock_guard<std::mutex> lock(mutex_);
    prefix_ = std::move(prefix);
  }

  /**
   * From ANY thread except the audio one.
   *
   * Takes a lock and may touch a file, either of which is fine on a UI or
   * worker thread and fatal in process().
   */
  void write(const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    appendLocked(stamp() + prefix_ + message);
  }

  /**
   * From the AUDIO thread.
   *
   * Copies at most kMaxLine bytes into a fixed ring and returns. No
   * allocation, no lock, no syscall. Longer messages are TRUNCATED rather than
   * split, because a message spanning two ring slots could be interleaved with
   * another thread's and neither would be readable.
   *
   * Returns false when the ring is full, so a caller that cares can count its
   * own losses -- and the drop is counted here regardless.
   */
  bool writeFromAudioThread(const char* message) {
    if (!message) return false;
    const size_t head = ringHead_.load(std::memory_order_relaxed);
    const size_t tail = ringTail_.load(std::memory_order_acquire);
    if (head - tail >= kRingLines) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    char* slot = &ring_[(head % kRingLines) * kMaxLine];
    size_t i = 0;
    for (; i + 1 < kMaxLine && message[i] != '\0'; ++i) slot[i] = message[i];
    slot[i] = '\0';
    // RELEASE, so the bytes above are visible before the slot is claimed. The
    // reader acquires the same variable, and without the pair a drainer can
    // legitimately see the new index and the old characters.
    ringHead_.store(head + 1, std::memory_order_release);
    return true;
  }

  /**
   * Move whatever the audio thread queued into the file.
   *
   * Called from the editor's 33 ms clock, alongside everything else that
   * crosses that boundary. Returns how many lines it moved.
   */
  int drain() {
    std::lock_guard<std::mutex> lock(mutex_);
    int moved = 0;
    for (;;) {
      const size_t tail = ringTail_.load(std::memory_order_relaxed);
      const size_t head = ringHead_.load(std::memory_order_acquire);
      if (tail == head) break;
      const char* slot = &ring_[(tail % kRingLines) * kMaxLine];
      appendLocked(stamp() + prefix_ + "[audio] " + std::string(slot));
      ringTail_.store(tail + 1, std::memory_order_release);
      ++moved;
    }
    const long lost = dropped_.exchange(0, std::memory_order_relaxed);
    if (lost > 0) {
      // Counted and SAID. A drop with a number attached is a fact; a silent
      // one is a lie about a log that is missing something.
      appendLocked(stamp() + prefix_ + "[audio] " + std::to_string(lost) +
                   " message(s) dropped -- the ring filled faster than it drained");
      ++moved;
    }
    return moved;
  }

  /** How many audio-thread messages have been thrown away since the last
   *  drain. Exposed so a caller can notice it is logging too much. */
  long droppedCount() const { return dropped_.load(std::memory_order_relaxed); }

  /**
   * The last few lines, for a "copy diagnostics" button.
   *
   * Kept in memory as well as written, because the most useful moment to read
   * a log is while the plugin is still open and the user is still on the
   * phone -- and because a plugin with no write permission still has this.
   */
  std::string recent(int lines = 200) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t count = recent_.size();
    const size_t take = (lines <= 0 || (size_t) lines > count) ? count : (size_t) lines;
    std::string out;
    for (size_t i = count - take; i < count; ++i) {
      out += recent_[i];
      out += "\n";
    }
    return out;
  }

  void clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    recent_.clear();
  }

  static constexpr size_t kRecentLines = 500;

private:
  Logger() : ring_(kRingLines * kMaxLine, '\0') {}

  /** UTC and ISO, so lines from two machines in two time zones sort into the
   *  order they happened. A local timestamp in a log that will be emailed is a
   *  puzzle for whoever reads it. */
  static std::string stamp() {
    const DateTime now = dateTimeFromUnix(currentUnixTime(), /*utc=*/true);
    return formatDateTime(now, /*withSeconds=*/true) + "Z ";
  }

  void appendLocked(const std::string& line) {
    recent_.push_back(line);
    if (recent_.size() > kRecentLines)
      recent_.erase(recent_.begin(), recent_.begin() + (recent_.size() - kRecentLines));
    if (path_.empty()) return;

    // Opened and closed per write, deliberately. A plugin can be unloaded at
    // any moment by a host that does not tell it, and a file handle held open
    // across that is a handle nothing closes -- on Windows that also stops the
    // user deleting the log they were told to send.
    std::FILE* f = std::fopen(path_.c_str(), "ab");
    if (!f) return;
    std::fwrite(line.data(), 1, line.size(), f);
    std::fputc('\n', f);
    const long size = std::ftell(f);
    std::fclose(f);

    if (maxBytes_ > 0 && size > 0 && (size_t) size > maxBytes_) rotateLocked();
  }

  /**
   * Start again, keeping the most recent lines rather than nothing.
   *
   * A log that truncated to empty at the moment it got interesting would be
   * worse than one that grew. But only as many lines as fit in HALF the cap --
   * the first version wrote back everything it had in memory, which is up to
   * five hundred lines, so a small cap could never be honoured at all and the
   * file came out sixteen times the limit it was given. Half, so there is room
   * to write again before the next rotation, rather than rotating on every
   * line from then on.
   *
   * Counted backwards from the newest, because the newest lines are the ones
   * that describe whatever just went wrong.
   */
  void rotateLocked() {
    const size_t budget = maxBytes_ / 2;
    size_t used = 0;
    size_t from = recent_.size();
    while (from > 0) {
      const size_t cost = recent_[from - 1].size() + 1;
      if (used + cost > budget) break;
      used += cost;
      --from;
    }

    std::FILE* f = std::fopen(path_.c_str(), "wb");
    if (!f) return;
    const std::string header =
        stamp() + "-- log restarted, " + std::to_string(from) + " older line(s) discarded --\n";
    std::fwrite(header.data(), 1, header.size(), f);
    for (size_t i = from; i < recent_.size(); ++i) {
      std::fwrite(recent_[i].data(), 1, recent_[i].size(), f);
      std::fputc('\n', f);
    }
    std::fclose(f);
  }

  mutable std::mutex mutex_;
  std::string path_;
  std::string prefix_;
  size_t maxBytes_ = 4u * 1024u * 1024u;
  std::vector<std::string> recent_;

  /** The audio thread's ring. A flat char buffer rather than strings, because
   *  a std::string in here would allocate on the one thread that must not. */
  std::vector<char> ring_;
  std::atomic<size_t> ringHead_{0};
  std::atomic<size_t> ringTail_{0};
  std::atomic<long> dropped_{0};
};

/** Shorthand, so a call site reads as one thing. */
inline void logMessage(const std::string& message) { Logger::get().write(message); }

} // namespace sonore
