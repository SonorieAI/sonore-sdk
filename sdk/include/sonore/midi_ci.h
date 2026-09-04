// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: MIDI Capability Inquiry.
//
// How a MIDI 2.0 device and a plugin introduce themselves: who are you, what
// can you do, and, the part that is actually useful, hand me your parameter
// list so I can map my knobs to it without the user doing it by hand.
//
// This is the MESSAGE LAYER only, and deliberately so. Every CI message is a
// universal SysEx with a fixed prologue, and the bugs live there: the seven-bit
// packing (SysEx data bytes cannot have the top bit set, so anything wider has
// to be split), the MUID (a 28-bit identity carried as four seven-bit bytes),
// and the little-endian ordering of both. All of that is testable exhaustively
// with no hardware, exactly like the OSC codec.
//
// What is NOT here is the negotiation state machine, for the same reason the
// ARA binding is a hook: it can only be proven against a CI-capable device or
// host, and shipping an unverifiable state machine is how the two macOS bugs
// in this SDK's history got written.

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace sonore {
namespace midici {

/** The message types this SDK models. The numbering is the specification's. */
enum class MessageType : uint8_t {
  Discovery = 0x70,
  DiscoveryReply = 0x71,
  InvalidateMuid = 0x7E,
  Nak = 0x7F,
  PropertyCapabilitiesInquiry = 0x30,
  PropertyCapabilitiesReply = 0x31,
};

/** Broadcast destination: every device answers. */
constexpr uint32_t kBroadcastMuid = 0x0FFFFFFF;

/** The CI version this SDK speaks. */
constexpr uint8_t kCiVersion = 0x02; // MIDI-CI 1.2

/** A device's identity, as it appears in Discovery and its reply. */
struct DeviceIdentity {
  uint32_t muid = 0;                 // 28 bits: the sender's temporary identity
  uint8_t manufacturer[3] = {0, 0, 0};
  uint16_t family = 0;
  uint16_t model = 0;
  uint8_t version[4] = {0, 0, 0, 0};
  /** Bit 1 = property exchange, bit 2 = profile configuration. */
  uint8_t categories = 0;
  uint32_t maxSysExSize = 512;
};

namespace detail {

/** A 28-bit value as four SEVEN-bit bytes, least significant first. The top
 *  bit of a SysEx data byte marks the end of the message, so nothing wider
 *  than seven bits may appear literally: the single most common way a
 *  hand-written CI encoder produces a message that terminates early. */
inline void write28(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back((uint8_t) (value & 0x7f));
  out.push_back((uint8_t) ((value >> 7) & 0x7f));
  out.push_back((uint8_t) ((value >> 14) & 0x7f));
  out.push_back((uint8_t) ((value >> 21) & 0x7f));
}

inline bool read28(const uint8_t* data, size_t size, size_t& pos, uint32_t* out) {
  if (pos + 4 > size) return false;
  for (int i = 0; i < 4; ++i)
    if (data[pos + (size_t) i] & 0x80) return false; // a status byte where data belongs
  *out = (uint32_t) data[pos] | ((uint32_t) data[pos + 1] << 7) |
         ((uint32_t) data[pos + 2] << 14) | ((uint32_t) data[pos + 3] << 21);
  pos += 4;
  return true;
}

inline void write14(std::vector<uint8_t>& out, uint16_t value) {
  out.push_back((uint8_t) (value & 0x7f));
  out.push_back((uint8_t) ((value >> 7) & 0x7f));
}

inline bool read14(const uint8_t* data, size_t size, size_t& pos, uint16_t* out) {
  if (pos + 2 > size) return false;
  if ((data[pos] & 0x80) || (data[pos + 1] & 0x80)) return false;
  *out = (uint16_t) ((uint16_t) data[pos] | ((uint16_t) data[pos + 1] << 7));
  pos += 2;
  return true;
}

} // namespace detail

/** The prologue every CI message shares, written into `out`. */
inline void writeHeader(std::vector<uint8_t>& out, MessageType type, uint32_t sourceMuid,
                        uint32_t destinationMuid, uint8_t deviceId = 0x7f) {
  out.push_back(0xF0); // SysEx start
  out.push_back(0x7E); // universal non-realtime
  out.push_back(deviceId);
  out.push_back(0x0D); // MIDI-CI
  out.push_back((uint8_t) type);
  out.push_back(kCiVersion);
  detail::write28(out, sourceMuid);
  detail::write28(out, destinationMuid);
}

/** Encode a Discovery message: "who is out there, and what can you do?" */
inline std::vector<uint8_t> encodeDiscovery(const DeviceIdentity& identity) {
  std::vector<uint8_t> out;
  writeHeader(out, MessageType::Discovery, identity.muid, kBroadcastMuid);
  for (int i = 0; i < 3; ++i) out.push_back((uint8_t) (identity.manufacturer[i] & 0x7f));
  detail::write14(out, identity.family);
  detail::write14(out, identity.model);
  for (int i = 0; i < 4; ++i) out.push_back((uint8_t) (identity.version[i] & 0x7f));
  out.push_back((uint8_t) (identity.categories & 0x7f));
  detail::write28(out, identity.maxSysExSize);
  out.push_back(0x00); // output path id
  out.push_back(0xF7); // SysEx end
  return out;
}

/** Encode the reply, which carries the same identity plus a function block. */
inline std::vector<uint8_t> encodeDiscoveryReply(const DeviceIdentity& identity,
                                                 uint32_t destinationMuid) {
  std::vector<uint8_t> out;
  writeHeader(out, MessageType::DiscoveryReply, identity.muid, destinationMuid);
  for (int i = 0; i < 3; ++i) out.push_back((uint8_t) (identity.manufacturer[i] & 0x7f));
  detail::write14(out, identity.family);
  detail::write14(out, identity.model);
  for (int i = 0; i < 4; ++i) out.push_back((uint8_t) (identity.version[i] & 0x7f));
  out.push_back((uint8_t) (identity.categories & 0x7f));
  detail::write28(out, identity.maxSysExSize);
  out.push_back(0x00); // output path id
  out.push_back(0x00); // function block
  out.push_back(0xF7);
  return out;
}

/** Encode a Property Exchange capabilities inquiry: the message that leads to
 *  a controller receiving a parameter list. */
inline std::vector<uint8_t> encodePropertyCapabilities(uint32_t sourceMuid,
                                                       uint32_t destinationMuid,
                                                       uint8_t simultaneousRequests = 1) {
  std::vector<uint8_t> out;
  writeHeader(out, MessageType::PropertyCapabilitiesInquiry, sourceMuid, destinationMuid);
  out.push_back((uint8_t) (simultaneousRequests & 0x7f));
  out.push_back(0x00); // major version
  out.push_back(0x00); // minor version
  out.push_back(0xF7);
  return out;
}

/** Tell everyone a MUID is no longer valid, which is what a plugin sends when
 *  it is being torn down. */
inline std::vector<uint8_t> encodeInvalidateMuid(uint32_t sourceMuid, uint32_t targetMuid) {
  std::vector<uint8_t> out;
  writeHeader(out, MessageType::InvalidateMuid, sourceMuid, kBroadcastMuid);
  detail::write28(out, targetMuid);
  out.push_back(0xF7);
  return out;
}

/** What a parsed message carries. */
struct Message {
  MessageType type = MessageType::Nak;
  uint8_t ciVersion = 0;
  uint8_t deviceId = 0x7f;
  uint32_t sourceMuid = 0;
  uint32_t destinationMuid = 0;
  DeviceIdentity identity; // filled for Discovery and its reply
  std::vector<uint8_t> payload; // whatever followed the header, unparsed
};

/**
 * Parse a CI message. Returns false for anything that is not one, rather than
 * guessing: an ordinary SysEx from a synth must not be mistaken for a CI
 * message and acted on.
 */
inline bool decode(const uint8_t* data, size_t size, Message* out) {
  if (!data || !out || size < 15) return false;
  if (data[0] != 0xF0 || data[size - 1] != 0xF7) return false;
  if (data[1] != 0x7E || data[3] != 0x0D) return false; // not universal, not CI

  out->deviceId = data[2];
  out->type = (MessageType) data[4];
  out->ciVersion = data[5];

  size_t pos = 6;
  if (!detail::read28(data, size, pos, &out->sourceMuid)) return false;
  if (!detail::read28(data, size, pos, &out->destinationMuid)) return false;

  const size_t bodyEnd = size - 1; // drop the terminating F7
  out->payload.assign(data + pos, data + bodyEnd);

  if (out->type == MessageType::Discovery || out->type == MessageType::DiscoveryReply) {
    out->identity = DeviceIdentity{};
    out->identity.muid = out->sourceMuid;
    if (pos + 3 > bodyEnd) return false;
    for (int i = 0; i < 3; ++i) out->identity.manufacturer[i] = data[pos + (size_t) i];
    pos += 3;
    if (!detail::read14(data, bodyEnd, pos, &out->identity.family)) return false;
    if (!detail::read14(data, bodyEnd, pos, &out->identity.model)) return false;
    if (pos + 4 > bodyEnd) return false;
    for (int i = 0; i < 4; ++i) out->identity.version[i] = data[pos + (size_t) i];
    pos += 4;
    if (pos >= bodyEnd) return false;
    out->identity.categories = data[pos++];
    if (!detail::read28(data, bodyEnd, pos, &out->identity.maxSysExSize)) return false;
  }
  return true;
}

/** Is this SysEx a CI message at all? Cheap enough to call on every incoming
 *  SysEx before doing anything more expensive. */
inline bool isCiMessage(const uint8_t* data, size_t size) {
  return data && size >= 5 && data[0] == 0xF0 && data[1] == 0x7E && data[3] == 0x0D;
}

/** Build the identity a Sonorie plugin presents. Property exchange is
 *  declared because that is the capability worth having: it is how a
 *  controller asks for the parameter list and maps its knobs itself. */
inline DeviceIdentity makeIdentity(uint32_t muid, const char* manufacturerId = "\x7D\x00\x00") {
  DeviceIdentity identity;
  identity.muid = muid & 0x0FFFFFFF;
  for (int i = 0; i < 3; ++i)
    identity.manufacturer[i] = (uint8_t) (manufacturerId[i] & 0x7f);
  identity.categories = 0x02; // property exchange
  identity.maxSysExSize = 512;
  return identity;
}

} // namespace midici
} // namespace sonore
