// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: recording audio to disk from a plugin, without stalling it.
//
// A threaded writer, and the reason it is a separate thing from "write a
// file" is the audio callback. Opening a file,
// growing a buffer and calling fwrite are all things that can block for
// milliseconds; doing any of them where audio is produced is a dropout. And
// the obvious alternative -- keep every sample in memory and write at the end
// -- is what the standalone's --capture used to do, which works for a
// twenty-second render and turns into gigabytes for a take.
//
// So: the audio thread pushes into a lock-free ring, a background thread
// drains it to a WAV file, and neither ever waits for the other. When the ring
// overflows the frames are DROPPED and counted, never overwritten -- a gap in
// a recording is obvious and recoverable, where a splice of two different
// moments sounds like the plugin corrupted the take.
#pragma once

#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "path_io.h"

#include "audio_ring.h" // the same SPSC ring a capture device fills

namespace sonore {

/**
 * A WAV file being written while audio is playing.
 *
 * start() and stop() are MAIN THREAD. write() is the only thing the audio
 * thread touches, and it allocates nothing, locks nothing and never blocks.
 * The audio thread need not stop calling it: stop() waits out a write() in
 * progress, so a take ends and the next one starts while audio runs.
 *
 * 32-bit float, matching writeWav(): a recording is usually the thing a user
 * wants to edit somewhere else, and quantising on the way out is a decision
 * best left to whatever they open it in.
 */
class WavRecorder {
public:
  /** stop(): the take in progress is finished and its header written. */
  ~WavRecorder() { stop(); }

  /** Not recording: write() does nothing until start(). */
  WavRecorder() = default;
  WavRecorder(const WavRecorder&) = delete;            ///< Not copyable: it owns a file and a thread.
  WavRecorder& operator=(const WavRecorder&) = delete; ///< Not copyable: it owns a file and a thread.

  /**
   * Open the file and start the writer thread.
   *
   * `bufferFrames` is how far behind the disk is allowed to fall. The default
   * is about a second and a half at 48 kHz, which rides out the pauses a
   * spinning disk or a virus scanner produces; a smaller ring saves memory
   * that nobody was short of and buys a dropped take.
   */
  bool start(const char* path, double sampleRate, int channels,
             size_t bufferFrames = 65536) {
    stop();
    if (!path || !*path || channels < 1 || !(sampleRate > 0.0)) return false;

    file_ = ::sonore::pathio::open(path, "wb");
    if (!file_) return false;
    path_ = path;
    channels_ = channels;
    sampleRate_ = (uint32_t) (sampleRate + 0.5);
    frames_.store(0, std::memory_order_relaxed);
    failed_.store(false, std::memory_order_relaxed);
    lost_.store(0, std::memory_order_relaxed);

    // A header whose lengths say "as long as a WAV can be", patched on the
    // way out. A take interrupted by a crash, a full disk or a power cut
    // never gets the patch, and it said zero: every reader opened it as an
    // empty file in front of the audio it had recorded. Claiming the most
    // instead is how a streaming writer leaves a length open, and a reader
    // -- this SDK's, ffmpeg, libsndfile -- reads what is actually there.
    if (!writeHeader(kOpenLength)) {
      std::fclose(file_);
      file_ = nullptr;
      return false;
    }

    ring_.reset(bufferFrames, channels);
    scratch_.assign((size_t) 4096 * (size_t) channels, 0.0f);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { drain(); });
    return true;
  }

