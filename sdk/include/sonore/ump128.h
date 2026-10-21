// SPDX-License-Identifier: Apache-2.0
//
// The 128-bit Universal MIDI Packets ump.h left out: Flex Data (message type
// 0xD), UMP Stream (0xF), and SysEx8 with its Mixed Data Set (0x5).
//
// ── Why these three, and why together ───────────────────────────────────────
//
// ump.h stopped at what a host bridging a MIDI 2.0 stream hands a plugin:
// channel voice and SysEx7. That is still the common case. But a plugin that
// SPEAKS UMP -- an endpoint in its own right, which is where MIDI 2.0 goes
// once hosts pass packets through instead of flattening them -- meets all
// three of these the moment it is connected:
//
//   * UMP Stream is how an endpoint introduces itself. Before anything else
//     crosses, the other side asks "who are you, which protocol, how many
//     function blocks", and a plugin that cannot answer is not discovered.
//   * Flex Data carries what MIDI 1.0 put in a Standard MIDI File's meta
//     events: tempo, time and key signature, chord names, lyrics, the song's
//     name. A clip launched from a MIDI 2.0 sequencer brings them inline.
//   * SysEx8 is SysEx without the seven-bit tax, and the Mixed Data Set is
//     its bulk-transfer sibling. A device that has both will use them.
//
// They are one file because they share one shape: four words, a two-bit or
// four-bit "form" saying whether this packet is a whole message, its start,
// its middle or its end, and bytes packed most-significant-first into the
// words. The reassembly problem is the same for all three, so it is written
// once (detail::Reassembly) and each message type supplies only its key and
// where its bytes sit.
//
// ── Bit positions ───────────────────────────────────────────────────────────
//
// Every layout here is the field table of the UMP specification (M2-104-UM,
// version 1.1), and each encoder states the word it builds in that table's
// own terms, MSB first. Packets are arrays of host-order 32-bit words, as in
// ump.h: byte order is the transport's business, not the message's.
//
// ── Reassembly, and what it refuses ─────────────────────────────────────────
//
// The rule from ump.h's SysEx7 assembler holds: a partial message is never
// handed on. A start inside an open message abandons the old one, a continue
// or end with nothing open is an orphan from a stream joined half way, and a
// message longer than its buffer is DROPPED rather than truncated -- a
// truncated SysEx is not a smaller message, it is a different one. Every
// refusal is counted, because "it silently did nothing" is the failure an
// assembler is otherwise indistinguishable from.
//
// Interleaving is legal and handled: SysEx8 carries a stream ID precisely so
// several messages can be in flight on one group, Flex Data text on different
// channels or of different kinds is independent, and a function block's name
// can arrive between two halves of the endpoint's. Each assembler keys an
// open message by everything that distinguishes it, in a fixed pool of slots,
// so nothing here allocates except the Mixed Data Set -- see that class for
// why it is the exception.
#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "midi_ci.h"
#include "ump.h"

namespace sonore {
namespace ump {

/** One 128-bit packet, word 0 first. */
using Packet128 = std::array<uint32_t, 4>;

/** The two-bit "Format" field of Flex Data and UMP Stream messages, and the
 *  low four values of SysEx8's status nibble: the same four meanings as
 *  SysEx7's kSysexComplete..kSysexEnd. */
enum : uint8_t {
  kFormComplete = 0x0, ///< The whole message, in this one packet.
  kFormStart = 0x1,    ///< The first packet of several.
  kFormContinue = 0x2, ///< A packet from the middle.
  kFormEnd = 0x3,      ///< The last packet.
};

/**
 * Split a run of words into packets, by the size each one's type declares.
 *
 * `fn(const uint32_t* words, size_t count)` per COMPLETE packet. Returns how
 * many words were consumed; a packet cut off at the end of the run is left
 * for the caller to prepend to the next one, which is what a transport that
 * delivers in arbitrary chunks needs.
 */
template <typename Fn>
inline size_t forEachPacket(const uint32_t* words, size_t count, Fn&& fn) {
  size_t pos = 0;
  if (!words) return 0;
  while (pos < count) {
    const size_t n = (size_t) packetWords(words[pos]);
    if (n > count - pos) break;
    fn(words + pos, n);
    pos += n;
  }
  return pos;
}

namespace detail {

/** Byte `index` of a 16-byte packet, counting from the top of word 0. Every
 *  128-bit message packs its payload this way: most significant byte first,
 *  word after word. */
inline uint8_t packetByte(const uint32_t* words, size_t index) {
  return (uint8_t) ((words[index / 4] >> (24 - 8 * (index % 4))) & 0xFF);
}

inline void setPacketByte(uint32_t* words, size_t index, uint8_t value) {
  const unsigned shift = (unsigned) (24 - 8 * (index % 4));
  words[index / 4] = (words[index / 4] & ~((uint32_t) 0xFF << shift)) | ((uint32_t) value << shift);
}

/** The longest prefix of `bytes` no longer than `limit` that does not cut a
 *  UTF-8 sequence in half. A name truncated mid-character is not a shorter
 *  name; it is invalid text that a receiver may refuse whole. */
inline size_t utf8Prefix(const char* text, size_t bytes, size_t limit) {
  if (bytes <= limit) return bytes;
  size_t end = limit;
  // Back off over continuation bytes (10xxxxxx) to the start of the
  // character that would have been cut.
  while (end > 0 && (((unsigned char) text[end]) & 0xC0) == 0x80) --end;
  return end;
}

/**
 * Multi-packet reassembly, shared by every assembler in this file.
 *
 * A message in progress is a slot: its key (everything that tells two
 * concurrent messages apart), its bytes so far, and when it was last
 * touched. Slots are a fixed pool. When a new message starts and none is
 * free, the one touched least recently is abandoned -- it is the one most
 * likely to be a stream that died half way -- and counted as dropped.
 *
 * A finished message is copied out, so data() stays valid until the next
 * message completes rather than until the slot is reused, and is followed by
 * a zero byte so text can be read as a C string.
 */
template <int Capacity, int Slots>
class Reassembly {
public:
  static constexpr int kCapacity = Capacity;
  static constexpr int kSlots = Slots;

  void reset() {
    for (Slot& s : slots_) s.open = false;
    outLength_ = 0;
    out_[0] = 0;
  }

  /** One packet's bytes for message `key`. True when it completed one. */
  bool step(uint32_t key, uint8_t form, const uint8_t* bytes, size_t n) {
    ++clock_;
    int s = find(key);
    switch (form) {
      case kFormComplete:
      case kFormStart:
        if (s >= 0) {
          // A beginning inside a message that never ended: the old one is
          // gone, and the new one is not glued onto it.
          ++dropped_;
          slots_[s].open = false;
        }
        s = claim(key);
        append(slots_[s], bytes, n);
        return form == kFormComplete ? finish(s) : false;
      case kFormContinue:
      case kFormEnd:
        if (s < 0) {
          ++orphans_;
          return false;
        }
        slots_[s].lastUse = clock_;
        append(slots_[s], bytes, n);
        return form == kFormEnd ? finish(s) : false;
      default:
        return false;
    }
  }

  /** Abandon whatever is open under `key` -- a malformed packet in the middle
   *  of a message means the message is missing bytes. */
  void abandon(uint32_t key) {
    const int s = find(key);
    if (s < 0) return;
    slots_[s].open = false;
    ++dropped_;
  }

  bool inProgress() const {
    for (const Slot& s : slots_)
      if (s.open) return true;
    return false;
  }

  const uint8_t* data() const { return out_; }
  size_t size() const { return outLength_; }
  uint32_t key() const { return outKey_; }
  /** Messages lost: interrupted by a new start, overflowed, or evicted. */
  uint32_t dropped() const { return dropped_; }
  /** Continues and ends that arrived with nothing open. */
  uint32_t orphans() const { return orphans_; }

private:
  struct Slot {
    bool open = false;
    bool overflowed = false;
    uint32_t key = 0;
    uint32_t lastUse = 0;
    int length = 0;
    uint8_t bytes[Capacity];
  };

  int find(uint32_t key) const {
    for (int i = 0; i < Slots; ++i)
      if (slots_[i].open && slots_[i].key == key) return i;
    return -1;
  }

  int claim(uint32_t key) {
    int pick = -1;
    for (int i = 0; i < Slots && pick < 0; ++i)
      if (!slots_[i].open) pick = i;
    if (pick < 0) {
      pick = 0;
      for (int i = 1; i < Slots; ++i)
        if ((int32_t) (slots_[i].lastUse - slots_[pick].lastUse) < 0) pick = i;
      ++dropped_; // evicted to make room
    }
    Slot& s = slots_[pick];
    s.open = true;
    s.overflowed = false;
    s.key = key;
    s.lastUse = clock_;
    s.length = 0;
    return pick;
  }

  static void append(Slot& s, const uint8_t* bytes, size_t n) {
    for (size_t i = 0; i < n; ++i) {
      if (s.length >= Capacity) {
        s.overflowed = true;
        return;
      }
      s.bytes[s.length++] = bytes[i];
    }
  }

