// SPDX-License-Identifier: Apache-2.0
//
// MidiMessageCollector: MIDI from other threads, into the next audio block,
// at the sample position its arrival time implies.
//
// ── The problem ─────────────────────────────────────────────────────────────
//
// A MIDI driver delivers on its own thread, an on-screen keyboard on the UI
// thread, a network bridge on a socket thread. The audio thread renders in
// blocks and wants each event at a FRAME inside the block. midiin::
// MessageQueue answers the first half -- a lock-free hand-over -- and puts
// everything at frame 0, which for a single USB keyboard is honest: its own
// jitter is already larger than a block.
//
// It is not honest for everything. At a 2048-frame block (43 ms at 48 kHz,
// which hosts do use when they are rendering a heavy session) every note in
// a fast run lands on the same frame, and a roll becomes a chord. Sources
// whose timing is better than the block -- a sequencer on another thread, a
// network clock, a driver's own timestamps -- lose it all at frame 0.
//
// ── What this does ──────────────────────────────────────────────────────────
//
// Each event is stamped with a time in seconds when it is queued (now(), a
// steady clock every thread shares, unless the source has a better stamp of
// its own). Each block, the audio thread takes the interval since the
// previous block and maps it onto the block's frames:
//
//   * an interval as long as the block maps one-to-one, so an event's offset
//     into the interval IS its frame;
//   * a SHORTER interval (a callback that came early) is laid against the
//     END of the block, so the delay from arrival to sound stays one block
//     however the host schedules its callbacks -- that constant block of
//     latency is the price of removing the jitter, and the reference pays it
//     the same way;
//   * a LONGER interval (a late callback, or a stall) is squeezed into the
//     block, keeping the events' order and relative spacing. Past 32 blocks
//     only the most recent 32 blocks' worth keeps its spacing; anything older
//     lands on frame 0. The reference DROPS those older events; here they are
//     kept, because one of them may be a note-off and a lost note-off is a
//     note that never stops.
//
// The mapping is a pure function, samplePosition(), so the arithmetic is
// tested with numbers rather than with a thread and a stopwatch.
//
// ── Threads ─────────────────────────────────────────────────────────────────
//
// Any number of producers; one consumer, the audio thread. The queue is a
// bounded ring with a sequence number per slot (Vyukov's): a producer claims
// a slot with one compare-and-swap and never waits -- a full queue refuses,
// and counts it -- and the consumer never waits at all. Nothing allocates
// after construction. The reference's collector takes a lock in the audio
// callback; this one cannot, and the test suite counts allocations around
// the drain to keep it that way.
//
// An event the block has no room for is LEFT in the queue for the next block
// rather than taken out and lost: it arrives a block late, at frame 0, which
// is better than not at all when it is a note-off.
#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "audio.h"

namespace sonore {
namespace detail {

/**
 * Bounded multi-producer, single-consumer ring.
 *
 * The consumer side is split into peek() and pop() so it can look at an
 * element, find the block full, and leave it where it is. A producer fills
 * its slot in place through a callback, so a 2 KB SysEx slot is written once
 * rather than built on the producer's stack and copied.
 */
template <typename T, size_t Capacity>
class CollectorRing {
  static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "a power of two");
  static_assert(std::atomic<size_t>::is_always_lock_free,
                "the hand-over to the audio thread must not fall back to a lock");

public:
  CollectorRing() {
    for (size_t i = 0; i < Capacity; ++i) slots_[i].seq.store(i, std::memory_order_relaxed);
  }

  /** [any thread] `fill(T&)` writes the claimed slot. False when full. */
  template <typename Fill>
  bool push(Fill&& fill) {
    size_t pos = head_.load(std::memory_order_relaxed);
    for (;;) {
      Slot& slot = slots_[pos & (Capacity - 1)];
      const size_t seq = slot.seq.load(std::memory_order_acquire);
      const std::ptrdiff_t lag = (std::ptrdiff_t) (seq - pos);
      if (lag == 0) {
        if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          fill(slot.value);
          slot.seq.store(pos + 1, std::memory_order_release);
          return true;
        }
      } else if (lag < 0) {
        return false; // full: the consumer has not reached this slot yet
      } else {
        pos = head_.load(std::memory_order_relaxed);
      }
    }
  }

