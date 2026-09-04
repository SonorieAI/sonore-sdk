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

#include "audio_ring.h" // the same SPSC ring a capture device fills

namespace sonore {

/**
 * A WAV file being written while audio is playing.
 *
 * start() and stop() are MAIN THREAD. write() is the only thing the audio
 * thread touches, and it allocates nothing, locks nothing and never blocks.
 *
 * 32-bit float, matching writeWav(): a recording is usually the thing a user
 * wants to edit somewhere else, and quantising on the way out is a decision
 * best left to whatever they open it in.
 */
class WavRecorder {
public:
  ~WavRecorder() { stop(); }

  WavRecorder() = default;
  WavRecorder(const WavRecorder&) = delete;
  WavRecorder& operator=(const WavRecorder&) = delete;

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

    file_ = std::fopen(path, "wb");
    if (!file_) return false;
    path_ = path;
    channels_ = channels;
    sampleRate_ = (uint32_t) (sampleRate + 0.5);
    frames_.store(0, std::memory_order_relaxed);

    // A header with zero lengths, patched on the way out. Written FIRST so
    // that a recording interrupted by a crash is still a file a tool can open
    // -- with a length of zero, but with a valid fmt chunk to read it by.
    if (!writeHeader(0)) {
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
   * [audio-thread] Hand over one block. Returns false if it did not all fit.
   *
   * The false is worth acting on: it means the disk is not keeping up and the
   * recording has a hole in it. A plugin that lights something when this
   * happens tells the user while they can still do the take again.
   */
  bool write(const float* interleaved, size_t numFrames) {
    if (!interleaved || numFrames == 0) return true;
    if (!running_.load(std::memory_order_acquire)) return false;
    const uint64_t before = ring_.droppedFrames();
    ring_.write(interleaved, numFrames);
    return ring_.droppedFrames() == before;
  }

  /** Finish: drain what is left, patch the header, close. Safe to call twice
   *  and safe to call on an object that never started. */
  void stop() {
    if (!file_) return;
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
    // Whatever the thread did not get to. It stopped on a flag, not on an
    // empty ring, so there is almost always a block still in there.
    flushRing();
    const uint64_t frames = frames_.load(std::memory_order_relaxed);
    writeHeader(frames, /*rewind=*/true);
    std::fclose(file_);
    file_ = nullptr;
  }

  bool recording() const { return running_.load(std::memory_order_acquire); }
  /** Frames actually on disk. */
  uint64_t framesWritten() const { return frames_.load(std::memory_order_relaxed); }
  /** Frames the ring had no room for. Any number above zero is a hole in the
   *  recording, not a rounding detail. */
  uint64_t framesDropped() const { return ring_.droppedFrames(); }
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
   *  was anything to move. */
  bool flushRing() {
    bool moved = false;
    for (;;) {
      const size_t have = ring_.available();
      if (have == 0) break;
      const size_t chunk = scratch_.size() / (size_t) channels_;
      const size_t take = have < chunk ? have : chunk;
      ring_.readInterleaved(scratch_.data(), take);
      const size_t values = take * (size_t) channels_;
      if (file_ && std::fwrite(scratch_.data(), sizeof(float), values, file_) == values)
        frames_.fetch_add(take, std::memory_order_relaxed);
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
    const uint32_t bytesPerFrame = (uint32_t) channels_ * (uint32_t) sizeof(float);
    // A WAV chunk length is 32 bits. Past 4 GB the file is no longer a WAV
    // whatever we write, so the length is clamped and the audio is left
    // alone: a truncated-length file plays the part that fits, where a
    // wrapped-around length plays noise.
    const uint64_t dataBytes64 = frames * bytesPerFrame;
    const uint32_t dataBytes =
        dataBytes64 > 0xFFFFFFF0ull ? 0xFFFFFFF0u : (uint32_t) dataBytes64;

    uint8_t h[44];
    std::memcpy(h, "RIFF", 4);
    putU32(h + 4, 36 + dataBytes);
    std::memcpy(h + 8, "WAVE", 4);
    std::memcpy(h + 12, "fmt ", 4);
    putU32(h + 16, 16);
    putU16(h + 20, 3); // IEEE float, as writeWav does
    putU16(h + 22, (uint16_t) channels_);
    putU32(h + 24, sampleRate_);
    putU32(h + 28, sampleRate_ * bytesPerFrame);
    putU16(h + 32, (uint16_t) bytesPerFrame);
    putU16(h + 34, 32);
    std::memcpy(h + 36, "data", 4);
    putU32(h + 40, dataBytes);

    if (rewind && std::fseek(file_, 0, SEEK_SET) != 0) return false;
    const bool ok = std::fwrite(h, 1, sizeof(h), file_) == sizeof(h);
    if (rewind) std::fseek(file_, 0, SEEK_END);
    return ok;
  }

  AudioRing ring_;
  std::vector<float> scratch_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> frames_{0};
  std::FILE* file_ = nullptr;
  std::string path_;
  int channels_ = 2;
  uint32_t sampleRate_ = 48000;
};

} // namespace sonore