  bool finish(int index) {
    Slot& s = slots_[index];
    s.open = false;
    if (s.overflowed) {
      ++dropped_;
      return false;
    }
    std::memcpy(out_, s.bytes, (size_t) s.length);
    outLength_ = (size_t) s.length;
    out_[outLength_] = 0;
    outKey_ = s.key;
    return true;
  }

  Slot slots_[Slots];
  uint8_t out_[Capacity + 1] = {0};
  size_t outLength_ = 0;
  uint32_t outKey_ = 0;
  uint32_t clock_ = 0;
  uint32_t dropped_ = 0, orphans_ = 0;
};

} // namespace detail

// ═══ Flex Data (message type 0xD) ═══════════════════════════════════════════
//
// Word 0:  [mt=0xD:4][group:4][form:2][address:2][channel:4][bank:8][status:8]
// Words 1-3 carry the message.
//
// `address` says whether the message is about one CHANNEL or the whole GROUP.
// Tempo and time signature are normally sent to the group; a chord name may
// be either, because a part's harmony can differ from the song's.

/** Flex Data's address field: what the message is about. */
enum : uint8_t {
  kFlexAddressChannel = 0x0, ///< One channel, the header's `channel`.
  kFlexAddressGroup = 0x1,   ///< The whole group; `channel` is not used.
};

/** Status banks. */
enum : uint8_t {
  kFlexBankSetup = 0x00,           ///< Setup and performance: tempo, signatures, chords.
  kFlexBankMetadataText = 0x01,    ///< The song's name, composer, copyright...
  kFlexBankPerformanceText = 0x02, ///< Lyrics, ruby.
};

/** Statuses in kFlexBankSetup. */
enum : uint8_t {
  kFlexSetTempo = 0x00,         ///< setTempo().
  kFlexSetTimeSignature = 0x01, ///< setTimeSignature().
  kFlexSetMetronome = 0x02,     ///< setMetronome().
  kFlexSetKeySignature = 0x05,  ///< setKeySignature().
  kFlexSetChordName = 0x06,     ///< setChordName().
};

/** Statuses in kFlexBankMetadataText. */
enum : uint8_t {
  kMetaUnknown = 0x00,               ///< Text of no stated kind.
  kMetaProjectName = 0x01,           ///< The project's name.
  kMetaCompositionName = 0x02,       ///< The song's title.
  kMetaClipName = 0x03,              ///< The clip's name.
  kMetaCopyright = 0x04,             ///< A copyright notice.
  kMetaComposer = 0x05,              ///< Who wrote the music.
  kMetaLyricist = 0x06,              ///< Who wrote the words.
  kMetaArranger = 0x07,              ///< Who arranged it.
  kMetaPublisher = 0x08,             ///< Who publishes it.
  kMetaPrimaryPerformer = 0x09,      ///< Who performs it.
  kMetaAccompanyingPerformer = 0x0A, ///< Who accompanies.
  kMetaRecordingDate = 0x0B,         ///< When it was recorded.
  kMetaRecordingLocation = 0x0C,     ///< Where it was recorded.
};

/** Statuses in kFlexBankPerformanceText. */
enum : uint8_t {
  kPerformanceUnknown = 0x00,        ///< Text of no stated kind.
  kPerformanceLyrics = 0x01,         ///< Lyrics, as they are sung.
  kPerformanceLyricsLanguage = 0x02, ///< The lyrics' language.
  kPerformanceRuby = 0x03,           ///< Ruby: a reading printed beside the lyrics.
  kPerformanceRubyLanguage = 0x04,   ///< The ruby's language.
};

/** The note letters of a key or chord, as the spec numbers them: A is 1, G is
 *  7, and 0 is "unknown or not a standard key". */
enum : uint8_t {
  kNoteUnknown = 0x0, ///< Not said, or not a standard key.
  kNoteA = 0x1,       ///< A.
  kNoteB = 0x2,       ///< B.
  kNoteC = 0x3,       ///< C.
  kNoteD = 0x4,       ///< D.
  kNoteE = 0x5,       ///< E.
  kNoteF = 0x6,       ///< F.
  kNoteG = 0x7,       ///< G.
};

/** Word 0 of a Flex Data packet, taken apart. */
struct FlexHeader {
  uint8_t group = 0;                   ///< The UMP group, 0..15.
  uint8_t form = kFormComplete;        ///< kFormComplete .. kFormEnd: where this packet sits in its message.
  uint8_t address = kFlexAddressGroup; ///< kFlexAddressChannel or kFlexAddressGroup.
  uint8_t channel = 0;                 ///< 0..15, when the address is a channel.
  uint8_t bank = kFlexBankSetup;       ///< The status bank: kFlexBankSetup or a text bank.
  uint8_t status = 0;                  ///< Which message within the bank.
};

/** Word 0 of a Flex Data packet from `h`; each field is cut to its width. */
inline uint32_t flexWord0(const FlexHeader& h) {
  return ((uint32_t) kMtFlexData << 28) | ((uint32_t) (h.group & 0xF) << 24) |
         ((uint32_t) (h.form & 0x3) << 22) | ((uint32_t) (h.address & 0x3) << 20) |
         ((uint32_t) (h.channel & 0xF) << 16) | ((uint32_t) h.bank << 8) | (uint32_t) h.status;
}

/** Word 0 taken apart. False for anything that is not a whole Flex Data
 *  packet. */
inline bool flexHeaderOf(const uint32_t* words, size_t count, FlexHeader* out) {
  if (!words || !out || count < 4 || messageType(words[0]) != kMtFlexData) return false;
  out->group = group(words[0]);
  out->form = (uint8_t) ((words[0] >> 22) & 0x3);
  out->address = (uint8_t) ((words[0] >> 20) & 0x3);
  out->channel = (uint8_t) ((words[0] >> 16) & 0xF);
  out->bank = (uint8_t) ((words[0] >> 8) & 0xFF);
  out->status = (uint8_t) (words[0] & 0xFF);
  return true;
}

/** A Flex Data packet from its header and three data words. */
inline Packet128 flexDataFrom(const FlexHeader& h, uint32_t w1, uint32_t w2 = 0, uint32_t w3 = 0) {
  return Packet128{{flexWord0(h), w1, w2, w3}};
}

namespace detail {
inline FlexHeader setupHeader(uint8_t status, uint8_t groupIndex, uint8_t address,
                              uint8_t channel) {
  FlexHeader h;
  h.group = groupIndex;
  h.form = kFormComplete;
  h.address = address;
  h.channel = channel;
  h.bank = kFlexBankSetup;
  h.status = status;
  return h;
}

/** A single-packet setup message: whole, in bank 0, with this status. */
inline bool isSetup(const uint32_t* words, size_t count, uint8_t status, FlexHeader* h) {
  return flexHeaderOf(words, count, h) && h->form == kFormComplete && h->bank == kFlexBankSetup &&
         h->status == status;
}

/** A four-bit two's-complement field: the sharps-and-flats count of a key or
 *  chord, -8..7. */
inline int8_t signed4(uint32_t nibble) {
  const int v = (int) (nibble & 0xF);
  return (int8_t) (v >= 8 ? v - 16 : v);
}
inline uint32_t nibbleOf(int value) { return (uint32_t) value & 0xF; }
} // namespace detail

// ── Set Tempo ───────────────────────────────────────────────────────────────
//
// Word 1: the number of 10-NANOSECOND units per quarter note. A Standard
// MIDI File says microseconds; this is a hundred times finer, so a tempo
// written by a DAW to two decimal places of BPM survives the trip.

/** 120 BPM is 50,000,000. Zero for a tempo that is not positive. */
inline uint32_t tempoFromBpm(double bpm) {
  if (!(bpm > 0.0)) return 0;
  const double units = 6.0e9 / bpm;
  return units >= 4294967295.0 ? 0xFFFFFFFFu : (uint32_t) std::llround(units);
}

/** The reverse of tempoFromBpm(): 50,000,000 is 120 BPM. 0 for 0. */
inline double bpmFromTempo(uint32_t tenNanosecondsPerQuarter) {
  return tenNanosecondsPerQuarter ? 6.0e9 / (double) tenNanosecondsPerQuarter : 0.0;
}

/** A Set Tempo packet, in 10 ns units per quarter note (tempoFromBpm()),
 *  addressed to the group by default. */
inline Packet128 setTempo(uint32_t tenNanosecondsPerQuarter, uint8_t groupIndex = 0,
                          uint8_t address = kFlexAddressGroup, uint8_t channel = 0) {
  return flexDataFrom(detail::setupHeader(kFlexSetTempo, groupIndex, address, channel),
                      tenNanosecondsPerQuarter);
}

/** The tempo out of a whole Set Tempo packet, in 10 ns units per quarter
 *  note, and its header when `header` is given. False for anything else. */
inline bool decodeSetTempo(const uint32_t* words, size_t count, uint32_t* tenNanoseconds,
                           FlexHeader* header = nullptr) {
  FlexHeader h;
  if (!tenNanoseconds || !detail::isSetup(words, count, kFlexSetTempo, &h)) return false;
  *tenNanoseconds = words[1];
  if (header) *header = h;
  return true;
}

// ── Set Time Signature ──────────────────────────────────────────────────────
//
// Word 1: [numerator:8][denominator:8][number of 1/32 notes:8][reserved:8]
// The denominator is a POWER OF TWO, as in a Standard MIDI File: 2 is a
// quarter note, 3 an eighth. 6/8 is numerator 6, denominator 3.

/** A Flex Data time signature, as setTimeSignature() writes it and
 *  decodeSetTimeSignature() fills it. The denominator is held as its power
 *  of two, the way the packet carries it; denominator() gives the number a
 *  score shows. Defaults to 4/4. */
struct TimeSignature {
  uint8_t numerator = 4;        ///< Beats per bar: the 6 of 6/8.
  uint8_t denominatorPower = 2; ///< The note value is 2^-denominatorPower of a whole: 3 for 6/8.
  /** The spec's "Number of 1/32 Notes" byte, carried as it is. Eight is the
   *  usual value and what an SMF's equivalent field almost always says. */
  uint8_t thirtySecondNotes = 8;