  /** [consumer] The front element, or null when there is none ready. A slot
   *  a producer has claimed but not finished reads as empty, and so does
   *  everything behind it until it is: order is kept, nobody waits. */
  const T* peek() const {
    const Slot& slot = slots_[tail_ & (Capacity - 1)];
    const size_t seq = slot.seq.load(std::memory_order_acquire);
    if ((std::ptrdiff_t) (seq - (tail_ + 1)) < 0) return nullptr;
    return &slot.value;
  }

  /** [consumer] Release the element peek() returned. */
  void pop() {
    Slot& slot = slots_[tail_ & (Capacity - 1)];
    slot.seq.store(tail_ + Capacity, std::memory_order_release);
    ++tail_;
  }

  /** [consumer] Discard everything ready. */
  void clear() {
    while (peek()) pop();
  }

private:
  struct Slot {
    std::atomic<size_t> seq{0};
    T value{};
  };
  Slot slots_[Capacity];
  std::atomic<size_t> head_{0};
  size_t tail_ = 0;
};

struct TimedMidiMessage {
  MidiMessage message;
  double time = 0.0;
};

struct TimedSysex {
  double time = 0.0;
  uint16_t length = 0;
  uint8_t bytes[MidiBuffer::kSysexCapacity];
};

} // namespace detail

/**
 * MIDI queued from any thread, handed to the audio thread a block at a time
 * at the frames its arrival times imply. Mirrors the reference's
 * MidiMessageCollector, without its lock in the audio callback and without
 * dropping the events of a long gap; the file comment has the mapping and
 * why.
 *
 * Any number of producers call addMessageToQueue() and addSysexToQueue(), on
 * any thread, without waiting. ONE consumer -- the audio thread -- calls
 * removeNextBlockOfMessages(), and reset() belongs to that same side: call it
 * from prepare() with the sample rate before the first block, or from the
 * audio thread, never while a drain is running elsewhere. Every stamp is in
 * now()'s seconds.
 *
 * Fixed capacity, allocated inline: about 80 KB of ring, so it belongs in a
 * member, not on a stack. Not copyable.
 */
class MidiMessageCollector {
public:
  /** Short messages in flight. 2048 is seconds of the densest MPE stream a
   *  controller sends between two blocks, and 2048 x 24 bytes is 48 KB. */
  static constexpr size_t kCapacity = 2048;
  /** Complete SysEx messages in flight, each up to what one block can carry. */
  static constexpr size_t kSysexSlots = 8;
  /** How many blocks of a long gap keep their spacing; see the header. */
  static constexpr int kMaxSpreadBlocks = 32;

