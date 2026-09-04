// SPDX-License-Identifier: Apache-2.0
//
// Universal MIDI Packets: the MIDI 2.0 wire format.
//
// ── What a UMP is ───────────────────────────────────────────────────────────
//
// MIDI 1.0 is a byte stream where the length of a message is worked out from
// its status byte. MIDI 2.0 is a stream of 32-bit WORDS, and how many words a
// message occupies is read from the top nibble of the first one. That single
// change is what lets it carry a 32-bit controller value, sixteen groups of
// sixteen channels, and per-note controllers, without the receiver ever
// guessing.
//
// This SDK already speaks the negotiation half of MIDI 2.0: midi_ci.h does
// discovery, profiles and property exchange. It could not speak the transport
// half at all, which is a strange place to stop -- a plugin could agree with a
// controller about what it supports and then have no way to receive it.
//
// ── What is here, and what is deliberately not ──────────────────────────────
//
// Packet SIZING, MIDI 1.0 channel voice both directions, MIDI 2.0 channel
// voice down to MidiMessage, and SysEx7 assembly. That is the set a plugin
// actually meets: a host translating a MIDI 2.0 stream sends channel voice
// and SysEx, and everything else in the specification is stream configuration
// the host handles before the plugin sees anything.
//
// Not here: Flex Data (score metadata), UMP Stream (endpoint discovery), and
// SysEx8. Named in the feature map rather than half-written.
//
// ── On losing precision, and saying so ──────────────────────────────────────
//
// MIDI 2.0 velocity is 16 bits and MidiMessage carries 7. Converting down
// LOSES information, and every function here that does it says so in its name
// or its comment. The alternative -- widening MidiMessage - would change the
// type every DSP in this SDK is written against, for a resolution no
// generated plugin has asked for yet. When one does, this is the file that
// records what was given up.
#pragma once

#include <cstddef>
#include <cstdint>

#include "audio.h"