  /** The denominator as it is written on a score: 4 for 2, 8 for 3. Zero for
   *  a power too large to be a note value. */
  int denominator() const { return denominatorPower < 16 ? 1 << denominatorPower : 0; }
};

/** A Set Time Signature packet, addressed to the group by default. */
inline Packet128 setTimeSignature(const TimeSignature& ts, uint8_t groupIndex = 0,
                                  uint8_t address = kFlexAddressGroup, uint8_t channel = 0) {
  const uint32_t w1 = ((uint32_t) ts.numerator << 24) | ((uint32_t) ts.denominatorPower << 16) |
                      ((uint32_t) ts.thirtySecondNotes << 8);
  return flexDataFrom(detail::setupHeader(kFlexSetTimeSignature, groupIndex, address, channel), w1);
}

/** The signature out of a whole Set Time Signature packet, and its header
 *  when `header` is given. False for anything else. */
inline bool decodeSetTimeSignature(const uint32_t* words, size_t count, TimeSignature* out,
                                   FlexHeader* header = nullptr) {
  FlexHeader h;
  if (!out || !detail::isSetup(words, count, kFlexSetTimeSignature, &h)) return false;
  out->numerator = (uint8_t) (words[1] >> 24);
  out->denominatorPower = (uint8_t) ((words[1] >> 16) & 0xFF);
  out->thirtySecondNotes = (uint8_t) ((words[1] >> 8) & 0xFF);
  if (header) *header = h;
  return true;
}

// ── Set Metronome ───────────────────────────────────────────────────────────
//
// Word 1: [clocks per primary click:8][bar accent 1:8][bar accent 2:8][bar accent 3:8]
// Word 2: [subdivision clicks 1:8][subdivision clicks 2:8][reserved:16]

/** A Flex Data metronome setting, field for field as the packet carries it:
 *  the primary click's spacing in MIDI clocks (24 a quarter note), the
 *  three bar-accent bytes and the two subdivision click counts, each
 *  carried as it is. */
struct Metronome {
  uint8_t clocksPerPrimaryClick = 24;    ///< MIDI clocks between primary clicks, 24 per quarter note.
  uint8_t barAccent[3] = {0, 0, 0};      ///< The spec's three bar-accent bytes, as carried.
  uint8_t subdivisionClicks[2] = {0, 0}; ///< The spec's two subdivision click counts, as carried.
};

/** A Set Metronome packet, addressed to the group by default. */
inline Packet128 setMetronome(const Metronome& m, uint8_t groupIndex = 0,
                              uint8_t address = kFlexAddressGroup, uint8_t channel = 0) {
  const uint32_t w1 = ((uint32_t) m.clocksPerPrimaryClick << 24) |
                      ((uint32_t) m.barAccent[0] << 16) | ((uint32_t) m.barAccent[1] << 8) |
                      (uint32_t) m.barAccent[2];
  const uint32_t w2 =
      ((uint32_t) m.subdivisionClicks[0] << 24) | ((uint32_t) m.subdivisionClicks[1] << 16);
  return flexDataFrom(detail::setupHeader(kFlexSetMetronome, groupIndex, address, channel), w1, w2);
}

/** The setting out of a whole Set Metronome packet, and its header when
 *  `header` is given. False for anything else. */
inline bool decodeSetMetronome(const uint32_t* words, size_t count, Metronome* out,
                               FlexHeader* header = nullptr) {
  FlexHeader h;
  if (!out || !detail::isSetup(words, count, kFlexSetMetronome, &h)) return false;
  out->clocksPerPrimaryClick = (uint8_t) (words[1] >> 24);
  out->barAccent[0] = (uint8_t) ((words[1] >> 16) & 0xFF);
  out->barAccent[1] = (uint8_t) ((words[1] >> 8) & 0xFF);
  out->barAccent[2] = (uint8_t) (words[1] & 0xFF);
  out->subdivisionClicks[0] = (uint8_t) (words[2] >> 24);
  out->subdivisionClicks[1] = (uint8_t) ((words[2] >> 16) & 0xFF);
  if (header) *header = h;
  return true;
}

// ── Set Key Signature ───────────────────────────────────────────────────────
//
// Word 1: [sharps/flats:4, signed][tonic note:4][reserved:24]
// Positive is sharps, negative is flats, and -8 means "not a standard key".

/** A Flex Data key signature: sharps (positive) or flats (negative), and the
 *  tonic as one of the kNote letters -- kNoteUnknown when the sender does not
 *  say. Both travel in four-bit fields, so a value outside them is cut to
 *  four bits on the way out. */
struct KeySignature {
  int8_t sharpsFlats = 0;       ///< Sharps positive, flats negative, -7..7; -8 for a non-standard key.
  uint8_t tonic = kNoteUnknown; ///< The tonic's letter, a kNote value.
};

/** A Set Key Signature packet, addressed to the group by default. */
inline Packet128 setKeySignature(const KeySignature& k, uint8_t groupIndex = 0,
                                 uint8_t address = kFlexAddressGroup, uint8_t channel = 0) {
  const uint32_t w1 = (detail::nibbleOf(k.sharpsFlats) << 28) | ((uint32_t) (k.tonic & 0xF) << 24);
  return flexDataFrom(detail::setupHeader(kFlexSetKeySignature, groupIndex, address, channel), w1);
}

/** The key out of a whole Set Key Signature packet, and its header when
 *  `header` is given. False for anything else. */
inline bool decodeSetKeySignature(const uint32_t* words, size_t count, KeySignature* out,
                                  FlexHeader* header = nullptr) {
  FlexHeader h;
  if (!out || !detail::isSetup(words, count, kFlexSetKeySignature, &h)) return false;
  out->sharpsFlats = detail::signed4(words[1] >> 28);
  out->tonic = (uint8_t) ((words[1] >> 24) & 0xF);
  if (header) *header = h;
  return true;
}

// ── Set Chord Name ──────────────────────────────────────────────────────────
//
// Word 1: [tonic sharps/flats:4][chord tonic:4][chord type:8]
//         [alt 1 type:4][alt 1 degree:4][alt 2 type:4][alt 2 degree:4]
// Word 2: [alt 3 type:4][alt 3 degree:4][alt 4 type:4][alt 4 degree:4][reserved:16]
// Word 3: [bass sharps/flats:4][bass note:4][bass chord type:8]
//         [bass alt 1 type:4][degree:4][bass alt 2 type:4][degree:4]
//
// The chord-type and alteration codes are the spec's tables, carried as
// numbers: a second copy of a 27-entry table here would be one more thing to
// disagree with the original.

/** One alteration of a chord -- add, subtract, raise or lower a scale
 *  degree -- as the spec's two nibbles. Type 0 is an empty slot. */
struct ChordAlteration {
  uint8_t type = 0;   ///< 0 none, 1 add, 2 subtract, 3 raise, 4 lower.
  uint8_t degree = 0; ///< The scale degree it applies to.
};

/** A Flex Data chord name, for a lead sheet or a chord track: the chord's
 *  tonic and type with up to four alterations, and a bass chord with up to
 *  two. Codes are the spec's tables, carried as numbers (see above); notes
 *  are kNote letters with a signed sharps/flats count. setChordName()
 *  addresses a channel by default, unlike the other setup messages. */
struct ChordName {
  int8_t tonicSharpsFlats = 0;        ///< The tonic's sharps (positive) or flats (negative).
  uint8_t tonic = kNoteUnknown;       ///< The tonic's letter, a kNote value.
  uint8_t chordType = 0;              ///< The spec's chord-type code; 0 for none.
  ChordAlteration alterations[4];     ///< Up to four alterations; type 0 is an empty slot.
  int8_t bassSharpsFlats = 0;         ///< The bass note's sharps or flats.
  uint8_t bassNote = kNoteUnknown;    ///< The bass note's letter; kNoteUnknown for none.
  uint8_t bassChordType = 0;          ///< The bass chord's type code.
  ChordAlteration bassAlterations[2]; ///< Up to two alterations of the bass chord.
};

namespace detail {
inline uint32_t alterationByte(const ChordAlteration& a) {
  return ((uint32_t) (a.type & 0xF) << 4) | (uint32_t) (a.degree & 0xF);
}
inline ChordAlteration alterationFrom(uint32_t byte) {
  ChordAlteration a;
  a.type = (uint8_t) ((byte >> 4) & 0xF);
  a.degree = (uint8_t) (byte & 0xF);
  return a;
}
} // namespace detail

/** A Set Chord Name packet, addressed to a CHANNEL by default. */
inline Packet128 setChordName(const ChordName& c, uint8_t groupIndex = 0,
                              uint8_t address = kFlexAddressChannel, uint8_t channel = 0) {
  const uint32_t w1 = (detail::nibbleOf(c.tonicSharpsFlats) << 28) |
                      ((uint32_t) (c.tonic & 0xF) << 24) | ((uint32_t) c.chordType << 16) |
                      (detail::alterationByte(c.alterations[0]) << 8) |
                      detail::alterationByte(c.alterations[1]);
  const uint32_t w2 = (detail::alterationByte(c.alterations[2]) << 24) |
                      (detail::alterationByte(c.alterations[3]) << 16);
  const uint32_t w3 = (detail::nibbleOf(c.bassSharpsFlats) << 28) |
                      ((uint32_t) (c.bassNote & 0xF) << 24) | ((uint32_t) c.bassChordType << 16) |
                      (detail::alterationByte(c.bassAlterations[0]) << 8) |
                      detail::alterationByte(c.bassAlterations[1]);
  return flexDataFrom(detail::setupHeader(kFlexSetChordName, groupIndex, address, channel), w1, w2,
                      w3);
}

/** The chord out of a whole Set Chord Name packet, and its header when
 *  `header` is given. False for anything else. */
inline bool decodeSetChordName(const uint32_t* words, size_t count, ChordName* out,
                               FlexHeader* header = nullptr) {
  FlexHeader h;
  if (!out || !detail::isSetup(words, count, kFlexSetChordName, &h)) return false;
  out->tonicSharpsFlats = detail::signed4(words[1] >> 28);
  out->tonic = (uint8_t) ((words[1] >> 24) & 0xF);
  out->chordType = (uint8_t) ((words[1] >> 16) & 0xFF);
  out->alterations[0] = detail::alterationFrom(words[1] >> 8);
  out->alterations[1] = detail::alterationFrom(words[1]);
  out->alterations[2] = detail::alterationFrom(words[2] >> 24);
  out->alterations[3] = detail::alterationFrom(words[2] >> 16);
  out->bassSharpsFlats = detail::signed4(words[3] >> 28);
  out->bassNote = (uint8_t) ((words[3] >> 24) & 0xF);
  out->bassChordType = (uint8_t) ((words[3] >> 16) & 0xFF);
  out->bassAlterations[0] = detail::alterationFrom(words[3] >> 8);
  out->bassAlterations[1] = detail::alterationFrom(words[3]);
  if (header) *header = h;
  return true;
}

// ── Text: metadata and lyrics ───────────────────────────────────────────────
//
// Banks 1 and 2. Words 1-3 hold twelve bytes of UTF-8, most significant byte
// first; a packet that is not full is padded with zero bytes. Longer text
// spans packets with the form field, exactly like a SysEx.

/**
 * Text as Flex Data packets. `fn(const uint32_t* words, size_t count)` per
 * packet; returns how many. Empty text is one complete packet of padding --
 * "the song's name is now nothing" is a message, not an absence of one.
 */
template <typename Fn>
inline int toFlexText(uint8_t bank, uint8_t status, const char* utf8, size_t bytes, Fn&& fn,
                      uint8_t groupIndex = 0, uint8_t address = kFlexAddressGroup,
                      uint8_t channel = 0) {
  if (!utf8) bytes = 0;
  FlexHeader h;
  h.group = groupIndex;
  h.address = address;
  h.channel = channel;
  h.bank = bank;
  h.status = status;
  int packets = 0;
  size_t sent = 0;
  do {
    const size_t take = (bytes - sent) > 12 ? 12 : (bytes - sent);
    const bool first = sent == 0, last = sent + take >= bytes;
    h.form = first ? (last ? kFormComplete : kFormStart) : (last ? kFormEnd : kFormContinue);
    uint32_t words[4] = {flexWord0(h), 0, 0, 0};
    for (size_t i = 0; i < take; ++i) detail::setPacketByte(words, 4 + i, (uint8_t) utf8[sent + i]);
    fn((const uint32_t*) words, (size_t) 4);
    ++packets;
    sent += take;
  } while (sent < bytes);
  return packets;
}

/**
 * Reassembles Flex Data text -- metadata and lyrics -- from its packets.
 *
 * Keyed by group, address, channel, bank and status: a lyric line on channel
 * 1 and the clip's name on the group are two messages, and may arrive packet
 * by packet in alternation. Setup messages (tempo and the rest) are single
 * packets with nothing to assemble; push() ignores them, so a caller can hand
 * it every Flex Data packet without sorting first.
 *
 * The zero padding is stripped per packet: a packet's text ends at its first
 * zero byte, which UTF-8 never contains as part of a character.
 */
class FlexTextAssembler {
public:
  static constexpr int kCapacity = 1024; ///< The longest text, in bytes; longer is dropped.
  static constexpr int kSlots = 8;       ///< Texts that may be open at once.