  /**
   * [audio-thread] Hand over one block. Returns false if it did not all fit,
   * or if the disk has refused a write -- full, unplugged, gone -- after
   * which nothing more reaches the file (see flushRing()).
   *
   * The false is worth acting on: it means the disk is not keeping up and the
   * recording has a hole in it. A plugin that lights something when this
   * happens tells the user while they can still do the take again.
   */
  bool write(const float* interleaved, size_t numFrames) {
    if (!interleaved || numFrames == 0) return true;
    // Counted in BEFORE the flag is read, so stop() can wait for a block
    // that got past it (see stop()). Both sides sequentially consistent:
    // either stop() sees this count, or this sees the flag already down.
    writers_.fetch_add(1, std::memory_order_seq_cst);
    bool fitted = false;
    if (running_.load(std::memory_order_seq_cst)) {
      if (failed_.load(std::memory_order_acquire)) {
        lost_.fetch_add(numFrames, std::memory_order_relaxed);
      } else {
        const uint64_t before = ring_.droppedFrames();
        ring_.write(interleaved, numFrames);
        fitted = ring_.droppedFrames() == before;
      }
    }
    writers_.fetch_sub(1, std::memory_order_release);
    return fitted;
  }

  /** Finish: drain what is left, patch the header, close. Safe to call twice
   *  and safe to call on an object that never started. */
  void stop() {
    if (!file_) return;
    running_.store(false, std::memory_order_seq_cst);
    // The flag stops the NEXT write(), not one already past it -- a block
    // the audio thread was still copying in while this drained, closed, and
    // a start() for the next take reset the ring under it. That block was
    // lost from the take it belonged to and landed at the head of the next
    // one behind a run of silence, and a ring that start() grew was freed
    // beneath the copy. One block's copy is microseconds; it is waited out.
    while (writers_.load(std::memory_order_seq_cst) != 0) std::this_thread::yield();
    if (thread_.joinable()) thread_.join();
    // Whatever the thread did not get to. It stopped on a flag, not on an
    // empty ring, so there is almost always a block still in there.
    flushRing();
    const uint64_t frames = frames_.load(std::memory_order_relaxed);
    writeHeader(frames, /*rewind=*/true);
    std::fclose(file_);
    file_ = nullptr;
  }

  /**
   * The 44-byte header for a float recording of `frames` frames.
   *
   * A WAV chunk length is 32 bits. Past 4 GB the file is no longer a WAV
   * whatever is written, so the lengths are clamped -- to whole frames, and
   * low enough that the RIFF length, which is the data length plus 36, still
   * fits. The data was clamped to 0xFFFFFFF0 and the RIFF length then
   * overflowed to 20: a long take whose header said the file was 28 bytes,
   * which a reader refuses outright, instead of one that plays the 4 GB that
   * fit.
   */
  static void header(uint8_t h[44], uint64_t frames, int channels, uint32_t sampleRate) {
    const uint32_t bytesPerFrame = (uint32_t) channels * (uint32_t) sizeof(float);
    const uint64_t ceiling = ((0xFFFFFFFFull - 36u) / bytesPerFrame) * bytesPerFrame;
    const uint64_t dataBytes64 = frames * bytesPerFrame;
    const uint32_t dataBytes = (uint32_t) (dataBytes64 > ceiling ? ceiling : dataBytes64);
    std::memcpy(h, "RIFF", 4);
    putU32(h + 4, 36u + dataBytes);
    std::memcpy(h + 8, "WAVE", 4);
    std::memcpy(h + 12, "fmt ", 4);
    putU32(h + 16, 16);
    putU16(h + 20, 3); // IEEE float, as writeWav does
    putU16(h + 22, (uint16_t) channels);
    putU32(h + 24, sampleRate);
    putU32(h + 28, sampleRate * bytesPerFrame);
    putU16(h + 32, (uint16_t) bytesPerFrame);
    putU16(h + 34, 32);
    std::memcpy(h + 36, "data", 4);
    putU32(h + 40, dataBytes);
  }