namespace sonore {
namespace ump {

/** The message type, from the top nibble of the first word. */
enum : uint8_t {
  kMtUtility = 0x0,
  kMtSystem = 0x1,
  kMtMidi1ChannelVoice = 0x2,
  kMtSysex7 = 0x3,
  kMtMidi2ChannelVoice = 0x4,
  kMtData128 = 0x5,
  kMtFlexData = 0xD,
  kMtStream = 0xF,
};

/** SysEx7 status, in the second nibble of the first word. */
enum : uint8_t {
  kSysexComplete = 0x0,
  kSysexStart = 0x1,
  kSysexContinue = 0x2,
  kSysexEnd = 0x3,
};

inline uint8_t messageType(uint32_t word0) { return (uint8_t) ((word0 >> 28) & 0xF); }
inline uint8_t group(uint32_t word0) { return (uint8_t) ((word0 >> 24) & 0xF); }

/**
 * How many 32-bit words this packet occupies, read from its type.
 *
 * The whole point of the format: a receiver that does not understand a
 * message can still skip exactly the right distance to the next one. A reader
 * that guessed would desynchronise on the first unfamiliar packet and never
 * recover -- which is the failure MIDI 1.0's running status is famous for.
 *
 * The reserved ranges have DEFINED sizes for that reason, and they are
 * honoured here even though nothing uses them yet.
 */
inline int packetWords(uint32_t word0) {
  switch (messageType(word0)) {
    case 0x0: case 0x1: case 0x2: case 0x6: case 0x7: return 1;
    case 0x3: case 0x4: case 0x8: case 0x9: case 0xA: return 2;
    case 0xB: case 0xC: return 3;
    default: return 4; // 0x5, 0xD, 0xE, 0xF
  }
}

// ── MIDI 1.0 channel voice, both directions ─────────────────────────────────
//
// The bridge every host uses today: a DAW that speaks UMP internally still
// hands a MIDI 1.0 plugin its notes as message type 2.

/** One MidiMessage as a MIDI 1.0 channel voice packet. */
inline uint32_t fromMidi1(const MidiMessage& m, uint8_t groupIndex = 0) {
  const uint32_t status = (uint32_t) (m.getRawStatus() & 0xF0);
  const uint32_t channel = (uint32_t) (m.getRawStatus() & 0x0F);
  return ((uint32_t) kMtMidi1ChannelVoice << 28) | ((uint32_t) (groupIndex & 0xF) << 24) |
         (status << 16) | (channel << 16) | ((uint32_t) (m.getRawData1() & 0x7F) << 8) |
         (uint32_t) (m.getRawData2() & 0x7F);
}

/**
 * And back. Returns false for a packet that is not MIDI 1.0 channel voice,
 * rather than inventing a message out of one that is not.
 *
 * `groupOut` is where the SECOND thing this conversion loses goes.
 *
 * A UMP stream carries sixteen GROUPS of sixteen channels -- 256 addressable
 * voices -- and MidiMessage has room for sixteen. So two instruments arriving
 * on groups 0 and 1 collapse onto the same channels and play each other's
 * notes, silently, with nothing in the message to say it happened.
 *
 * That is a larger loss than the velocity narrowing below, and it went
 * undocumented in the first version of this file while the velocity one was
 * described at length. A caller that cares passes a pointer and keeps the
 * group; a caller on a single-group stream, which is what a host bridging
 * MIDI 1.0 sends, passes nothing and loses nothing.
 */
inline bool toMidi1(uint32_t word0, MidiMessage* out, uint8_t* groupOut = nullptr) {
  if (!out || messageType(word0) != kMtMidi1ChannelVoice) return false;
  if (groupOut) *groupOut = group(word0);
  const int status = (int) ((word0 >> 16) & 0xFF);
  *out = MidiMessage(status, (int) ((word0 >> 8) & 0x7F), (int) (word0 & 0x7F));
  return true;
}

/** A system message -- clock, start, stop -- as message type 1. */
inline uint32_t fromSystem(int status, int d1 = 0, int d2 = 0, uint8_t groupIndex = 0) {
  return ((uint32_t) kMtSystem << 28) | ((uint32_t) (groupIndex & 0xF) << 24) |
         ((uint32_t) (status & 0xFF) << 16) | ((uint32_t) (d1 & 0x7F) << 8) |
         (uint32_t) (d2 & 0x7F);
}

inline bool toSystem(uint32_t word0, MidiMessage* out, uint8_t* groupOut = nullptr) {
  if (!out || messageType(word0) != kMtSystem) return false;
  if (groupOut) *groupOut = group(word0);
  *out = MidiMessage((int) ((word0 >> 16) & 0xFF), (int) ((word0 >> 8) & 0x7F),
                     (int) (word0 & 0x7F));
  return true;
}

// ── MIDI 2.0 channel voice, narrowed ────────────────────────────────────────

/**
 * A MIDI 2.0 channel voice packet as a MidiMessage, LOSING RESOLUTION.
 *
 * Velocity arrives as 16 bits and leaves as 7. Controller values arrive as 32
 * and leave as 7. That is not a rounding detail to be discovered later, which
 * is why it is in the function's name.
 *
 * The GROUP is lost too unless `groupOut` is passed -- see toMidi1 above for
 * why that matters more than the velocity does.
 *
 * The scaling is a shift, not a divide-and-round: MIDI 2.0's own conversion
 * tables are defined that way, so a value that came UP from MIDI 1.0 and goes
 * back down again lands on exactly where it started. Rounding would move it
 * by one at half the possible inputs.
 */
inline bool toMidi1Narrowing(const uint32_t* words, size_t count, MidiMessage* out,
                             uint8_t* groupOut = nullptr) {
  if (!out || !words || count < 2 || messageType(words[0]) != kMtMidi2ChannelVoice) return false;
  if (groupOut) *groupOut = group(words[0]);
  const int status = (int) ((words[0] >> 20) & 0x0F) << 4;
  const int channel = (int) ((words[0] >> 16) & 0x0F);
  const int index = (int) ((words[0] >> 8) & 0x7F);
  const uint32_t value = words[1];

  switch (status) {
    case 0x90: // note on
    case 0x80: { // note off
      const int velocity = (int) ((value >> 25) & 0x7F); // top 7 of the 16-bit field
      *out = MidiMessage(status | channel, index, velocity);
      return true;
    }
    case 0xB0: // control change: a 32-bit value down to 7 bits
      *out = MidiMessage(status | channel, index, (int) (value >> 25));
      return true;
    case 0xD0: // channel pressure
      *out = MidiMessage(status | channel, (int) (value >> 25), 0);
      return true;
    case 0xE0: { // pitch bend: 32 bits down to 14
      const uint32_t bend14 = value >> 18;
      *out = MidiMessage::pitchBend(channel, (int) bend14);
      return true;
    }
    default:
      // Per-note controllers, per-note pitch, registered controllers: real
      // MIDI 2.0 messages with no MIDI 1.0 spelling at all. Refused rather
      // than approximated -- a per-note pitch bend flattened onto the channel
      // would bend every note that was sounding.
      return false;
  }
}

// ── SysEx7 ──────────────────────────────────────────────────────────────────

/**
 * Assembles a SysEx from the 64-bit packets that carry it.
 *
 * UMP does not put a SysEx on the wire as bytes. It carries up to six data
 * bytes per packet with a status nibble saying whether this is the whole
 * thing, the start, the middle or the end -- so the reassembly is a different
 * problem from the serial one in sysex.h, and shares only the rule that a
 * partial message is never handed on.
 *
 * The bytes carried are the PAYLOAD: the 0xF0 and 0xF7 that delimit a MIDI 1.0
 * SysEx are not transmitted. They are added back here, because every consumer
 * in this SDK -- midi_ci.h included -- expects a complete MIDI 1.0 message.
 */
class Sysex7Assembler {
public:
  static constexpr int kCapacity = MidiBuffer::kSysexCapacity;