  /** Abandon whatever is open and forget the last text. The counters stay. */
  void reset() { reassembly_.reset(); }

  /** One packet. True when it completed a text message. */
  bool push(const uint32_t* words, size_t count) {
    FlexHeader h;
    if (!flexHeaderOf(words, count, &h)) return false;
    if (h.bank != kFlexBankMetadataText && h.bank != kFlexBankPerformanceText) return false;
    if (h.address > kFlexAddressGroup) return false; // reserved addresses: not a message
    uint8_t bytes[12];
    size_t n = 0;
    while (n < 12) {
      const uint8_t b = detail::packetByte(words, 4 + n);
      if (b == 0) break;
      bytes[n++] = b;
    }
    if (!reassembly_.step(keyOf(h), h.form, bytes, n)) return false;
    header_ = h;
    header_.form = kFormComplete;
    return true;
  }

  /** What the last completed message was, and its text (zero-terminated). */
  const FlexHeader& header() const { return header_; }
  /** The last text, UTF-8 and zero-terminated; valid until the next one. */
  const char* text() const { return (const char*) reassembly_.data(); }
  /** Its length in bytes, the terminator not counted. */
  size_t size() const { return reassembly_.size(); }

  /** True while any text is part way through. */
  bool inProgress() const { return reassembly_.inProgress(); }
  /** Texts lost: interrupted by a new start, too long, or evicted. */
  uint32_t dropped() const { return reassembly_.dropped(); }
  /** Continues and ends that arrived with nothing open. */
  uint32_t orphans() const { return reassembly_.orphans(); }

private:
  static uint32_t keyOf(const FlexHeader& h) {
    return ((uint32_t) h.group << 24) | ((uint32_t) h.address << 20) | ((uint32_t) h.channel << 16) |
           ((uint32_t) h.bank << 8) | (uint32_t) h.status;
  }

