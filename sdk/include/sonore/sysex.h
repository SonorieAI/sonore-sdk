// SPDX-License-Identifier: Apache-2.0
//
// SysexAssembler: one complete message out of however many pieces arrive.
//
// ── Why reassembly is needed at all ─────────────────────────────────────────
//
// A System Exclusive message has no length field. It starts with 0xF0 and runs
// until 0xF7, and everything in between is data bytes with the top bit clear.
// How it reaches a program depends entirely on the transport:
//
//   - A serial MIDI stream delivers it a byte at a time, and REALTIME bytes
//     may appear in the middle of it. A clock byte between two data bytes is
//     legal MIDI, and a reassembler that treats it as data corrupts the
//     message.
//   - Windows multimedia hands over a buffer at a time and will split a long
//     message across several, flagged with MHDR_DONE only at the end.
//   - CoreMIDI and ALSA deliver packets that may contain a fragment, several
//     whole messages, or a fragment of one and the start of another.
//
// So every backend has the same job and, left to itself, would grow its own
// copy of it. This project has now found the same rule diverging across
// wrappers three times -- the LV2 turtle validator, the MIDI status filter,
// the denormal guard -- so the rule is written once here.
//
// ── What it refuses to do ───────────────────────────────────────────────────
//
// It never emits a partial message. A SysEx cut short is not a shorter
// message, it is a different one: the manufacturer id still parses, the
// message type still parses, and the payload is simply wrong. Silence is
// recoverable; a plausible wrong answer is not.
#pragma once

#include <cstddef>
#include <cstdint>

#include "audio.h"

namespace sonore {

/**
 * Feed it bytes; it hands back complete messages.
 *
 * Fixed capacity and no allocation, because the serial backends run this on
 * their own reader thread and the packet backends run it on a callback that
 * must not block.
 */
class SysexAssembler {
public:
  /** Matches MidiBuffer's arena, so anything this assembles can be carried. A
   *  larger buffer here would only produce messages that are then dropped. */
  static constexpr int kCapacity = MidiBuffer::kSysexCapacity;

  void reset() {
    length_ = 0;
    inMessage_ = false;
    overflowed_ = false;
  }

  /** Whether a message is currently being collected. A backend can use this to
   *  tell "no SysEx" from "a SysEx that has not finished yet". */
  bool inProgress() const { return inMessage_; }

  /** How many messages have been abandoned for want of room. Reported rather
   *  than hidden: a plugin that quietly stops answering MIDI-CI should be able
   *  to say why. */
  uint32_t dropped() const { return dropped_; }

  /**
   * One byte.
   *
   * Returns true when that byte COMPLETED a message, which is then available
   * from data()/size() until the next call to push().
   */
  bool push(uint8_t byte) {
    // Realtime bytes are not part of anything. They may be interleaved into
    // the middle of a SysEx by a device that is also sending clock, and
    // treating them as payload is how a reassembled message ends up one byte
    // longer than it should be, with 0xF8 in the middle of it.
    if (isSystemRealtime(byte)) return false;

    if (byte == 0xF0) {
      // A new message starting while one is open means the first was cut off
      // -- a cable pulled, a device reset. The truncated one is abandoned,
      // deliberately and without emitting it.
      if (inMessage_ && length_ > 0) ++dropped_;
      length_ = 0;
      inMessage_ = true;
      overflowed_ = false;
      append(byte);
      return false;
    }

    if (!inMessage_) return false; // data with no 0xF0 in front of it

    if (byte == 0xF7) {
      append(byte);
      inMessage_ = false;
      if (overflowed_) {
        // It ran past the buffer somewhere in the middle. What is held is a
        // prefix, and a prefix is not the message.
        ++dropped_;
        length_ = 0;
        overflowed_ = false;
        return false;
      }
      return length_ >= 2;
    }

    if (byte >= 0x80) {
      // Any other status byte terminates a SysEx without an 0xF7. The
      // specification allows this and devices do it; the message is over and
      // it is over UNFINISHED, so nothing is emitted.
      ++dropped_;
      length_ = 0;
      inMessage_ = false;
      overflowed_ = false;
      return false;
    }

    append(byte);
    return false;
  }

  /** Feed a run of bytes. Returns how many complete messages came out; use
   *  the callback form when there may be more than one. */
  template <typename Fn>
  int pushBlock(const uint8_t* bytes, size_t count, Fn&& onMessage) {
    int found = 0;
    for (size_t i = 0; i < count; ++i) {
      if (push(bytes[i])) {
        onMessage(data(), size());
        ++found;
      }
    }
    return found;
  }

  const uint8_t* data() const { return buffer_; }
  size_t size() const { return (size_t) length_; }

private:
  void append(uint8_t byte) {
    if (length_ >= kCapacity) {
      // Remembered rather than acted on immediately: the message still has to
      // be consumed to its terminator, or every byte after this point would
      // be mistaken for the start of a new one.
      overflowed_ = true;
      return;
    }
    buffer_[length_++] = byte;
  }

  uint8_t buffer_[kCapacity]{};
  int length_ = 0;
  bool inMessage_ = false, overflowed_ = false;
  uint32_t dropped_ = 0;
};

} // namespace sonore