  void reset() {
    length_ = 0;
    inMessage_ = false;
    overflowed_ = false;
  }

  bool inProgress() const { return inMessage_; }
  uint32_t dropped() const { return dropped_; }
  const uint8_t* data() const { return buffer_; }
  size_t size() const { return (size_t) length_; }

  /** One packet. True when it completed a message. */
  bool push(const uint32_t* words, size_t count) {
    if (!words || count < 2 || messageType(words[0]) != kMtSysex7) return false;
    const uint8_t status = (uint8_t) ((words[0] >> 20) & 0xF);
    const uint8_t numBytes = (uint8_t) ((words[0] >> 16) & 0xF);
    if (numBytes > 6) return false; // six is the maximum a packet can hold

    if (status == kSysexComplete || status == kSysexStart) {
      if (inMessage_ && length_ > 1) ++dropped_; // a start inside a message
      length_ = 0;
      overflowed_ = false;
      inMessage_ = true;
      append(0xF0);
    } else if (!inMessage_) {
      // A continue or an end with no start in front of it. The message began
      // before this stream was joined, and half of one is not a message.
      return false;
    }

    for (uint8_t i = 0; i < numBytes; ++i) append(byteAt(words, i));

    if (status == kSysexComplete || status == kSysexEnd) {
      append(0xF7);
      inMessage_ = false;
      if (overflowed_) {
        ++dropped_;
        length_ = 0;
        overflowed_ = false;
        return false;
      }
      return length_ >= 2;
    }
    return false;
  }

private:
  /** Data byte i of a SysEx7 packet: two in the first word, four in the
   *  second, all in the low seven bits of their byte. */
  static uint8_t byteAt(const uint32_t* words, uint8_t i) {
    if (i < 2) return (uint8_t) ((words[0] >> (8 * (1 - i))) & 0x7F);
    return (uint8_t) ((words[1] >> (8 * (3 - (i - 2)))) & 0x7F);
  }

  void append(uint8_t byte) {
    if (length_ >= kCapacity) {
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

/**
 * A complete MIDI 1.0 SysEx as SysEx7 packets.
 *
 * `fn(const uint32_t* words, size_t count)` is called once per packet. The
 * 0xF0 and 0xF7 are STRIPPED: they are framing for a byte stream and the
 * status nibble carries the same information here.
 */
template <typename Fn>
inline int toSysex7(const uint8_t* message, size_t bytes, Fn&& fn, uint8_t groupIndex = 0) {
  if (!message || bytes < 2 || message[0] != 0xF0 || message[bytes - 1] != 0xF7) return 0;
  const uint8_t* payload = message + 1;
  const size_t total = bytes - 2;

  int packets = 0;
  size_t sent = 0;
  do {
    const size_t take = (total - sent) > 6 ? 6 : (total - sent);
    const bool first = sent == 0;
    const bool last = sent + take >= total;
    const uint8_t status = first ? (last ? kSysexComplete : kSysexStart)
                                 : (last ? kSysexEnd : kSysexContinue);
    uint32_t words[2] = {0, 0};
    words[0] = ((uint32_t) kMtSysex7 << 28) | ((uint32_t) (groupIndex & 0xF) << 24) |
               ((uint32_t) status << 20) | ((uint32_t) take << 16);
    for (size_t i = 0; i < take; ++i) {
      const uint32_t b = (uint32_t) (payload[sent + i] & 0x7F);
      if (i < 2) words[0] |= b << (8 * (1 - i));
      else words[1] |= b << (8 * (3 - (i - 2)));
    }
    fn(words, (size_t) 2);
    ++packets;
    sent += take;
  } while (sent < total);
  return packets;
}

} // namespace ump
} // namespace sonore
