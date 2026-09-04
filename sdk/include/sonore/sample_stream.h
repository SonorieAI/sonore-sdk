// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: playing a file from disk instead of from memory.
//
// audiostream.h can read frame N of a file without loading the rest. It
// cannot be called from an audio callback: a seek and an fread block for as
// long as the disk feels like, and "as long as the disk feels like" on a
// laptop that has just decided to index something is tens of milliseconds.
//
// So there are two sides, and the whole design is the boundary between them:
//
//   A WORKER thread does the reading, into a window that runs ahead of where
//   playback is.
//   The AUDIO thread reads out of that window and never touches the file.
//
// The window is double-buffered rather than a ring, deliberately. A ring
// gives back a range in two pieces when it wraps, and every caller then has
// to handle the split -- including an interpolator that wants four contiguous
// samples and would otherwise need a special case at exactly the point where
// a bug is least visible. Two buffers cost twice the memory of one and give
// back a plain pointer every time.
//
// What it does NOT do is hide an underrun. If playback reaches frames the
// worker has not fetched, the read returns short and the counter goes up.
// Papering over it with silence and saying nothing would turn a disk that
// cannot keep up into a plugin that sounds broken for no stated reason.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "audiostream.h"

namespace sonore {

/**
 * One file, one playback position, streamed.
 *
 * ONE position: a second voice playing the same file at a different point
 * needs a second streamer, with its own file handle. Serving two positions
 * from one window would mean a window big enough to span them, which for two
 * notes at opposite ends of a five-minute recording is the whole file: the
 * thing this exists to avoid.
 */
class SampleStreamer {
public:
  /** Frames held in each half of the window. Two halves, so the worker always
   *  has somewhere to write that the audio thread is not reading. 32768 at
   *  48 kHz is 0.68 s per half, which is more scheduling slack than any
   *  desktop needs and small enough that a hundred voices is still megabytes
   *  rather than gigabytes. */
  static constexpr size_t kHalfFrames = 32768;

  bool open(const char* path) {
    if (!reader_.open(path)) {
      error_ = reader_.error();
      return false;
    }
    channels_ = reader_.numChannels();
    buffer_.assign(kHalfFrames * 2 * (size_t) channels_, 0.0f);
    resetCounters();
    // Not started: a caller decides where playback begins, and priming from
    // frame 0 for a sample that will be played from its loop point would be
    // a read thrown away.
    ready_.store(false, std::memory_order_release);
    return true;
  }

  void close() {
    reader_.close();
    ready_.store(false, std::memory_order_release);
  }

  bool isOpen() const { return reader_.isOpen(); }
  uint16_t numChannels() const { return channels_; }
  uint64_t numFrames() const { return reader_.numFrames(); }
  uint32_t sampleRate() const { return reader_.sampleRate(); }
  const std::string& error() const { return error_; }

  /** Frames the audio thread asked for and the worker had not fetched. Zero
   *  is the only good answer; anything else is a disk that did not keep up or
   *  a service() that is not being called often enough. */
  uint64_t underrunFrames() const { return underruns_.load(std::memory_order_relaxed); }
  /** How many times the window had to be rebuilt from scratch because
   *  playback jumped outside it. Normal after a seek, and a sign of thrashing
   *  if it climbs while playing straight through. */
  uint64_t refills() const { return refills_.load(std::memory_order_relaxed); }

