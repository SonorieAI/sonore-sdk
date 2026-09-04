// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the one lock-free ring both directions of audio use.
//
// It lived inside audio_input.h, in a `standalone` namespace, because a live
// capture device was the only thing that needed it. A recorder needs exactly
// the same object pointing the other way -- audio thread writes, another
// thread drains -- and including audio_input.h to get it would drag a WASAPI
// or ALSA backend into a plugin that only wants to write a file.
//
// So it lives here, and audio_input.h keeps its old name for it. One ring,
// one set of index arithmetic, one place where "full" and "empty" are told
// apart.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace sonore {

/** One writer, one reader, no locks, no allocation after construction.
 *
 *  Stereo interleaved, because that is what crosses the boundary: the DSP is
 *  handed two channels and a capture device with six has already been folded
 *  down by the time it gets here.
 *
 *  Capacity is rounded UP to a power of two so the wrap is a mask rather than
 *  a modulo, and one frame is left permanently unused so full and empty are
 *  distinguishable without a third variable that the two threads would then
 *  have to agree about. */
class AudioRing {
public:
  /** `channels` defaults to two, which is what a capture device gives and
   *  what every existing caller wants. A recorder writing a surround bus to
   *  disk needs more, and the index arithmetic is the same either way -- so
   *  it is the same ring rather than a second one that drifts. */
  void reset(size_t frames, int channels = 2) {
    size_t capacity = 64;
    while (capacity < frames + 1) capacity <<= 1;
    channels_ = channels < 1 ? 1 : channels;
    data_.assign(capacity * (size_t) channels_, 0.0f);
    mask_ = capacity - 1;
    write_.store(0, std::memory_order_relaxed);
    read_.store(0, std::memory_order_relaxed);
    dropped_.store(0, std::memory_order_relaxed);
    starved_.store(0, std::memory_order_relaxed);
  }

  size_t capacityFrames() const { return data_.empty() ? 0 : (mask_ + 1) - 1; }
  int channels() const { return channels_; }

  size_t available() const {
    const size_t w = write_.load(std::memory_order_acquire);
    const size_t r = read_.load(std::memory_order_acquire);
    return (w - r) & mask_;
  }

  /** Capture side. Frames that do not fit are DROPPED and counted, never
   *  written over frames the reader has not taken: overwriting would make the
   *  reader's next block a splice of two different moments, which sounds far
   *  worse than a gap and is much harder to recognise. */
  void write(const float* interleaved, size_t frames) {
    if (data_.empty()) return;
    const size_t w = write_.load(std::memory_order_relaxed);
    const size_t r = read_.load(std::memory_order_acquire);
    const size_t room = mask_ - ((w - r) & mask_);
    const size_t take = frames < room ? frames : room;
    const size_t ch = (size_t) channels_;
    for (size_t i = 0; i < take; ++i) {
      const size_t slot = ((w + i) & mask_) * ch;
      for (size_t c = 0; c < ch; ++c) data_[slot + c] = interleaved[i * ch + c];
    }
    write_.store((w + take) & mask_, std::memory_order_release);
    if (take < frames) dropped_.fetch_add(frames - take, std::memory_order_relaxed);
  }

  /** Render side. A short read is filled with SILENCE and counted. Repeating
   *  the last frame would be cheaper and would turn a gap into a click that
   *  sounds like a bad plugin rather than a starved input. */
  void read(float* left, float* right, size_t frames) {
    if (data_.empty()) {
      std::memset(left, 0, frames * sizeof(float));
      std::memset(right, 0, frames * sizeof(float));
      return;
    }
    const size_t r = read_.load(std::memory_order_relaxed);
    const size_t w = write_.load(std::memory_order_acquire);
    const size_t have = (w - r) & mask_;
    const size_t take = frames < have ? frames : have;
    const size_t ch = (size_t) channels_;
    for (size_t i = 0; i < take; ++i) {
      const size_t slot = ((r + i) & mask_) * ch;
      left[i] = data_[slot];
      // A mono ring feeding a stereo reader gives both sides the same sample
      // rather than silence on the right, which is what "mono" means to a
      // listener and not what an out-of-range read would give them.
      right[i] = data_[slot + (ch > 1 ? 1 : 0)];
    }
    for (size_t i = take; i < frames; ++i) left[i] = right[i] = 0.0f;
    read_.store((r + take) & mask_, std::memory_order_release);
    if (take < frames) starved_.fetch_add(frames - take, std::memory_order_relaxed);
  }

  /** Frames the capture side could not fit. Non-zero means the render side is
   *  not keeping up, or stopped. */
  /**
   * Drain side, interleaved and WITHOUT the silence fill.
   *
   * The stereo read() above pads a short read with silence, because a render
   * that arrives late still has to produce a block. A file has no such
   * deadline: a recorder asks for exactly what is there, and padding would
   * write silence into the take that nobody played.
   *
   * Returns how many frames were actually taken, which is what the caller
   * writes rather than what it asked for.
   */
  size_t readInterleaved(float* out, size_t frames) {
    if (data_.empty() || !out) return 0;
    const size_t r = read_.load(std::memory_order_relaxed);
    const size_t w = write_.load(std::memory_order_acquire);
    const size_t have = (w - r) & mask_;
    const size_t take = frames < have ? frames : have;
    const size_t ch = (size_t) channels_;
    for (size_t i = 0; i < take; ++i) {
      const size_t slot = ((r + i) & mask_) * ch;
      for (size_t c = 0; c < ch; ++c) out[i * ch + c] = data_[slot + c];
    }
    read_.store((r + take) & mask_, std::memory_order_release);
    return take;
  }

  uint64_t droppedFrames() const { return dropped_.load(std::memory_order_relaxed); }
  /** Frames the render side wanted and did not have. Non-zero means the
   *  capture side is behind, which is what clock drift looks like from here. */
  uint64_t starvedFrames() const { return starved_.load(std::memory_order_relaxed); }

private:
  std::vector<float> data_;
  size_t mask_ = 0;
  int channels_ = 2;
  std::atomic<size_t> write_{0};
  std::atomic<size_t> read_{0};
  std::atomic<uint64_t> dropped_{0};
  std::atomic<uint64_t> starved_{0};
};

} // namespace sonore