  detail::Reassembly<kCapacity, kSlots> reassembly_;
  FlexHeader header_;
};

// ═══ UMP Stream (message type 0xF) ══════════════════════════════════════════
//
// Word 0:  [mt=0xF:4][form:2][status:10][16 bits of message]
// No group: these are about the ENDPOINT, the whole connection, not about
// anything carried on it.

/** UMP Stream statuses: which message a packet is. */
enum : uint16_t {
  kStreamEndpointDiscovery = 0x000,         ///< "Who are you?": endpointDiscovery().
  kStreamEndpointInfo = 0x001,              ///< endpointInfoNotification().
  kStreamDeviceIdentity = 0x002,            ///< deviceIdentityNotification().
  kStreamEndpointName = 0x003,              ///< endpointNameNotification().
  kStreamProductInstanceId = 0x004,         ///< productInstanceIdNotification().
  kStreamConfigurationRequest = 0x005,      ///< streamConfigurationRequest().
  kStreamConfigurationNotification = 0x006, ///< streamConfigurationNotification().
  kStreamFunctionBlockDiscovery = 0x010,    ///< functionBlockDiscovery().
  kStreamFunctionBlockInfo = 0x011,         ///< functionBlockInfoNotification().
  kStreamFunctionBlockName = 0x012,         ///< functionBlockNameNotification().
  kStreamStartOfClip = 0x020,               ///< startOfClip().
  kStreamEndOfClip = 0x021,                 ///< endOfClip().
};

/** Endpoint Discovery's filter: which notifications the asker wants back. */
enum : uint8_t {
  kDiscoverEndpointInfo = 0x01,        ///< Send an Endpoint Info Notification.
  kDiscoverDeviceIdentity = 0x02,      ///< Send a Device Identity Notification.
  kDiscoverEndpointName = 0x04,        ///< Send the endpoint's name.
  kDiscoverProductInstanceId = 0x08,   ///< Send the product instance ID.
  kDiscoverStreamConfiguration = 0x10, ///< Send a Stream Configuration Notification.
};

/** Function Block Discovery's filter, and the block number that means all. */
enum : uint8_t {
  kDiscoverFunctionBlockInfo = 0x01, ///< Send each block's Info Notification.
  kDiscoverFunctionBlockName = 0x02, ///< Send each block's name.
  kAllFunctionBlocks = 0xFF,         ///< The block number that asks about every block.
};

/** Stream Configuration's protocol byte. */
enum : uint8_t {
  kProtocolMidi1 = 0x01, ///< MIDI 1.0 messages, carried in UMP.
  kProtocolMidi2 = 0x02, ///< MIDI 2.0 messages.
};

/** A function block's direction, UI hint, and MIDI 1.0 field. */
enum : uint8_t {
  kBlockDirectionInput = 0x1,         ///< Direction: the block receives.
  kBlockDirectionOutput = 0x2,        ///< Direction: the block sends.
  kBlockDirectionBidirectional = 0x3, ///< Direction: both.
  kBlockHintUnknown = 0x0,            ///< UI hint: not said.
  kBlockHintReceiver = 0x1,           ///< UI hint: show it as a receiver.
  kBlockHintSender = 0x2,             ///< UI hint: show it as a sender.
  kBlockHintBoth = 0x3,               ///< UI hint: show it as both.
  kBlockNotMidi1 = 0x0,               ///< MIDI 1.0 field: not a MIDI 1.0 block.
  kBlockMidi1 = 0x1,                  ///< MIDI 1.0, no bandwidth restriction.
  kBlockMidi1Restricted = 0x2,        ///< MIDI 1.0 at 31.25 kbit/s: a DIN port behind it.
};

/** The text limits the spec sets: seven packets of name, three of product
 *  instance ID, seven of function block name. */
enum : uint8_t {
  kEndpointNameMax = 98,      ///< Bytes of endpoint name: seven packets of fourteen.
  kProductInstanceIdMax = 42, ///< Bytes of product instance ID: three packets of fourteen.
  kFunctionBlockNameMax = 91, ///< Bytes of function block name: seven packets of thirteen.
};

/** A UMP Stream packet's 10-bit status, a kStream value, from its word 0. */
inline uint16_t streamStatus(uint32_t word0) { return (uint16_t) ((word0 >> 16) & 0x3FF); }
/** A UMP Stream packet's form, kFormComplete .. kFormEnd, from its word 0. */
inline uint8_t streamForm(uint32_t word0) { return (uint8_t) ((word0 >> 26) & 0x3); }

/** Word 0 of a UMP Stream packet: message type 0xF, `form`, `status` and the
 *  message's own low 16 bits. */
inline uint32_t streamWord0(uint8_t form, uint16_t status, uint16_t low16) {
  return ((uint32_t) kMtStream << 28) | ((uint32_t) (form & 0x3) << 26) |
         ((uint32_t) (status & 0x3FF) << 16) | (uint32_t) low16;
}

namespace detail {
/** A whole, single-packet stream message with this status. */
inline bool isStream(const uint32_t* words, size_t count, uint16_t status) {
  return words && count >= 4 && messageType(words[0]) == kMtStream &&
         streamStatus(words[0]) == status && streamForm(words[0]) == kFormComplete;
}
} // namespace detail

// ── Endpoint Discovery ──────────────────────────────────────────────────────
// Word 0 low: [UMP version major:8][minor:8]   Word 1: [reserved:24][filter:8]

/** Endpoint Discovery: ask an endpoint for what `filter` (kDiscover flags,
 *  ORed) names, saying this side speaks UMP `versionMajor`.`versionMinor`. */
inline Packet128 endpointDiscovery(uint8_t filter, uint8_t versionMajor = 1,
                                   uint8_t versionMinor = 1) {
  return Packet128{{streamWord0(kFormComplete, kStreamEndpointDiscovery,
                                (uint16_t) ((versionMajor << 8) | versionMinor)),
                    (uint32_t) filter, 0, 0}};
}

/** The filter, and the UMP version when asked, out of a whole Endpoint
 *  Discovery packet. False for anything else. */
inline bool decodeEndpointDiscovery(const uint32_t* words, size_t count, uint8_t* filter,
                                    uint8_t* versionMajor = nullptr,
                                    uint8_t* versionMinor = nullptr) {
  if (!filter || !detail::isStream(words, count, kStreamEndpointDiscovery)) return false;
  *filter = (uint8_t) (words[1] & 0xFF);
  if (versionMajor) *versionMajor = (uint8_t) ((words[0] >> 8) & 0xFF);
  if (versionMinor) *versionMinor = (uint8_t) (words[0] & 0xFF);
  return true;
}

// ── Endpoint Info Notification ──────────────────────────────────────────────
// Word 0 low: [UMP version major:8][minor:8]
// Word 1: [static function blocks:1][number of function blocks:7][reserved:14]
//         [MIDI 2.0:1][MIDI 1.0:1][reserved:6][receives JR:1][transmits JR:1]

/** What an endpoint says about itself in an Endpoint Info Notification --
 *  the answer to Endpoint Discovery: the UMP version it speaks, how many
 *  function blocks it has and whether they can change, which protocols it
 *  can be switched to, and its jitter-reduction timestamp support. */
struct EndpointInfo {
  uint8_t versionMajor = 1; ///< The UMP version it speaks: the 1 of 1.1.
  uint8_t versionMinor = 1; ///< ...and the 1 after the point.
  /** The blocks never change while connected, so a receiver may cache them. */
  bool staticFunctionBlocks = true;
  uint8_t numFunctionBlocks = 0;      ///< 0..32.
  bool midi2 = true;                  ///< It can be switched to the MIDI 2.0 protocol.
  bool midi1 = false;                 ///< It can be switched to the MIDI 1.0 protocol.
  bool receivesJrTimestamps = false;  ///< It takes jitter-reduction timestamps.
  bool transmitsJrTimestamps = false; ///< It sends jitter-reduction timestamps.
};

/** An Endpoint Info Notification: the answer to kDiscoverEndpointInfo. */
inline Packet128 endpointInfoNotification(const EndpointInfo& info) {
  const uint32_t w1 = ((uint32_t) (info.staticFunctionBlocks ? 1 : 0) << 31) |
                      ((uint32_t) (info.numFunctionBlocks & 0x7F) << 24) |
                      ((uint32_t) (info.midi2 ? 1 : 0) << 9) | ((uint32_t) (info.midi1 ? 1 : 0) << 8) |
                      ((uint32_t) (info.receivesJrTimestamps ? 1 : 0) << 1) |
                      (uint32_t) (info.transmitsJrTimestamps ? 1 : 0);
  return Packet128{{streamWord0(kFormComplete, kStreamEndpointInfo,
                                (uint16_t) ((info.versionMajor << 8) | info.versionMinor)),
                    w1, 0, 0}};
}

/** `out` filled from a whole Endpoint Info Notification. False for anything
 *  else. */
inline bool decodeEndpointInfo(const uint32_t* words, size_t count, EndpointInfo* out) {
  if (!out || !detail::isStream(words, count, kStreamEndpointInfo)) return false;
  out->versionMajor = (uint8_t) ((words[0] >> 8) & 0xFF);
  out->versionMinor = (uint8_t) (words[0] & 0xFF);
  out->staticFunctionBlocks = (words[1] >> 31) != 0;
  out->numFunctionBlocks = (uint8_t) ((words[1] >> 24) & 0x7F);
  out->midi2 = ((words[1] >> 9) & 1u) != 0;
  out->midi1 = ((words[1] >> 8) & 1u) != 0;
  out->receivesJrTimestamps = ((words[1] >> 1) & 1u) != 0;
  out->transmitsJrTimestamps = (words[1] & 1u) != 0;
  return true;
}

// ── Device Identity Notification ────────────────────────────────────────────
// Word 1: [reserved:8][0][manufacturer 1:7][0][manufacturer 2:7][0][manufacturer 3:7]
// Word 2: [0][family LSB:7][0][family MSB:7][0][model LSB:7][0][model MSB:7]
// Word 3: [0][version 1:7][0][version 2:7][0][version 3:7][0][version 4:7]
//
// The same identity MIDI-CI's Discovery carries, so the same struct: one
// device does not get to have two descriptions of itself. The MUID and the
// CI fields are not part of this message and are left untouched.

/** A Device Identity Notification: manufacturer, family, model and version
 *  from `id`, each byte cut to seven bits. */
inline Packet128 deviceIdentityNotification(const midici::DeviceIdentity& id) {
  const uint32_t w1 = ((uint32_t) (id.manufacturer[0] & 0x7F) << 16) |
                      ((uint32_t) (id.manufacturer[1] & 0x7F) << 8) |
                      (uint32_t) (id.manufacturer[2] & 0x7F);
  const uint32_t w2 = ((uint32_t) (id.family & 0x7F) << 24) | ((uint32_t) ((id.family >> 7) & 0x7F) << 16) |
                      ((uint32_t) (id.model & 0x7F) << 8) | (uint32_t) ((id.model >> 7) & 0x7F);
  const uint32_t w3 = ((uint32_t) (id.version[0] & 0x7F) << 24) |
                      ((uint32_t) (id.version[1] & 0x7F) << 16) |
                      ((uint32_t) (id.version[2] & 0x7F) << 8) | (uint32_t) (id.version[3] & 0x7F);
  return Packet128{{streamWord0(kFormComplete, kStreamDeviceIdentity, 0), w1, w2, w3}};
}

/** The manufacturer, family, model and version fields of `out` filled from
 *  a whole Device Identity Notification; the rest of `out` is left as it
 *  was. False for anything else. */
inline bool decodeDeviceIdentity(const uint32_t* words, size_t count, midici::DeviceIdentity* out) {
  if (!out || !detail::isStream(words, count, kStreamDeviceIdentity)) return false;
  out->manufacturer[0] = (uint8_t) ((words[1] >> 16) & 0x7F);
  out->manufacturer[1] = (uint8_t) ((words[1] >> 8) & 0x7F);
  out->manufacturer[2] = (uint8_t) (words[1] & 0x7F);
  out->family = (uint16_t) (((words[2] >> 24) & 0x7F) | (((words[2] >> 16) & 0x7F) << 7));
  out->model = (uint16_t) (((words[2] >> 8) & 0x7F) | ((words[2] & 0x7F) << 7));
  for (int i = 0; i < 4; ++i) out->version[i] = (uint8_t) ((words[3] >> (24 - 8 * i)) & 0x7F);
  return true;
}

// ── Stream Configuration Request / Notification ─────────────────────────────
// Word 0 low: [protocol:8][reserved:6][receive JR:1][transmit JR:1]

/** The protocol a stream runs (kProtocolMidi2 or the MIDI 1.0 one) and
 *  whether jitter-reduction timestamps go each way. The same fields serve
 *  the request that asks for a configuration and the notification that
 *  confirms one. */
struct StreamConfiguration {
  uint8_t protocol = kProtocolMidi2; ///< kProtocolMidi1 or kProtocolMidi2.
  bool receiveJrTimestamps = false;  ///< Jitter-reduction timestamps toward the endpoint.
  bool transmitJrTimestamps = false; ///< Jitter-reduction timestamps from it.
};

namespace detail {
inline Packet128 streamConfiguration(uint16_t status, const StreamConfiguration& c) {
  return Packet128{{streamWord0(kFormComplete, status,
                                (uint16_t) (((uint32_t) c.protocol << 8) |
                                            ((uint32_t) (c.receiveJrTimestamps ? 1 : 0) << 1) |
                                            (uint32_t) (c.transmitJrTimestamps ? 1 : 0))),
                    0, 0, 0}};
}
} // namespace detail

/** A Stream Configuration Request: ask the endpoint to switch to `c`. */
inline Packet128 streamConfigurationRequest(const StreamConfiguration& c) {
  return detail::streamConfiguration(kStreamConfigurationRequest, c);
}
/** A Stream Configuration Notification: say this side now runs `c`. */
inline Packet128 streamConfigurationNotification(const StreamConfiguration& c) {
  return detail::streamConfiguration(kStreamConfigurationNotification, c);
}

/** Either one; `isRequest` says which arrived. */
inline bool decodeStreamConfiguration(const uint32_t* words, size_t count, StreamConfiguration* out,
                                      bool* isRequest = nullptr) {
  if (!out) return false;
  const bool request = detail::isStream(words, count, kStreamConfigurationRequest);
  if (!request && !detail::isStream(words, count, kStreamConfigurationNotification)) return false;
  out->protocol = (uint8_t) ((words[0] >> 8) & 0xFF);
  out->receiveJrTimestamps = ((words[0] >> 1) & 1u) != 0;
  out->transmitJrTimestamps = (words[0] & 1u) != 0;
  if (isRequest) *isRequest = request;
  return true;
}

// ── Function Block Discovery / Info Notification ────────────────────────────
// Discovery, word 0 low: [block number:8][filter:8]
// Info, word 0 low: [active:1][block number:7][reserved:2][UI hint:2][MIDI 1.0:2][direction:2]
//       word 1: [first group:8][groups spanned:8][MIDI-CI version:8][max SysEx8 streams:8]

/** Function Block Discovery: ask about block `block` (0..31, or
 *  kAllFunctionBlocks) for what `filter` (kDiscoverFunctionBlock flags)
 *  names. */
inline Packet128 functionBlockDiscovery(uint8_t block, uint8_t filter) {
  return Packet128{{streamWord0(kFormComplete, kStreamFunctionBlockDiscovery,
                                (uint16_t) ((block << 8) | filter)),
                    0, 0, 0}};
}

/** The block and filter out of a whole Function Block Discovery packet.
 *  False for anything else. */
inline bool decodeFunctionBlockDiscovery(const uint32_t* words, size_t count, uint8_t* block,
                                         uint8_t* filter) {
  if (!block || !filter || !detail::isStream(words, count, kStreamFunctionBlockDiscovery))
    return false;
  *block = (uint8_t) ((words[0] >> 8) & 0xFF);
  *filter = (uint8_t) (words[0] & 0xFF);
  return true;
}

/** One function block, as a Function Block Info Notification describes it:
 *  its number, whether it is active, the groups it spans (firstGroup and
 *  numGroups, groups counted from 0), its direction, UI hint and MIDI 1.0
 *  role (the kBlock constants), and its MIDI-CI and SysEx8 capacity. Fields
 *  narrower than a byte are cut to their width on the way out. */
struct FunctionBlockInfo {
  bool active = true;                               ///< False for a block that is there but switched off.
  uint8_t number = 0;                               ///< 0..31.
  uint8_t uiHint = kBlockHintUnknown;               ///< A kBlockHint value.
  uint8_t midi1 = kBlockNotMidi1;                   ///< kBlockNotMidi1, kBlockMidi1 or kBlockMidi1Restricted.
  uint8_t direction = kBlockDirectionBidirectional; ///< A kBlockDirection value.
  uint8_t firstGroup = 0;                           ///< The first group it spans, from 0.
  uint8_t numGroups = 1;                            ///< How many groups it spans.
  uint8_t midiCiVersion = 0;                        ///< The MIDI-CI version it speaks; 0 for none.
  uint8_t maxSysex8Streams = 0;                     ///< SysEx8 streams it takes at once.
};

/** A Function Block Info Notification describing `b`. */
inline Packet128 functionBlockInfoNotification(const FunctionBlockInfo& b) {
  const uint16_t low = (uint16_t) (((uint32_t) (b.active ? 1 : 0) << 15) |
                                   ((uint32_t) (b.number & 0x7F) << 8) |
                                   ((uint32_t) (b.uiHint & 0x3) << 4) |
                                   ((uint32_t) (b.midi1 & 0x3) << 2) | (uint32_t) (b.direction & 0x3));
  const uint32_t w1 = ((uint32_t) b.firstGroup << 24) | ((uint32_t) b.numGroups << 16) |
                      ((uint32_t) b.midiCiVersion << 8) | (uint32_t) b.maxSysex8Streams;
  return Packet128{{streamWord0(kFormComplete, kStreamFunctionBlockInfo, low), w1, 0, 0}};
}

/** `out` filled from a whole Function Block Info Notification. False for
 *  anything else. */
inline bool decodeFunctionBlockInfo(const uint32_t* words, size_t count, FunctionBlockInfo* out) {
  if (!out || !detail::isStream(words, count, kStreamFunctionBlockInfo)) return false;
  out->active = ((words[0] >> 15) & 1u) != 0;
  out->number = (uint8_t) ((words[0] >> 8) & 0x7F);
  out->uiHint = (uint8_t) ((words[0] >> 4) & 0x3);
  out->midi1 = (uint8_t) ((words[0] >> 2) & 0x3);
  out->direction = (uint8_t) (words[0] & 0x3);
  out->firstGroup = (uint8_t) (words[1] >> 24);
  out->numGroups = (uint8_t) ((words[1] >> 16) & 0xFF);
  out->midiCiVersion = (uint8_t) ((words[1] >> 8) & 0xFF);
  out->maxSysex8Streams = (uint8_t) (words[1] & 0xFF);
  return true;
}

// ── Start / End of Clip ─────────────────────────────────────────────────────
// Nothing but the status. They bracket a clip in a file or a stream, so a
// receiver knows where the Flex Data that set up the clip ends.

/** A Start of Clip packet. */
inline Packet128 startOfClip() {
  return Packet128{{streamWord0(kFormComplete, kStreamStartOfClip, 0), 0, 0, 0}};
}
/** An End of Clip packet. */
inline Packet128 endOfClip() {
  return Packet128{{streamWord0(kFormComplete, kStreamEndOfClip, 0), 0, 0, 0}};
}
/** True for a whole Start of Clip packet. */
inline bool isStartOfClip(const uint32_t* words, size_t count) {
  return detail::isStream(words, count, kStreamStartOfClip);
}
/** True for a whole End of Clip packet. */
inline bool isEndOfClip(const uint32_t* words, size_t count) {
  return detail::isStream(words, count, kStreamEndOfClip);
}

// ── Names: endpoint, product instance, function block ───────────────────────
//
// Endpoint Name and Product Instance Id: fourteen bytes per packet, from the
// low half of word 0 through word 3. Function Block Name gives up one of them
// to say which block: [block number:8] then thirteen bytes. Padded with
// zeros, spanning packets by the form field.

namespace detail {
template <typename Fn>
inline int streamText(uint16_t status, int blockNumber, const char* utf8, size_t bytes, size_t limit,
                      Fn&& fn) {
  if (!utf8) bytes = 0;
  bytes = utf8Prefix(utf8 ? utf8 : "", bytes, limit);
  const size_t first = blockNumber >= 0 ? 3 : 2; // where text starts in the packet
  const size_t perPacket = 16 - first;
  int packets = 0;
  size_t sent = 0;
  do {
    const size_t take = (bytes - sent) > perPacket ? perPacket : (bytes - sent);
    const bool head = sent == 0, last = sent + take >= bytes;
    const uint8_t form =
        head ? (last ? kFormComplete : kFormStart) : (last ? kFormEnd : kFormContinue);
    uint32_t words[4] = {streamWord0(form, status, 0), 0, 0, 0};
    if (blockNumber >= 0) setPacketByte(words, 2, (uint8_t) blockNumber);
    for (size_t i = 0; i < take; ++i) setPacketByte(words, first + i, (uint8_t) utf8[sent + i]);
    fn((const uint32_t*) words, (size_t) 4);
    ++packets;
    sent += take;
  } while (sent < bytes);
  return packets;
}
} // namespace detail

/** The endpoint's name, at most kEndpointNameMax bytes -- longer is cut at a
 *  character boundary, not mid-character. Returns the packet count. */
template <typename Fn>
inline int endpointNameNotification(const char* utf8, size_t bytes, Fn&& fn) {
  return detail::streamText(kStreamEndpointName, -1, utf8, bytes, kEndpointNameMax, fn);
}

/** A string that tells two identical devices apart -- a serial number. */
template <typename Fn>
inline int productInstanceIdNotification(const char* utf8, size_t bytes, Fn&& fn) {
  return detail::streamText(kStreamProductInstanceId, -1, utf8, bytes, kProductInstanceIdMax, fn);
}

/** Block `block`'s name (cut to seven bits), at most kFunctionBlockNameMax
 *  bytes, cut at a character boundary. Returns the packet count. */
template <typename Fn>
inline int functionBlockNameNotification(uint8_t block, const char* utf8, size_t bytes, Fn&& fn) {
  return detail::streamText(kStreamFunctionBlockName, block & 0x7F, utf8, bytes,
                            kFunctionBlockNameMax, fn);
}

/**
 * Reassembles the three name notifications.
 *
 * Keyed by status and, for a function block's name, by the block: an
 * endpoint answering a discovery sends its own name and every block's, and a
 * sender that interleaves them is within its rights.
 */
class StreamTextAssembler {
public:
  static constexpr int kCapacity = 128; ///< Above every limit the spec sets.
  static constexpr int kSlots = 4;      ///< Names that may be open at once.