  /**
   * [main] Pin the window over a loop, so service() stops sliding past it.
   *
   * A window that follows the play position is exactly right for a one-shot
   * and exactly wrong for a loop: playback crosses into the second half, the
   * window slides forward, and then the loop jumps BACK to a point the window
   * has already left. The result is a rebuild per cycle and silence in
   * between -- which is what happened, at 8.7% of a three-cycle render.
   *
   * When the loop fits, the answer is simply not to slide. When it does not,
   * this says so through loopFitsWindow() rather than pretending: a loop
   * longer than 1.3 seconds needs a second stream held at its start, which
   * this class does not do, and a caller that asks should be told.
   */
  void setLoop(uint64_t start, uint64_t end) {
    loopStart_ = start;
    loopEnd_ = end;
    pinned_ = false;
    // Reset BEFORE the early returns. fits_ was set true only at the bottom and
    // never cleared at the top, so widening a fitting loop past the window --
    // a sample-editor drag -- took the early return with fits_ STILL true and
    // the new bounds, and service() then pinned the window at loopStart_
    // forever: silence for everything past loopStart_ + one window.
    fits_ = false;
    if (end <= start) return;
    // Aligning the base down costs up to one half of the span, so the loop
    // has to fit in what is left rather than in the whole window.
    const uint64_t aligned = (start / kHalfFrames) * kHalfFrames;
    if (end - aligned > kHalfFrames * 2) return;
    // Recorded, NOT acted on. A sampler almost always starts at the beginning
    // of the file and reaches the loop later -- pinning the window over the
    // loop here would leave the attack outside it, which is silence for
    // exactly as long as the attack lasts. The pin happens in service(), when
    // playback actually arrives.
    fits_ = true;
  }

  void clearLoop() {
    pinned_ = false;
    fits_ = false;
    loopEnd_ = loopStart_ = 0;
  }

  /** Whether the loop that was set fits inside the window. False means
   *  service() will keep sliding and a cycle will cost a rebuild. */
  bool loopFitsWindow() const { return fits_; }
  /** Whether the window is currently held over the loop rather than
   *  following playback. False until playback reaches the loop. */
  bool isPinned() const { return pinned_; }

  /** [main or worker] Point the window at `frame` and fill it.
   *
   *  Blocking: it reads. Call it before playback starts, or from the worker
   *  when a voice jumps. */
  void seek(uint64_t frame) {
    if (!reader_.isOpen()) return;
    if (frame > reader_.numFrames()) frame = reader_.numFrames();
    // ALIGNED DOWN to a half boundary. Which half holds a frame is decided by
    // its position in the FILE, so that the answer does not change when the
    // window slides -- and that only works if the window starts on a
    // boundary. An unaligned base made halfIndexFor() and the offset within
    // the half disagree, which reads the right samples out of the wrong half.
    const uint64_t aligned = (frame / kHalfFrames) * kHalfFrames;
    ready_.store(false, std::memory_order_release);
    const size_t first = halfIndexFor(aligned);
    filled_[first].store(fillHalf(first, aligned), std::memory_order_relaxed);
    filled_[first ^ 1].store(fillHalf(first ^ 1, aligned + kHalfFrames), std::memory_order_relaxed);
    base_.store(aligned, std::memory_order_release);
    refills_.fetch_add(1, std::memory_order_relaxed);
    ready_.store(true, std::memory_order_release);
  }

  /**
   * [worker] Keep the window ahead of `playPosition`.
   *
   *  Called as often as the host likes; it does nothing until playback has
   *  actually left a half, at which point it refetches that half for the far
   *  side of the window. Doing it any earlier would mean reading frames that
   *  are still being played out of.
   */
  void service(uint64_t playPosition) {
    if (!reader_.isOpen()) return;
    // Self-priming, so a caller cannot forget: the first service is a seek to
    // wherever playback is, which is what a caller would have had to do by
    // hand and would eventually not do.
    if (!ready_.load(std::memory_order_acquire)) {
      seek(playPosition);
      return;
    }

    // Inside a loop that fits: hold the window over it and stop following.
    //
    // Following is exactly right for a one-shot and exactly wrong here.
    // Playback crosses into the second half, the window slides forward, and
    // then the loop jumps BACK to a point the window has already left --
    // a rebuild per cycle with silence in between, which measured 8.7% of a
    // three-cycle render.
    if (fits_ && loopEnd_ > loopStart_ && playPosition >= loopStart_) {
      if (!pinned_) {
        seek(loopStart_);
        pinned_ = true;
      }
      return;
    }
    pinned_ = false;

    const uint64_t windowBase = base_.load(std::memory_order_acquire);
    // Outside the window entirely: something jumped, so start again there
    // rather than crawling forwards one half at a time.
    if (playPosition < windowBase || playPosition >= windowBase + kHalfFrames * 2) {
      seek(playPosition);
      return;
    }
    if (playPosition < windowBase + kHalfFrames) return; // still in the first half

    // Playback has crossed into the second half, so the first is finished
    // with and becomes the half AFTER the second.
    //
    // The audio thread is reading at or after playPosition, which is past
    // everything in the recycled half -- that is the ONLY reason this is safe
    // without a lock, and it is why service() must be given a position no
    // later than what the audio thread is about to read. The order below
    // matters: the samples land first, then the count that says how many are
    // valid, then the base that makes the audio thread look there at all.
    const uint64_t nextBase = windowBase + kHalfFrames;
    const size_t recycled = halfIndexFor(windowBase);
    const size_t filled = fillHalf(recycled, nextBase + kHalfFrames);
    filled_[recycled].store(filled, std::memory_order_release);
    base_.store(nextBase, std::memory_order_release);
  }

