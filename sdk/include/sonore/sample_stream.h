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
#include <thread>
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
  /** How far behind the play position a reader may still read: the half it
   *  is leaving is kept until playback is this far into the next, and a seek
   *  or a pinned loop starts the window at least this far behind its frame. */
  static constexpr size_t kReachBehind = 256;

  /** [main] Open a WAV or AIFF at `path`, UTF-8, through AudioFileReader,
   *  and size the window for its channels: allocates. Nothing is readable
   *  until a seek(). False, with error() saying why, for a file the reader
   *  refuses. With no service() or read() running. */
  bool open(const char* path) {
    error_.clear(); // about the latest open(), as the reader's is
    if (!reader_.open(path)) {
      error_ = reader_.error();
      return false;
    }
    channels_ = reader_.numChannels();
    buffer_.assign(kHalfFrames * 2 * (size_t) channels_, 0.0f);
    resetCounters();
    state_.store(0, std::memory_order_seq_cst); // nothing readable until a seek fills it
    filled_[0].store(0, std::memory_order_relaxed);
    filled_[1].store(0, std::memory_order_relaxed);
    // Not started: a caller decides where playback begins, and priming from
    // frame 0 for a sample that will be played from its loop point would be
    // a read thrown away.
    ready_.store(false, std::memory_order_release);
    return true;
  }

  /** [main] Close the file; read() returns 0 from here on. With no
   *  service() running. */
  void close() {
    reader_.close();
    ready_.store(false, std::memory_order_release);
  }

  /** True after a successful open(), until close(). */
  bool isOpen() const { return reader_.isOpen(); }
  /** Channels per frame, interleaved in what read() writes. */
  uint16_t numChannels() const { return channels_; }
  /** The file's length in frames; 0 when not open. */
  uint64_t numFrames() const { return reader_.numFrames(); }
  /** The file's rate in Hz; 0 when not open. */
  uint32_t sampleRate() const { return reader_.sampleRate(); }
  /** Why the latest open() failed; empty after one that succeeded. */
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
    // Atomics, written with the flag down: service() reads them on the
    // worker while this runs on the main thread, and plain fields there were
    // a data race. A service() that catches the bounds mid-change sees fits_
    // false and follows playback for one call, which is harmless; the pin is
    // the worker's own state and is dropped there, on loopChanged_.
    //
    // Reset BEFORE the early returns. fits_ was set true only at the bottom and
    // never cleared at the top, so widening a fitting loop past the window --
    // a sample-editor drag -- took the early return with fits_ STILL true and
    // the new bounds, and service() then pinned the window at loopStart_
    // forever: silence for everything past loopStart_ + one window.
    fits_.store(false, std::memory_order_release);
    loopStart_.store(start, std::memory_order_relaxed);
    loopEnd_.store(end, std::memory_order_relaxed);
    loopChanged_.store(true, std::memory_order_release);
    if (end <= start) return;
    // Aligning the base down costs up to one half of the span, so the loop
    // has to fit in what is left rather than in the whole window. The base
    // is the one service() pins at, which serves the reach behind the start.
    const uint64_t aligned = baseFor(start);
    if (end - aligned > kHalfFrames * 2) return;
    // Recorded, NOT acted on. A sampler almost always starts at the beginning
    // of the file and reaches the loop later -- pinning the window over the
    // loop here would leave the attack outside it, which is silence for
    // exactly as long as the attack lasts. The pin happens in service(), when
    // playback actually arrives.
    fits_.store(true, std::memory_order_release);
  }

  /** [main] Drop the loop: service() follows playback again, and the pin,
   *  if any, is let go on its next call. */
  void clearLoop() {
    fits_.store(false, std::memory_order_release);
    loopStart_.store(0, std::memory_order_relaxed);
    loopEnd_.store(0, std::memory_order_relaxed);
    loopChanged_.store(true, std::memory_order_release);
  }

  /** Whether the loop that was set fits inside the window. False means
   *  service() will keep sliding and a cycle will cost a rebuild. */
  bool loopFitsWindow() const { return fits_.load(std::memory_order_acquire); }
  /** Whether the window is currently held over the loop rather than
   *  following playback. False until playback reaches the loop, and again
   *  once it has played on past the loop's end. */
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
    // And aligned from kReachBehind before the frame: see baseFor().
    const uint64_t aligned = baseFor(frame);
    ready_.store(false, std::memory_order_release);
    retire(windowBase(), 0u); // both halves are rewritten: neither is readable meanwhile
    const size_t first = halfIndexFor(aligned);
    filled_[first].store(fillHalf(first, aligned), std::memory_order_relaxed);
    filled_[first ^ 1].store(fillHalf(first ^ 1, aligned + kHalfFrames), std::memory_order_relaxed);
    state_.store(pack(aligned, 3u), std::memory_order_seq_cst);
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

    if (loopChanged_.exchange(false, std::memory_order_acq_rel)) pinned_ = false;

    // Inside a loop that fits: hold the window over it and stop following.
    //
    // Following is exactly right for a one-shot and exactly wrong here.
    // Playback crosses into the second half, the window slides forward, and
    // then the loop jumps BACK to a point the window has already left --
    // a rebuild per cycle with silence in between, which measured 8.7% of a
    // three-cycle render.
    //
    // INSIDE it, both ends: a note released mid-loop plays on past the loop's
    // end, and with no upper bound the window stayed pinned over the loop for
    // the rest of the file. Everything from one window past the loop start
    // was silence -- 101,696 frames of a 200,000-frame release tail.
    const bool fits = fits_.load(std::memory_order_acquire);
    const uint64_t loopStart = loopStart_.load(std::memory_order_relaxed);
    const uint64_t loopEnd = loopEnd_.load(std::memory_order_relaxed);
    if (fits && loopEnd > loopStart && playPosition >= loopStart && playPosition < loopEnd) {
      if (!pinned_) {
        // Pinned WITHOUT a rebuild whenever the window can get there by
        // itself. It used to seek(loopStart_) unconditionally -- which drops
        // ready_ and rereads both halves, usually the very frames already in
        // the window -- so every read() during that refill came back empty:
        // a dropout at exactly the loop entry the pin exists to protect.
        // baseFor() puts loopStart at least kReachBehind into the window, so
        // the one-half slide below also leaves the reach intact.
        const uint64_t aligned = baseFor(loopStart);
        const uint64_t windowBase = this->windowBase();
        if (windowBase == aligned) {
          // Already over the loop.
        } else if (windowBase + kHalfFrames == aligned && playPosition >= aligned) {
          // One half behind: the ordinary slide, which only rewrites the half
          // playback has finished with.
          slideOnce(windowBase);
        } else {
          seek(loopStart); // somewhere else entirely: a rebuild is the only way
        }
        pinned_ = true;
      }
      return;
    }
    pinned_ = false;

    const uint64_t windowBase = this->windowBase();
    // Outside the window entirely: something jumped, so start again there
    // rather than crawling forwards one half at a time.
    if (playPosition < windowBase || playPosition >= windowBase + kHalfFrames * 2) {
      seek(playPosition);
      return;
    }
    // Still in the first half -- or not far enough past it for the half to
    // be finished with. A reader interpolating at playPosition reaches
    // BEHIND it: SampleVoice's band-limited read needs up to 72 frames
    // before the position it plays. Recycled the moment playback crossed,
    // those frames were gone, and a voice read silence for its kernel's
    // reach at every half crossing.
    if (playPosition < windowBase + kHalfFrames + kReachBehind) return;

    // Playback has crossed into the second half, so the first is finished
    // with and becomes the half AFTER the second.
    //
    // The audio thread is reading at or after playPosition - kReachBehind,
    // which is past everything in the recycled half -- as long as playback
    // has not jumped back since. It may have: `playPosition` is whatever the
    // caller last published, and a loop or a retrigger can send the voice
    // back into this very half while the worker acts on the old position.
    // slideOnce() shuts the half to readers first, so such a read comes back
    // short rather than reading a half being overwritten.
    slideOnce(windowBase);
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
    // Inside, as far as retire() is concerned, from before the window is
    // looked at until the last sample is copied. Both orders are seq_cst, and
    // so are retire()'s: either this read sees a half already shut, or the
    // worker sees this read and waits for it before overwriting the half.
    readers_.fetch_add(1, std::memory_order_seq_cst);
    const uint64_t state = state_.load(std::memory_order_seq_cst);
    const uint64_t windowBase = baseOf(state);
    const unsigned readable = (unsigned) (state & 3u);

    size_t done = 0;
    while (startFrame >= windowBase && done < frames) {
      const uint64_t at = startFrame + done;
      if (at >= windowBase + kHalfFrames * 2) break;
      const size_t half = halfIndexFor(at);
      if (!(readable & (1u << half))) break; // being refilled
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
    readers_.fetch_sub(1, std::memory_order_release);
    if (startFrame < windowBase) {
      underruns_.fetch_add(frames, std::memory_order_relaxed);
      return 0;
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
  /** Recycle the half before playback as the half after the window. The
   *  order matters: the half is shut to readers, the samples land, then the
   *  count that says how many are valid, then the new base and the half
   *  opened again in one store that makes the audio thread look there. */
  void slideOnce(uint64_t windowBase) {
    const uint64_t nextBase = windowBase + kHalfFrames;
    const size_t recycled = halfIndexFor(windowBase);
    retire(windowBase, 1u << (recycled ^ 1)); // the other half plays on meanwhile
    const size_t filled = fillHalf(recycled, nextBase + kHalfFrames);
    filled_[recycled].store(filled, std::memory_order_relaxed);
    state_.store(pack(nextBase, 3u), std::memory_order_seq_cst);
  }

  /**
   * [main or worker] Shut every half not in `readable` to readers, then wait
   * until no read() that might still be copying out of one is running.
   *
   * The slide used to rewrite a half and trust that the audio thread was not
   * in it, because the position it was told was past it. That position is
   * the caller's LAST one: a loop wrap or a retrigger sends the voice back
   * into the half the worker is refilling while the worker still acts on the
   * position from before the jump. The voice then read the half as it was
   * overwritten -- samples from a second or more later in the file. Three
   * hundred two-thread runs, each looping four times over a loop the window
   * cannot hold, served about a million wrong frames.
   *
   * The audio thread never waits: a read that finds its half shut comes back
   * short and is counted as an underrun, as an unfetched half always was.
   * Only this side waits, and only for a read already in progress -- one
   * memcpy of at most a voice's cache.
   */
  void retire(uint64_t windowBase, unsigned readable) {
    state_.store(pack(windowBase, readable), std::memory_order_seq_cst);
    while (readers_.load(std::memory_order_seq_cst) != 0) std::this_thread::yield();
  }

  /** The window as one word: its base in halves above the two bits that say
   *  which halves may be read. One word, so a reader never pairs a base with
   *  a half that belongs to another. */
  static uint64_t pack(uint64_t base, unsigned readable) {
    return ((base / kHalfFrames) << 2) | (uint64_t) (readable & 3u);
  }
  static uint64_t baseOf(uint64_t state) { return (state >> 2) * kHalfFrames; }
  uint64_t windowBase() const { return baseOf(state_.load(std::memory_order_acquire)); }

  void resetCounters() {
    underruns_.store(0, std::memory_order_relaxed);
    refills_.store(0, std::memory_order_relaxed);
  }

  /**
   * Where a window that serves `frame` starts: the half boundary at or
   * before kReachBehind frames ahead of it.
   *
   * A reader reaches BEHIND the frame it plays -- SampleVoice refills its
   * cache from 144 frames before the frame it wants -- and a read that starts
   * before the window is served nothing at all. The slide keeps that reach;
   * seek() and the loop pin aligned the frame itself down. A loop starting
   * within the reach after a half boundary (32768 is one) was pinned with its
   * start at the window's edge, so on every cycle the voice's first refills
   * after the wrap came back empty: up to 144 frames of silence per cycle.
   */
  static uint64_t baseFor(uint64_t frame) {
    const uint64_t from = frame > kReachBehind ? frame - kReachBehind : 0;
    return (from / kHalfFrames) * kHalfFrames;
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
  std::atomic<uint64_t> state_{0}; // pack(): the window's base and its readable halves
  std::atomic<uint32_t> readers_{0}; // read() calls in progress, for retire()
  uint16_t channels_ = 0;
  std::atomic<bool> ready_{false};
  std::atomic<uint64_t> underruns_{0};
  std::atomic<uint64_t> refills_{0};
  std::atomic<uint64_t> loopStart_{0}, loopEnd_{0};
  std::atomic<bool> fits_{false}, loopChanged_{false};
  bool pinned_ = false; // the worker's: written only in service()
  std::string error_;
};

} // namespace sonore