  /** The clock every timestamp here is on: steady (never jumps when the
   *  wall clock is set) and shared by every thread in the process. */
  static double now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  /**
   * Start, or start over, at this sample rate: from prepare(), or from the
   * audio thread itself (it is the consumer). Clears whatever is queued, and
   * takes `nowSeconds` as the time the first block's interval begins.
   */
  void reset(double sampleRate) { reset(sampleRate, now()); }
  /** The same, with the first block's start given in now()'s seconds, as a
   *  test or a caller with its own clock does. */
  void reset(double sampleRate, double nowSeconds) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 0.0;
    previousBlock_ = nowSeconds;
    started_ = true;
    events_.clear();
    sysex_.clear();
  }

  /** The rate reset() was given, in Hz; 0 before it, or for one that was not
   *  positive. */
  double sampleRate() const { return sampleRate_; }

  /** [any thread] Queue a short message, stamped now. False when the queue
   *  is full (counted in dropped()) or the message is not one a DSP takes. */
  bool addMessageToQueue(const MidiMessage& m) { return addMessageToQueue(m, now()); }

  /** [any thread] Queue with a stamp of the caller's own, in now()'s
   *  seconds -- a driver that timestamps on arrival knows better than the
   *  moment its callback ran. */
  bool addMessageToQueue(const MidiMessage& m, double timestampSeconds) {
    if (!deliverableToDsp(m.getRawStatus())) return false;
    const bool ok = events_.push([&](detail::TimedMidiMessage& slot) {
      slot.message = m;
      slot.time = timestampSeconds;
    });
    if (!ok) dropped_.fetch_add(1, std::memory_order_relaxed);
    return ok;
  }

  /** [any thread] A complete SysEx, 0xF0 through 0xF7, copied in. */
  bool addSysexToQueue(const uint8_t* data, size_t bytes) {
    return addSysexToQueue(data, bytes, now());
  }
  /** [any thread] The same, stamped `timestampSeconds` in now()'s seconds.
   *  False for a message too short or too long for a block, or a full queue
   *  (counted in dropped()). */
  bool addSysexToQueue(const uint8_t* data, size_t bytes, double timestampSeconds) {
    if (!data || bytes < 2 || bytes > (size_t) MidiBuffer::kSysexCapacity) return false;
    const bool ok = sysex_.push([&](detail::TimedSysex& slot) {
      for (size_t i = 0; i < bytes; ++i) slot.bytes[i] = data[i];
      slot.length = (uint16_t) bytes;
      slot.time = timestampSeconds;
    });
    if (!ok) dropped_.fetch_add(1, std::memory_order_relaxed);
    return ok;
  }

  /** Messages refused because the queue was full. */
  uint32_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

  /** [audio-thread] Everything queued, into `dest`, positioned for a block of
   *  `numSamples` frames that is being rendered now. */
  void removeNextBlockOfMessages(MidiBuffer& dest, int numSamples) {
    removeNextBlockOfMessages(dest, numSamples, now());
  }

  /**
   * [audio-thread] The same, with the block's time given: a host that says
   * when its block was scheduled, or a test, supplies it.
   *
   * No allocation, no lock, no system call beyond now()'s clock read. A
   * zero-length block takes nothing and does not move the interval on, so
   * the events wait for a block that has frames to put them at.
   */
  void removeNextBlockOfMessages(MidiBuffer& dest, int numSamples, double blockTimeSeconds) {
    if (numSamples <= 0) return;
    if (!started_) {
      // Never reset: assume one block has passed, so the first block's
      // events still get positions rather than all landing on frame 0.
      previousBlock_ = sampleRate_ > 0.0 ? blockTimeSeconds - (double) numSamples / sampleRate_
                                         : blockTimeSeconds;
      started_ = true;
    }
    const double previous = previousBlock_;
    previousBlock_ = blockTimeSeconds;

    while (const detail::TimedMidiMessage* e = events_.peek()) {
      if (dest.isFull()) break; // left for the next block, not lost
      dest.addEvent(e->message,
                    samplePosition(e->time, previous, blockTimeSeconds, sampleRate_, numSamples));
      events_.pop();
    }
    while (const detail::TimedSysex* s = sysex_.peek()) {
      if (!dest.addSysex(s->bytes, s->length,
                         samplePosition(s->time, previous, blockTimeSeconds, sampleRate_,
                                        numSamples)))
        break;
      sysex_.pop();
    }
  }

  /**
   * Where in a block an event stamped `t` belongs.
   *
   * `previous` and `current` are the times of the last block and this one;
   * the interval between them is what this block stands for. Always a frame
   * inside the block, 0..numSamples-1: an event from before the interval --
   * one that waited a block for room -- is at 0, and one stamped after this
   * block's own time is at the end. NaN stamps land on 0.
   */
  static int samplePosition(double t, double previous, double current, double sampleRate,
                            int numSamples) {
    if (numSamples <= 0 || !(sampleRate > 0.0)) return 0;
    const double block = (double) numSamples;
    double elapsed = std::floor((current - previous) * sampleRate + 0.5);
    if (!(elapsed >= 1.0)) elapsed = 1.0; // also catches a NaN interval
    double at = (t - previous) * sampleRate;
    if (elapsed > block) {
      const double spread = block * (double) kMaxSpreadBlocks;
      const double window = elapsed < spread ? elapsed : spread;
      at = (at - (elapsed - window)) * block / window;
    } else {
      at += block - elapsed;
    }
    // To the NEAREST frame: an event stamped exactly k frames in must land on
    // k, and the subtraction of two large clock readings leaves it a hair
    // either side of that.
    at = std::floor(at + 0.5);
    if (!(at >= 0.0)) return 0;
    if (at >= block - 1.0) return numSamples - 1;
    return (int) at;
  }

private:
  detail::CollectorRing<detail::TimedMidiMessage, kCapacity> events_;
  detail::CollectorRing<detail::TimedSysex, kSysexSlots> sysex_;
  std::atomic<uint32_t> dropped_{0};
  // The consumer's own: written by reset() and the drain, read by nobody else.
  double sampleRate_ = 0.0;
  double previousBlock_ = 0.0;
  bool started_ = false;
};

} // namespace sonore