  /**
   * [audio] Copy `frames` frames from `startFrame` into `interleaved`.
   *
   *  Returns how many it could serve. A short answer means the worker has not
   *  fetched that far, which is counted; it is never silently padded, because
   *  a caller that gets 128 back when it asked for 128 should be able to
   *  believe it.
   *
   *  No locks, no allocation, no file access.
   */
  size_t read(uint64_t startFrame, float* interleaved, size_t frames) {
    if (!interleaved || frames == 0 || channels_ == 0) return 0;
    if (!ready_.load(std::memory_order_acquire)) {
      underruns_.fetch_add(frames, std::memory_order_relaxed);
      return 0;
    }
    const uint64_t windowBase = base_.load(std::memory_order_acquire);
    if (startFrame < windowBase) {
      underruns_.fetch_add(frames, std::memory_order_relaxed);
      return 0;
    }

    size_t done = 0;
    while (done < frames) {
      const uint64_t at = startFrame + done;
      if (at >= windowBase + kHalfFrames * 2) break;
      const size_t half = halfIndexFor(at);
      const size_t within = (size_t) ((at - windowBase) % kHalfFrames);
      const size_t have = filled_[half].load(std::memory_order_acquire);
      if (within >= have) break; // past the end of the file, or unfetched
      const size_t run = have - within;
      const size_t take = (frames - done) < run ? (frames - done) : run;
      std::memcpy(interleaved + done * channels_,
                  buffer_.data() + (half * kHalfFrames + within) * channels_,
                  take * channels_ * sizeof(float));
      done += take;
    }
    if (done < frames) {
      // Only the shortfall that is NOT simply the end of the file. Running
      // out of recording is not a performance problem and should not read as
      // one on a counter someone is watching.
      const uint64_t total = reader_.numFrames();
      const uint64_t wanted = startFrame + frames;
      const uint64_t beyondFile = wanted > total ? wanted - total : 0;
      const uint64_t missing = frames - done;
      if (missing > beyondFile)
        underruns_.fetch_add(missing - beyondFile, std::memory_order_relaxed);
    }
    return done;
  }

private:
  void resetCounters() {
    underruns_.store(0, std::memory_order_relaxed);
    refills_.store(0, std::memory_order_relaxed);
  }

  /** Which half of the window holds a frame. The halves alternate every
   *  kHalfFrames from the start of the FILE, not from the window, so the
   *  answer does not change when the window slides. */
  size_t halfIndexFor(uint64_t frame) const {
    return (size_t) ((frame / kHalfFrames) & 1);
  }

  size_t fillHalf(size_t half, uint64_t fromFrame) {
    if (fromFrame >= reader_.numFrames()) return 0;
    return reader_.read(fromFrame, buffer_.data() + half * kHalfFrames * channels_, kHalfFrames);
  }

  AudioFileReader reader_;
  std::vector<float> buffer_;
  std::atomic<size_t> filled_[2] = {{0}, {0}};
  std::atomic<uint64_t> base_{0};
  uint16_t channels_ = 0;
  std::atomic<bool> ready_{false};
  std::atomic<uint64_t> underruns_{0};
  std::atomic<uint64_t> refills_{0};
  uint64_t loopStart_ = 0, loopEnd_ = 0;
  bool pinned_ = false, fits_ = false;
  std::string error_;
};

} // namespace sonore