  /** Abandon whatever is open and forget the last name. The counters stay. */
  void reset() { reassembly_.reset(); }

  /** One packet. True when it completed a name. Any other stream message is
   *  ignored, so a caller can hand it every UMP Stream packet. */
  bool push(const uint32_t* words, size_t count) {
    if (!words || count < 4 || messageType(words[0]) != kMtStream) return false;
    const uint16_t status = streamStatus(words[0]);
    size_t first = 2;
    uint32_t block = 0;
    if (status == kStreamFunctionBlockName) {
      first = 3;
      block = detail::packetByte(words, 2);
    } else if (status != kStreamEndpointName && status != kStreamProductInstanceId) {
      return false;
    }
    uint8_t bytes[14];
    size_t n = 0;
    while (first + n < 16) {
      const uint8_t b = detail::packetByte(words, first + n);
      if (b == 0) break;
      bytes[n] = b;
      ++n;
    }
    if (!reassembly_.step(((uint32_t) status << 8) | block, streamForm(words[0]), bytes, n))
      return false;
    status_ = status;
    block_ = (uint8_t) block;
    return true;
  }

  /** kStreamEndpointName, kStreamProductInstanceId or kStreamFunctionBlockName. */
  uint16_t status() const { return status_; }
  /** Which block, when status() is kStreamFunctionBlockName. */
  uint8_t functionBlock() const { return block_; }
  /** The last name, UTF-8 and zero-terminated; valid until the next one. */
  const char* text() const { return (const char*) reassembly_.data(); }
  /** Its length in bytes, the terminator not counted. */
  size_t size() const { return reassembly_.size(); }