  /** [any thread] True between a successful start() and stop(). */
  bool recording() const { return running_.load(std::memory_order_acquire); }
  /** Frames actually on disk. */
  uint64_t framesWritten() const { return frames_.load(std::memory_order_relaxed); }
  /** Frames that are not in the recording: the ring had no room for them,
   *  or the disk refused them or a write before them. Any number above zero
   *  is a hole in the recording, not a rounding detail. */
  uint64_t framesDropped() const {
    return ring_.droppedFrames() + lost_.load(std::memory_order_relaxed);
  }
  /** The file of the last start() that opened one, UTF-8; it stays after
   *  stop(). [main-thread], as start() is. */
  const std::string& path() const { return path_; }

private:
  void drain() {
    while (running_.load(std::memory_order_acquire)) {
      if (!flushRing()) {
        // Nothing waiting. Sleeping beats spinning: this thread has no
        // deadline and a busy loop would take a core away from the one that
        // does.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    }
  }

  /** Move everything currently in the ring to the file. Returns whether there
   *  was anything to move.
   *
   *  The first write the disk refuses ends the file; what follows is drained
   *  and counted, never written. A refused write used to be skipped and the
   *  next one tried: write() kept saying everything fitted, framesDropped()
   *  stayed at zero, and when the disk took writes again the take went on
   *  from wherever the C library's buffer had left the file -- part of a
   *  frame later, so every sample after the gap was read from the wrong
   *  bytes: noise to the end of the take. Each chunk is flushed before it is
   *  counted, so framesWritten() is what reached the file and the header
   *  stop() patches says no more than that. */
  bool flushRing() {
    bool moved = false;
    for (;;) {
      const size_t have = ring_.available();
      if (have == 0) break;
      const size_t chunk = scratch_.size() / (size_t) channels_;
      const size_t take = have < chunk ? have : chunk;
      ring_.readInterleaved(scratch_.data(), take);
      const size_t values = take * (size_t) channels_;
      if (!failed_.load(std::memory_order_relaxed) && file_ &&
          std::fwrite(scratch_.data(), sizeof(float), values, file_) == values &&
          std::fflush(file_) == 0) {
        frames_.fetch_add(take, std::memory_order_relaxed);
      } else {
        failed_.store(true, std::memory_order_release);
        lost_.fetch_add(take, std::memory_order_relaxed);
      }
      moved = true;
    }
    return moved;
  }

  static void putU32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t) (v & 0xff);
    p[1] = (uint8_t) ((v >> 8) & 0xff);
    p[2] = (uint8_t) ((v >> 16) & 0xff);
    p[3] = (uint8_t) ((v >> 24) & 0xff);
  }
  static void putU16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t) (v & 0xff);
    p[1] = (uint8_t) ((v >> 8) & 0xff);
  }

  /** The same 44 bytes writeWav() emits, built here so it can be written
   *  twice: once empty and once with the real length. Deliberately NOT
   *  factored into wav.h -- that function's whole shape is "one buffer, one
   *  file", and bending it to also produce a header in isolation would make
   *  the common case harder to read for the sake of this one. */
  bool writeHeader(uint64_t frames, bool rewind = false) {
    if (!file_) return false;
    uint8_t h[44];
    header(h, frames, channels_, sampleRate_);
    if (rewind && std::fseek(file_, 0, SEEK_SET) != 0) return false;
    const bool ok = std::fwrite(h, 1, sizeof(h), file_) == sizeof(h);
    if (rewind) std::fseek(file_, 0, SEEK_END);
    return ok;
  }

  /** The frame count a header is written with while the take is running:
   *  more than a WAV holds, which header() clamps to the most it can say. */
  static constexpr uint64_t kOpenLength = 0xFFFFFFFFull;

  AudioRing ring_;
  std::vector<float> scratch_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  /** write() calls in progress: what stop() waits for. */
  std::atomic<int> writers_{0};
  std::atomic<uint64_t> frames_{0};
  /** The disk refused a write; nothing more goes to the file this take. */
  std::atomic<bool> failed_{false};
  /** Frames handed over that the file will not have, because of that. */
  std::atomic<uint64_t> lost_{0};
  std::FILE* file_ = nullptr;
  std::string path_;
  int channels_ = 2;
  uint32_t sampleRate_ = 48000;
};

} // namespace sonore