  /** True while any name is part way through. */
  bool inProgress() const { return reassembly_.inProgress(); }
  /** Names lost: interrupted by a new start, too long, or evicted. */
  uint32_t dropped() const { return reassembly_.dropped(); }
  /** Continues and ends that arrived with nothing open. */
  uint32_t orphans() const { return reassembly_.orphans(); }

private:
  detail::Reassembly<kCapacity, kSlots> reassembly_;
  uint16_t status_ = 0;
  uint8_t block_ = 0;
};

// ═══ SysEx8 and Mixed Data Set (message type 0x5) ═══════════════════════════
//
// SysEx8:  [mt=5:4][group:4][status:4][number of bytes:4][stream ID:8][data 1:8]
//          words 1-3: data 2..13
// The byte count INCLUDES the stream ID, so it runs 1..14 and a packet
// carries at most thirteen data bytes. Unlike SysEx7 the bytes are full
// eight-bit values, and there is no 0xF0/0xF7 framing at all -- nothing here
// adds it back, because no MIDI 1.0 consumer can read an eight-bit SysEx.
//
// The stream ID is what lets several SysEx8 messages be in flight on one
// group at once; the assembler keys by group AND stream for that reason.

/** The status nibble of the two Mixed Data Set packets, beside SysEx8's
 *  four forms. */
enum : uint8_t {
  kMixedDataSetHeader = 0x8,  ///< A chunk's header: mixedDataSetHeader().
  kMixedDataSetPayload = 0x9, ///< Fourteen bytes of the chunk's data.
};

/**
 * Bytes as SysEx8 packets. `data` is the payload -- manufacturer ID onward;
 * there is no framing to strip. `fn(const uint32_t* words, size_t count)` per
 * packet; returns how many. An empty message is one complete packet carrying
 * only its stream ID.
 */
template <typename Fn>
inline int toSysex8(const uint8_t* data, size_t bytes, Fn&& fn, uint8_t streamId = 0,
                    uint8_t groupIndex = 0) {
  if (!data) bytes = 0;
  int packets = 0;
  size_t sent = 0;
  do {
    const size_t take = (bytes - sent) > 13 ? 13 : (bytes - sent);
    const bool first = sent == 0, last = sent + take >= bytes;
    const uint8_t status =
        first ? (last ? kFormComplete : kFormStart) : (last ? kFormEnd : kFormContinue);
    uint32_t words[4] = {((uint32_t) kMtData128 << 28) | ((uint32_t) (groupIndex & 0xF) << 24) |
                             ((uint32_t) status << 20) | ((uint32_t) (take + 1) << 16) |
                             ((uint32_t) streamId << 8),
                         0, 0, 0};
    for (size_t i = 0; i < take; ++i) detail::setPacketByte(words, 3 + i, data[sent + i]);
    fn((const uint32_t*) words, (size_t) 4);
    ++packets;
    sent += take;
  } while (sent < bytes);
  return packets;
}

/**
 * Reassembles SysEx8 from its packets, several streams at once.
 *
 * A packet whose byte count is 0 (not even the stream ID) or above 14 is
 * malformed. If it claimed to continue or end a message that is open, that
 * message is now missing bytes, so it is abandoned rather than finished with
 * a hole in it.
 */
class Sysex8Assembler {
public:
  static constexpr int kCapacity = MidiBuffer::kSysexCapacity; ///< The longest message, in bytes; longer is dropped.
  static constexpr int kSlots = 8; ///< Messages (group and stream) that may be open at once.

  /** Abandon whatever is open and forget the last message. The counters
   *  stay. */
  void reset() { reassembly_.reset(); }

  /** One packet. True when it completed a message. Mixed Data Set packets
   *  share the message type and are ignored here: see MixedDataSetAssembler. */
  bool push(const uint32_t* words, size_t count) {
    if (!words || count < 4 || messageType(words[0]) != kMtData128) return false;
    const uint8_t status = (uint8_t) ((words[0] >> 20) & 0xF);
    if (status > kFormEnd) return false;
    const uint8_t numBytes = (uint8_t) ((words[0] >> 16) & 0xF);
    const uint8_t streamId = detail::packetByte(words, 2);
    const uint32_t key = ((uint32_t) ump::group(words[0]) << 8) | streamId;
    if (numBytes == 0 || numBytes > 14) {
      ++malformed_;
      if (status == kFormContinue || status == kFormEnd) reassembly_.abandon(key);
      return false;
    }
    uint8_t bytes[13];
    for (uint8_t i = 0; i + 1 < numBytes; ++i) bytes[i] = detail::packetByte(words, 3u + i);
    return reassembly_.step(key, status, bytes, (size_t) (numBytes - 1));
  }

  /** The last completed message: its data bytes (no stream ID), its group
   *  and its stream. Valid until the next message completes. */
  const uint8_t* data() const { return reassembly_.data(); }
  size_t size() const { return reassembly_.size(); } ///< The last message's length in bytes.
  uint8_t group() const { return (uint8_t) ((reassembly_.key() >> 8) & 0xF); } ///< Its group, 0..15.
  uint8_t streamId() const { return (uint8_t) (reassembly_.key() & 0xFF); } ///< Its stream ID.

  /** True while any message is part way through. */
  bool inProgress() const { return reassembly_.inProgress(); }
  /** Messages lost: interrupted by a new start, too long, evicted, or
   *  abandoned after a malformed packet. */
  uint32_t dropped() const { return reassembly_.dropped(); }
  /** Continues and ends that arrived with nothing open. */
  uint32_t orphans() const { return reassembly_.orphans(); }
  /** Packets refused for a byte count of 0 or above 14. */
  uint32_t malformed() const { return malformed_; }

private:
  detail::Reassembly<kCapacity, kSlots> reassembly_;
  uint32_t malformed_ = 0;
};

// ── Mixed Data Set ──────────────────────────────────────────────────────────
//
// Header:  [mt=5:4][group:4][status=8:4][MDS ID:4][valid bytes in this chunk:16]
//          word 1: [number of chunks:16][number of this chunk:16]
//          word 2: [manufacturer ID:16][device ID:16]
//          word 3: [sub ID 1:16][sub ID 2:16]
// Payload: [mt=5:4][group:4][status=9:4][MDS ID:4][data 1-2:16]
//          words 1-3: data 3..14
//
// A data set is sent as CHUNKS, each one header followed by its payloads, and
// up to sixteen data sets (the MDS ID) can be in flight per group. This file
// reads the header's byte count as the bytes its chunk's PAYLOADS carry --
// fourteen per payload, the last one zero-padded -- which is what tells the
// receiver where the chunk ends and how much of its final packet is data.
// A chunk count of 0 means the sender did not know how many there would be.

/** A Mixed Data Set chunk header, field for field: which group and data set
 *  (mdsId) it belongs to, how many payload bytes the chunk carries, its place
 *  among the set's chunks, and the manufacturer, device and sub IDs that say
 *  what the data is. Written by mixedDataSetHeader(), and filled for each
 *  completed chunk by MixedDataSetAssembler. */
struct MixedDataSetHeader {
  uint8_t group = 0;           ///< The UMP group, 0..15.
  uint8_t mdsId = 0;           ///< Which data set, 0..15.
  uint16_t validBytes = 0;     ///< Bytes in this chunk's payloads.
  uint16_t numChunks = 1;      ///< Chunks in the set; 0 for unknown.
  uint16_t chunkNumber = 1;    ///< This chunk's place among them.
  uint16_t manufacturerId = 0; ///< Whose data it is.
  uint16_t deviceId = 0;       ///< Which device it is for.
  uint16_t subId1 = 0;         ///< What the data is, first word.
  uint16_t subId2 = 0;         ///< ...and second.
};

/** A Mixed Data Set chunk header from `h`, as it is: toMixedDataSetChunk()
 *  sets validBytes for a caller sending a whole chunk. */
inline Packet128 mixedDataSetHeader(const MixedDataSetHeader& h) {
  return Packet128{{((uint32_t) kMtData128 << 28) | ((uint32_t) (h.group & 0xF) << 24) |
                        ((uint32_t) kMixedDataSetHeader << 20) | ((uint32_t) (h.mdsId & 0xF) << 16) |
                        (uint32_t) h.validBytes,
                    ((uint32_t) h.numChunks << 16) | (uint32_t) h.chunkNumber,
                    ((uint32_t) h.manufacturerId << 16) | (uint32_t) h.deviceId,
                    ((uint32_t) h.subId1 << 16) | (uint32_t) h.subId2}};
}

/** `out` filled from a Mixed Data Set header packet. False for anything
 *  else. */
inline bool decodeMixedDataSetHeader(const uint32_t* words, size_t count, MixedDataSetHeader* out) {
  if (!out || !words || count < 4 || messageType(words[0]) != kMtData128 ||
      ((words[0] >> 20) & 0xF) != kMixedDataSetHeader)
    return false;
  out->group = group(words[0]);
  out->mdsId = (uint8_t) ((words[0] >> 16) & 0xF);
  out->validBytes = (uint16_t) (words[0] & 0xFFFF);
  out->numChunks = (uint16_t) (words[1] >> 16);
  out->chunkNumber = (uint16_t) (words[1] & 0xFFFF);
  out->manufacturerId = (uint16_t) (words[2] >> 16);
  out->deviceId = (uint16_t) (words[2] & 0xFFFF);
  out->subId1 = (uint16_t) (words[3] >> 16);
  out->subId2 = (uint16_t) (words[3] & 0xFFFF);
  return true;
}

/**
 * One chunk: its header, then its payloads. The header's validBytes is set
 * from `bytes`; more than 65535 is refused (0 packets), because the field
 * cannot say it -- a longer set is several chunks, and which way to split it
 * is the sender's decision.
 */
template <typename Fn>
inline int toMixedDataSetChunk(MixedDataSetHeader header, const uint8_t* data, size_t bytes,
                               Fn&& fn) {
  if (!data) bytes = 0;
  if (bytes > 0xFFFF) return 0;
  header.validBytes = (uint16_t) bytes;
  const Packet128 head = mixedDataSetHeader(header);
  fn(head.data(), (size_t) 4);
  int packets = 1;
  for (size_t sent = 0; sent < bytes; sent += 14) {
    uint32_t words[4] = {((uint32_t) kMtData128 << 28) | ((uint32_t) (header.group & 0xF) << 24) |
                             ((uint32_t) kMixedDataSetPayload << 20) |
                             ((uint32_t) (header.mdsId & 0xF) << 16),
                         0, 0, 0};
    const size_t take = (bytes - sent) > 14 ? 14 : (bytes - sent);
    for (size_t i = 0; i < take; ++i) detail::setPacketByte(words, 2 + i, data[sent + i]);
    fn((const uint32_t*) words, (size_t) 4);
    ++packets;
  }
  return packets;
}

/**
 * Reassembles Mixed Data Set chunks, one per group and MDS ID at a time.
 *
 * The one assembler here that allocates. A chunk may be 65535 bytes and 256
 * may be open at once; a fixed pool for that is sixteen megabytes held
 * against data that is almost never sent. So each open chunk reserves what
 * its header declares -- once, on the header, never while payloads arrive --
 * and a Mixed Data Set is not something to assemble on the audio thread. It
 * is a bulk transfer: a sample, a firmware image, a file.
 *
 * Delivered per CHUNK. Putting the chunks of a set back together is the
 * caller's, because only the caller knows whether chunk numbers it never saw
 * mean a gap or a sender that numbers from somewhere else.
 */
class MixedDataSetAssembler {
public:
  /** Abandon every open chunk and forget the last one. The counters stay. */
  void reset() {
    for (Open& o : open_) {
      o.active = false;
      o.bytes.clear();
    }
    done_.clear();
  }

  /** One packet. True when it completed a chunk. */
  bool push(const uint32_t* words, size_t count) {
    if (!words || count < 4 || messageType(words[0]) != kMtData128) return false;
    const uint8_t status = (uint8_t) ((words[0] >> 20) & 0xF);
    const size_t slot = ((size_t) ump::group(words[0]) << 4) | ((words[0] >> 16) & 0xF);
    Open& o = open_[slot];

    if (status == kMixedDataSetHeader) {
      if (o.active) ++dropped_; // a header inside an unfinished chunk
      decodeMixedDataSetHeader(words, count, &o.header);
      o.active = true;
      o.bytes.clear();
      o.bytes.reserve(o.header.validBytes);
      return o.header.validBytes == 0 ? finish(o) : false;
    }
    if (status != kMixedDataSetPayload) return false;
    if (!o.active) {
      ++orphans_;
      return false;
    }
    const size_t want = (size_t) o.header.validBytes - o.bytes.size();
    const size_t take = want > 14 ? 14 : want;
    for (size_t i = 0; i < take; ++i) o.bytes.push_back(detail::packetByte(words, 2 + i));
    return o.bytes.size() >= o.header.validBytes ? finish(o) : false;
  }

  /** The last completed chunk's header. */
  const MixedDataSetHeader& header() const { return doneHeader_; }
  /** Its payload bytes; valid until the next chunk completes. */
  const uint8_t* data() const { return done_.data(); }
  /** How many: the header's validBytes. */
  size_t size() const { return done_.size(); }

  /** True while any chunk is part way through. */
  bool inProgress() const {
    for (const Open& o : open_)
      if (o.active) return true;
    return false;
  }
  /** Chunks lost to a header arriving before they finished. */
  uint32_t dropped() const { return dropped_; }
  /** Payloads that arrived with no chunk open. */
  uint32_t orphans() const { return orphans_; }

private:
  /** One group and data set's chunk in progress: its header, and the payload
   *  bytes so far, up to the header's validBytes. A header arriving while one
   *  is active abandons it and counts a drop. */
  struct Open {
    bool active = false;        ///< A header has arrived and its bytes have not.
    MixedDataSetHeader header;  ///< That header.
    std::vector<uint8_t> bytes; ///< The payload so far.
  };

  bool finish(Open& o) {
    o.active = false;
    doneHeader_ = o.header;
    done_.swap(o.bytes); // the finished chunk's storage becomes the next one's
    o.bytes.clear();
    return true;
  }

  Open open_[16 * 16];
  MixedDataSetHeader doneHeader_;
  std::vector<uint8_t> done_;
  uint32_t dropped_ = 0, orphans_ = 0;
};

} // namespace ump
} // namespace sonore
