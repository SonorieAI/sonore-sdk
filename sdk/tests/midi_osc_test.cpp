// SPDX-License-Identifier: Apache-2.0
// Sonore SDK -- MIDI 2.0's 128-bit packets, the timestamped MIDI collector,
// OSC address patterns and colours, and the MPE keyboard.
//
// The UMP packets are checked BIT-EXACTLY against words worked out by hand
// from the field tables of the UMP specification (M2-104-UM v1.1): each
// expected word below is written as the fields that make it up, so a wrong
// shift shows up as a wrong hex digit in a known place. The OSC pattern table
// follows the rules of OSC 1.0's "OSC Message Dispatching and Pattern
// Matching", case by case, with the negative cases each rule implies.
//
// The collector's lock-free claim is tested two ways: the drain is run with
// global operator new counting, and three producer threads race a consumer
// with nothing lost or reordered.
#include <sonore/gfx/bitmap.h>
#include <sonore/gfx/graphics.h>
#include <sonore/gfx/mpe_keyboard.h>
#include <sonore/midi_collector.h>
#include <sonore/osc.h>
#include <sonore/ump128.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <thread>
#include <vector>

using namespace sonore;

// ── Allocation counter, armed around the audio-thread call ─────────────────
static std::atomic<bool> g_armed{false};
static std::atomic<long> g_allocations{0};

void* operator new(std::size_t n) {
  if (g_armed.load(std::memory_order_relaxed)) g_allocations.fetch_add(1);
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

static int g_checks = 0;
static int g_failures = 0;

static void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

static bool samePacket(const uint32_t* got, uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3) {
  const bool ok = got[0] == w0 && got[1] == w1 && got[2] == w2 && got[3] == w3;
  if (!ok)
    std::printf("       got %08X %08X %08X %08X, want %08X %08X %08X %08X\n", got[0], got[1],
                got[2], got[3], w0, w1, w2, w3);
  return ok;
}

struct Packets {
  std::vector<std::vector<uint32_t>> list;
  void operator()(const uint32_t* w, size_t n) { list.emplace_back(w, w + n); }
};

// ═══ Flex Data ═══════════════════════════════════════════════════════════════

static void flexData() {
  std::printf("\n[UMP Flex Data]\n");
  using namespace ump;

  // Set Tempo, group 0, addressed to the group (address 1), channel 0:
  //   [D][0][form 00 | addr 01 | chan 0000 = 0x10][bank 00][status 00]
  //   word 1: 120 BPM = 500,000 us = 50,000,000 ten-nanosecond units
  const uint32_t tenNs = tempoFromBpm(120.0);
  check(tenNs == 50000000u, "120 BPM is 50,000,000 ten-nanosecond units per quarter");
  const Packet128 tempo = setTempo(tenNs);
  check(samePacket(tempo.data(), 0xD0100000u, 0x02FAF080u, 0, 0), "Set Tempo, bit-exact");
  uint32_t back = 0;
  check(decodeSetTempo(tempo.data(), 4, &back) && back == tenNs, "and decodes to the same value");
  check(std::fabs(bpmFromTempo(back) - 120.0) < 1e-9, "which is 120 BPM again");
  check(std::fabs(bpmFromTempo(tempoFromBpm(133.33)) - 133.33) < 1e-6,
        "a tempo to two decimal places survives the trip");

  // Set Time Signature 6/8 on group 3: denominator as a power of two (3 = an
  // eighth note), eight 32nd notes.
  TimeSignature six8;
  six8.numerator = 6;
  six8.denominatorPower = 3;
  const Packet128 ts = setTimeSignature(six8, 3);
  check(samePacket(ts.data(), 0xD3100001u, 0x06030800u, 0, 0), "Set Time Signature 6/8, group 3");
  TimeSignature tsBack;
  FlexHeader h;
  check(decodeSetTimeSignature(ts.data(), 4, &tsBack, &h) && tsBack.numerator == 6 &&
            tsBack.denominator() == 8 && h.group == 3,
        "decodes to 6/8 on group 3");

  // Set Metronome: 24 clocks per click, accents 4/0/0, 2 subdivision clicks.
  Metronome met;
  met.clocksPerPrimaryClick = 24;
  met.barAccent[0] = 4;
  met.subdivisionClicks[0] = 2;
  const Packet128 m = setMetronome(met);
  check(samePacket(m.data(), 0xD0100002u, 0x18040000u, 0x02000000u, 0), "Set Metronome, bit-exact");
  Metronome metBack;
  check(decodeSetMetronome(m.data(), 4, &metBack) && metBack.barAccent[0] == 4 &&
            metBack.subdivisionClicks[0] == 2 && metBack.clocksPerPrimaryClick == 24,
        "and back");

  // Key signature: sharps/flats is a signed nibble. A major (3 sharps, tonic
  // A = 1) and two flats on B (-2 = 0xE).
  KeySignature aMajor;
  aMajor.sharpsFlats = 3;
  aMajor.tonic = kNoteA;
  check(samePacket(setKeySignature(aMajor).data(), 0xD0100005u, 0x31000000u, 0, 0),
        "Set Key Signature: three sharps, tonic A");
  KeySignature flats;
  flats.sharpsFlats = -2;
  flats.tonic = kNoteB;
  const Packet128 ks = setKeySignature(flats);
  check(ks[1] == 0xE2000000u, "two flats is the nibble 0xE");
  KeySignature ksBack;
  check(decodeSetKeySignature(ks.data(), 4, &ksBack) && ksBack.sharpsFlats == -2 &&
            ksBack.tonic == kNoteB,
        "and reads back as -2, not 14");

  // Chord name, addressed to channel 0: C (tonic 3), type 0x0D, one
  // alteration "add 9" (type 1, degree 9), bass note E (5).
  ChordName c7;
  c7.tonic = kNoteC;
  c7.chordType = 0x0D;
  c7.alterations[0].type = 1;
  c7.alterations[0].degree = 9;
  c7.bassNote = kNoteE;
  const Packet128 chord = setChordName(c7);
  check(samePacket(chord.data(), 0xD0000006u, 0x030D1900u, 0, 0x05000000u),
        "Set Chord Name, bit-exact");
  ChordName chordBack;
  check(decodeSetChordName(chord.data(), 4, &chordBack) && chordBack.tonic == kNoteC &&
            chordBack.chordType == 0x0D && chordBack.alterations[0].degree == 9 &&
            chordBack.bassNote == kNoteE,
        "and back");
  check(!decodeSetTempo(chord.data(), 4, &back), "a chord is not decoded as a tempo");

  // A one-packet text: the composition name "Hello", bank 1 status 2.
  Packets text;
  toFlexText(kFlexBankMetadataText, kMetaCompositionName, "Hello", 5, text);
  check(text.list.size() == 1 &&
            samePacket(text.list[0].data(), 0xD0100102u, 0x48656C6Cu, 0x6F000000u, 0),
        "metadata text \"Hello\": one complete packet, zero-padded");

  // Thirty bytes of lyric on channel 0: start, continue, end.
  const char* lyric = "Twinkle twinkle little star, h";
  Packets lyr;
  toFlexText(kFlexBankPerformanceText, kPerformanceLyrics, lyric, 30, lyr, 0, kFlexAddressChannel, 0);
  check(lyr.list.size() == 3, "thirty bytes of lyric is three packets");
  check(lyr.list.size() == 3 && lyr.list[0][0] == 0xD0400201u && lyr.list[1][0] == 0xD0800201u &&
            lyr.list[2][0] == 0xD0C00201u,
        "with the form field start, continue, end in bits 23-22");

  FlexTextAssembler fa;
  bool done = false;
  for (const auto& p : lyr.list) done = fa.push(p.data(), 4);
  check(done && fa.size() == 30 && std::strcmp(fa.text(), lyric) == 0, "reassembled exactly");
  check(fa.header().bank == kFlexBankPerformanceText && fa.header().status == kPerformanceLyrics,
        "and says what it was");

  // Exactly twelve bytes: no room for a zero, and none needed.
  Packets twelve;
  toFlexText(kFlexBankMetadataText, kMetaComposer, "ABCDEFGHIJKL", 12, twelve);
  check(twelve.list.size() == 1 && fa.push(twelve.list[0].data(), 4) &&
            std::strcmp(fa.text(), "ABCDEFGHIJKL") == 0,
        "twelve bytes fill one packet with no terminator");

  Packets empty;
  toFlexText(kFlexBankMetadataText, kMetaClipName, "", 0, empty);
  check(empty.list.size() == 1 && fa.push(empty.list[0].data(), 4) && fa.size() == 0,
        "empty text is a message: the clip's name is now nothing");

  // Interleaved: two lyrics on channels 1 and 2, packet by packet.
  const std::string a = "first line of the verse on one";
  const std::string b = "second voice, on channel two, a bit longer";
  Packets pa, pb;
  toFlexText(kFlexBankPerformanceText, kPerformanceLyrics, a.data(), a.size(), pa, 0,
             kFlexAddressChannel, 1);
  toFlexText(kFlexBankPerformanceText, kPerformanceLyrics, b.data(), b.size(), pb, 0,
             kFlexAddressChannel, 2);
  FlexTextAssembler inter;
  std::vector<std::string> got;
  for (size_t i = 0; i < 8; ++i) {
    if (i < pa.list.size() && inter.push(pa.list[i].data(), 4)) got.push_back(inter.text());
    if (i < pb.list.size() && inter.push(pb.list[i].data(), 4)) got.push_back(inter.text());
  }
  check(got.size() == 2 && got[0] == a && got[1] == b, "two interleaved texts both come out whole");
  check(inter.dropped() == 0 && inter.orphans() == 0, "with nothing counted as lost");

  // Interrupted: a start, then another start for the same channel.
  FlexTextAssembler cut;
  cut.push(pa.list[0].data(), 4);
  bool second = false;
  for (const auto& p : pa.list) second = cut.push(p.data(), 4);
  check(second && cut.text() == a && cut.dropped() == 1,
        "a start inside an open text abandons the old one, counted, and the new one completes");

  // An end with nothing open is an orphan from a stream joined half way.
  FlexTextAssembler late;
  check(!late.push(pa.list.back().data(), 4) && late.orphans() == 1, "an end with no start is refused");

  // Too long for the buffer: dropped, never truncated.
  std::string huge(FlexTextAssembler::kCapacity + 40, 'x');
  Packets ph;
  toFlexText(kFlexBankMetadataText, kMetaCopyright, huge.data(), huge.size(), ph);
  FlexTextAssembler big;
  bool any = false;
  for (const auto& p : ph.list) any = big.push(p.data(), 4) || any;
  check(!any && big.dropped() == 1, "text longer than the buffer is dropped whole");

  // Setup messages pass straight through an assembler fed everything.
  check(!fa.push(tempo.data(), 4), "a tempo is not text and is ignored by the text assembler");
}

// ═══ UMP Stream ══════════════════════════════════════════════════════════════

static void umpStream() {
  std::printf("\n[UMP Stream]\n");
  using namespace ump;

  // Endpoint Discovery, UMP 1.1, asking for everything:
  //   [F][form 00 | status 0x000][major 01][minor 01]   word 1: filter 0x1F
  const Packet128 disc = endpointDiscovery(0x1F);
  check(samePacket(disc.data(), 0xF0000101u, 0x0000001Fu, 0, 0), "Endpoint Discovery, bit-exact");
  uint8_t filter = 0, major = 0, minor = 0;
  check(decodeEndpointDiscovery(disc.data(), 4, &filter, &major, &minor) && filter == 0x1F &&
            major == 1 && minor == 1,
        "and back");

  // Endpoint Info: static blocks (bit 31), three of them (bits 30-24), MIDI
  // 2.0 (bit 9) and 1.0 (bit 8), no JR timestamps.
  EndpointInfo info;
  info.staticFunctionBlocks = true;
  info.numFunctionBlocks = 3;
  info.midi2 = true;
  info.midi1 = true;
  const Packet128 ei = endpointInfoNotification(info);
  check(samePacket(ei.data(), 0xF0010101u, 0x83000300u, 0, 0), "Endpoint Info Notification");
  EndpointInfo eiBack;
  check(decodeEndpointInfo(ei.data(), 4, &eiBack) && eiBack.numFunctionBlocks == 3 &&
            eiBack.staticFunctionBlocks && eiBack.midi1 && eiBack.midi2 &&
            !eiBack.receivesJrTimestamps,
        "and back");

  // Device Identity: manufacturer 00 21 09, family 0x0123 (LSB 0x23, MSB
  // 0x02), model 0x0045, version 1.2.3.4 -- each a seven-bit byte.
  midici::DeviceIdentity id;
  id.manufacturer[0] = 0x00;
  id.manufacturer[1] = 0x21;
  id.manufacturer[2] = 0x09;
  id.family = 0x0123;
  id.model = 0x0045;
  id.version[0] = 1;
  id.version[1] = 2;
  id.version[2] = 3;
  id.version[3] = 4;
  const Packet128 di = deviceIdentityNotification(id);
  check(samePacket(di.data(), 0xF0020000u, 0x00002109u, 0x23024500u, 0x01020304u),
        "Device Identity Notification, LSB before MSB");
  midici::DeviceIdentity idBack;
  check(decodeDeviceIdentity(di.data(), 4, &idBack) && idBack.family == 0x0123 &&
            idBack.model == 0x0045 && idBack.manufacturer[1] == 0x21 && idBack.version[3] == 4,
        "and back into MIDI-CI's own struct");

  Packets name;
  endpointNameNotification("Sonore", 6, name);
  check(name.list.size() == 1 &&
            samePacket(name.list[0].data(), 0xF003536Fu, 0x6E6F7265u, 0, 0),
        "Endpoint Name \"Sonore\": two bytes in word 0, then word 1");

  StreamConfiguration cfg;
  cfg.protocol = kProtocolMidi2;
  check(samePacket(streamConfigurationRequest(cfg).data(), 0xF0050200u, 0, 0, 0),
        "Stream Configuration Request: MIDI 2.0");
  cfg.receiveJrTimestamps = cfg.transmitJrTimestamps = true;
  const Packet128 note = streamConfigurationNotification(cfg);
  check(samePacket(note.data(), 0xF0060203u, 0, 0, 0), "Notification with both JR bits");
  StreamConfiguration cfgBack;
  bool request = true;
  check(decodeStreamConfiguration(note.data(), 4, &cfgBack, &request) && !request &&
            cfgBack.protocol == kProtocolMidi2 && cfgBack.receiveJrTimestamps,
        "and back, as a notification");

  check(samePacket(functionBlockDiscovery(kAllFunctionBlocks, 0x03).data(), 0xF010FF03u, 0, 0, 0),
        "Function Block Discovery: all blocks, info and name");

  FunctionBlockInfo fb;
  fb.active = true;
  fb.number = 0;
  fb.uiHint = kBlockHintBoth;
  fb.direction = kBlockDirectionBidirectional;
  fb.firstGroup = 0;
  fb.numGroups = 1;
  fb.midiCiVersion = 1;
  const Packet128 fbi = functionBlockInfoNotification(fb);
  check(samePacket(fbi.data(), 0xF0118033u, 0x00010100u, 0, 0), "Function Block Info Notification");
  FunctionBlockInfo fbBack;
  check(decodeFunctionBlockInfo(fbi.data(), 4, &fbBack) && fbBack.active &&
            fbBack.uiHint == kBlockHintBoth && fbBack.direction == kBlockDirectionBidirectional &&
            fbBack.midiCiVersion == 1,
        "and back");

  Packets fbName;
  functionBlockNameNotification(2, "Synth", 5, fbName);
  check(fbName.list.size() == 1 &&
            samePacket(fbName.list[0].data(), 0xF0120253u, 0x796E7468u, 0, 0),
        "Function Block Name: block number, then thirteen bytes per packet");

  check(samePacket(startOfClip().data(), 0xF0200000u, 0, 0, 0) &&
            samePacket(endOfClip().data(), 0xF0210000u, 0, 0, 0),
        "Start and End of Clip");
  check(isStartOfClip(startOfClip().data(), 4) && !isStartOfClip(endOfClip().data(), 4),
        "and told apart");

  // A long name across packets, interleaved with a function block's.
  std::string longName;
  for (int i = 0; i < 98; ++i) longName.push_back((char) ('a' + i % 26));
  Packets ln, fn;
  endpointNameNotification(longName.data(), longName.size(), ln);
  functionBlockNameNotification(5, "Main Engine and its long name", 29, fn);
  check(ln.list.size() == 7, "98 bytes of name is seven packets of fourteen");
  check(ln.list.size() == 7 && streamForm(ln.list[0][0]) == kFormStart &&
            streamForm(ln.list[3][0]) == kFormContinue && streamForm(ln.list[6][0]) == kFormEnd,
        "start, continues, end");
  StreamTextAssembler sa;
  std::vector<std::string> names;
  std::vector<int> blocks;
  for (size_t i = 0; i < 7; ++i) {
    if (sa.push(ln.list[i].data(), 4)) {
      names.push_back(sa.text());
      blocks.push_back(-1);
    }
    if (i < fn.list.size() && sa.push(fn.list[i].data(), 4)) {
      names.push_back(sa.text());
      blocks.push_back(sa.functionBlock());
    }
  }
  check(names.size() == 2 && names[0] == "Main Engine and its long name" && blocks[0] == 5 &&
            names[1] == longName,
        "interleaved names both reassemble, the block's with its number");

  // The limit is cut at a character boundary: 97 ASCII bytes and a three-
  // byte character would end mid-character at 98.
  std::string over(97, 'n');
  over += "\xE2\x82\xAC"; // the euro sign
  Packets cutName;
  endpointNameNotification(over.data(), over.size(), cutName);
  StreamTextAssembler sc;
  bool fin = false;
  for (const auto& p : cutName.list) fin = sc.push(p.data(), 4);
  check(fin && sc.size() == 97, "a name over the limit is cut BEFORE the character that crosses it");
}

// ═══ SysEx8 and Mixed Data Set ═══════════════════════════════════════════════

static void sysex8() {
  std::printf("\n[UMP SysEx8 / Mixed Data Set]\n");
  using namespace ump;

  // Identity request payload, stream 0, group 0: four data bytes plus the
  // stream ID is five.
  //   [5][0][status 0][bytes 5][stream 00][7E]   [7F 06 01 00]
  const uint8_t ident[] = {0x7E, 0x7F, 0x06, 0x01};
  Packets one;
  toSysex8(ident, 4, one);
  check(one.list.size() == 1 && samePacket(one.list[0].data(), 0x5005007Eu, 0x7F060100u, 0, 0),
        "a four-byte SysEx8, bit-exact: the count includes the stream ID");

  uint8_t thirteen[13];
  for (int i = 0; i < 13; ++i) thirteen[i] = (uint8_t) (0xF0 + i); // eight-bit values
  Packets full;
  toSysex8(thirteen, 13, full);
  check(full.list.size() == 1 && ((full.list[0][0] >> 16) & 0xF) == 14 &&
            (full.list[0][3] & 0xFF) == 0xFC,
        "thirteen bytes fill one packet exactly (count 14)");

  std::vector<uint8_t> thirty(30);
  for (size_t i = 0; i < 30; ++i) thirty[i] = (uint8_t) (i * 9 + 200);
  Packets three;
  toSysex8(thirty.data(), 30, three, 7, 2);
  check(three.list.size() == 3 && (three.list[0][0] & 0xFFFFFF00u) == 0x521E0700u &&
            (three.list[1][0] & 0xFFFFFF00u) == 0x522E0700u &&
            (three.list[2][0] & 0xFFFFFF00u) == 0x52350700u,
        "thirty bytes on stream 7, group 2: start(14), continue(14), end(5)");

  Sysex8Assembler as;
  bool done = false;
  for (const auto& p : three.list) done = as.push(p.data(), 4);
  check(done && as.size() == 30 && std::memcmp(as.data(), thirty.data(), 30) == 0 &&
            as.streamId() == 7 && as.group() == 2,
        "reassembled, all eight bits of every byte, with its stream and group");

  // Two streams on one group, interleaved packet by packet.
  std::vector<uint8_t> a(40), b(27);
  for (size_t i = 0; i < a.size(); ++i) a[i] = (uint8_t) (i ^ 0xA5);
  for (size_t i = 0; i < b.size(); ++i) b[i] = (uint8_t) (255 - i);
  Packets pa, pb;
  toSysex8(a.data(), a.size(), pa, 1);
  toSysex8(b.data(), b.size(), pb, 2);
  Sysex8Assembler inter;
  int whole = 0;
  bool aOk = false, bOk = false;
  for (size_t i = 0; i < 4; ++i) {
    if (i < pb.list.size() && inter.push(pb.list[i].data(), 4)) {
      ++whole;
      bOk = inter.streamId() == 2 && inter.size() == b.size() &&
            std::memcmp(inter.data(), b.data(), b.size()) == 0;
    }
    if (i < pa.list.size() && inter.push(pa.list[i].data(), 4)) {
      ++whole;
      aOk = inter.streamId() == 1 && inter.size() == a.size() &&
            std::memcmp(inter.data(), a.data(), a.size()) == 0;
    }
  }
  check(whole == 2 && aOk && bOk, "two streams interleaved on one group both come out whole");

  // The same stream ID on two groups is two messages.
  Packets g0, g1;
  toSysex8(a.data(), a.size(), g0, 3, 0);
  toSysex8(b.data(), b.size(), g1, 3, 1);
  Sysex8Assembler groups;
  int finished = 0;
  for (size_t i = 0; i < 4; ++i) {
    if (i < g0.list.size() && groups.push(g0.list[i].data(), 4)) ++finished;
    if (i < g1.list.size() && groups.push(g1.list[i].data(), 4)) ++finished;
  }
  check(finished == 2 && groups.dropped() == 0, "stream 3 on groups 0 and 1 does not collide");

  // Malformed: a continue claiming zero bytes abandons the open message.
  Sysex8Assembler mal;
  mal.push(pa.list[0].data(), 4);
  std::vector<uint32_t> broken = pa.list[1];
  broken[0] &= ~(0xFu << 16); // byte count 0: not even a stream ID
  check(!mal.push(broken.data(), 4) && mal.malformed() == 1, "a packet with byte count 0 is malformed");
  check(!mal.inProgress() && mal.dropped() == 1, "and the message it was part of is abandoned");
  check(!mal.push(pa.list[2].data(), 4) && mal.orphans() == 1,
        "so its next packet is an orphan, not the rest of a message with a hole in it");
  std::vector<uint32_t> fifteen = pa.list[0];
  fifteen[0] |= 0xFu << 16;
  check(!mal.push(fifteen.data(), 4) && mal.malformed() == 2, "a byte count of 15 is malformed too");

  // Interrupted.
  Sysex8Assembler again;
  again.push(pa.list[0].data(), 4);
  bool second = false;
  for (const auto& p : pa.list) second = again.push(p.data(), 4);
  check(second && again.dropped() == 1, "a start inside an open stream abandons it, counted");

  // Too long: dropped, not truncated.
  std::vector<uint8_t> huge((size_t) Sysex8Assembler::kCapacity + 5, 0x42);
  Packets ph;
  toSysex8(huge.data(), huge.size(), ph);
  Sysex8Assembler big;
  bool any = false;
  for (const auto& p : ph.list) any = big.push(p.data(), 4) || any;
  check(!any && big.dropped() == 1, "a SysEx8 longer than the buffer is dropped whole");

  // ── Mixed Data Set ──
  MixedDataSetHeader mh;
  mh.mdsId = 0;
  mh.numChunks = 1;
  mh.chunkNumber = 1;
  mh.manufacturerId = 0x0041;
  mh.deviceId = 0x0010;
  mh.subId1 = 0x0102;
  mh.subId2 = 0x0304;
  std::vector<uint8_t> chunk(20);
  for (size_t i = 0; i < 20; ++i) chunk[i] = (uint8_t) (0x10 + i);
  Packets mds;
  toMixedDataSetChunk(mh, chunk.data(), chunk.size(), mds);
  check(mds.list.size() == 3, "twenty bytes is a header and two payloads of fourteen");
  check(mds.list.size() == 3 &&
            samePacket(mds.list[0].data(), 0x50800014u, 0x00010001u, 0x00410010u, 0x01020304u),
        "Mixed Data Set Header, bit-exact");
  check(mds.list.size() == 3 &&
            samePacket(mds.list[1].data(), 0x50901011u, 0x12131415u, 0x16171819u, 0x1A1B1C1Du),
        "first payload: two bytes in word 0, twelve after");
  check(mds.list.size() == 3 &&
            samePacket(mds.list[2].data(), 0x50901E1Fu, 0x20212223u, 0, 0),
        "last payload: six bytes, zero-padded");

  MixedDataSetAssembler ma;
  bool chunkDone = false;
  for (const auto& p : mds.list) chunkDone = ma.push(p.data(), 4);
  check(chunkDone && ma.size() == 20 && std::memcmp(ma.data(), chunk.data(), 20) == 0 &&
            ma.header().manufacturerId == 0x0041 && ma.header().subId2 == 0x0304,
        "reassembled with its header");

  // Two data sets interleaved; a payload with no header; a header inside a chunk.
  MixedDataSetHeader other = mh;
  other.mdsId = 5;
  std::vector<uint8_t> chunk2(33, 0x77);
  Packets mds2;
  toMixedDataSetChunk(other, chunk2.data(), chunk2.size(), mds2);
  MixedDataSetAssembler mi;
  int chunks = 0;
  for (size_t i = 0; i < 4; ++i) {
    if (i < mds.list.size() && mi.push(mds.list[i].data(), 4))
      chunks += (mi.size() == 20 && mi.header().mdsId == 0) ? 1 : 0;
    if (i < mds2.list.size() && mi.push(mds2.list[i].data(), 4))
      chunks += (mi.size() == 33 && mi.header().mdsId == 5) ? 1 : 0;
  }
  check(chunks == 2 && mi.dropped() == 0, "data sets 0 and 5 interleaved both complete");
  MixedDataSetAssembler orphan;
  check(!orphan.push(mds.list[1].data(), 4) && orphan.orphans() == 1, "a payload with no header is refused");
  orphan.push(mds.list[0].data(), 4);
  orphan.push(mds.list[0].data(), 4);
  check(orphan.dropped() == 1, "a header inside an unfinished chunk drops it, counted");
  Packets none;
  toMixedDataSetChunk(mh, nullptr, 0, none);
  MixedDataSetAssembler zero;
  check(none.list.size() == 1 && zero.push(none.list[0].data(), 4) && zero.size() == 0,
        "an empty chunk completes on its header");

  // ── Splitting a word stream ──
  std::vector<uint32_t> stream = {0x20903C64u,               // MIDI 1.0 note on: 1 word
                                  0x40903C00u, 0xFFFF0000u}; // MIDI 2.0 note on: 2 words
  const Packet128 discovery = endpointDiscovery(0x1F);
  stream.insert(stream.end(), discovery.data(), discovery.data() + 4); // 4 words
  stream.push_back(0xD0100000u);                               // a tempo cut short
  int seen = 0;
  const size_t used = forEachPacket(stream.data(), stream.size(), [&](const uint32_t*, size_t) { ++seen; });
  check(seen == 3 && used == 7, "a word run splits into whole packets, leaving the cut-off one");
}

// ═══ MidiMessageCollector ════════════════════════════════════════════════════

static void collector() {
  std::printf("\n[MidiMessageCollector]\n");
  char msg[200];
  const double sr = 48000.0;
  const double t0 = 100.0;
  const double block = 512.0 / sr;

  // An interval exactly one block long maps one-to-one.
  check(MidiMessageCollector::samplePosition(t0, t0, t0 + block, sr, 512) == 0,
        "an event at the start of the interval is frame 0");
  check(MidiMessageCollector::samplePosition(t0 + 100.0 / sr, t0, t0 + block, sr, 512) == 100,
        "100 samples in is frame 100");
  check(MidiMessageCollector::samplePosition(t0 + block, t0, t0 + block, sr, 512) == 511,
        "the block's own time is its last frame");
  check(MidiMessageCollector::samplePosition(t0 - 1.0, t0, t0 + block, sr, 512) == 0,
        "an event from before the interval (it waited for room) is frame 0");
  check(MidiMessageCollector::samplePosition(std::nan(""), t0, t0 + block, sr, 512) == 0,
        "a NaN stamp is frame 0, not undefined behaviour");

  // A callback that came early: 256 samples of time for a 512-sample block.
  // Laid against the END, so arrival-to-sound stays one block.
  const double early = 256.0 / sr;
  check(MidiMessageCollector::samplePosition(t0, t0, t0 + early, sr, 512) == 256 &&
            MidiMessageCollector::samplePosition(t0 + 100.0 / sr, t0, t0 + early, sr, 512) == 356,
        "an early callback's interval sits at the end of the block");

  // A late one: 1024 samples into 512 frames, spacing halved, order kept.
  const double late = 1024.0 / sr;
  const int p1 = MidiMessageCollector::samplePosition(t0 + 512.0 / sr, t0, t0 + late, sr, 512);
  const int p2 = MidiMessageCollector::samplePosition(t0 + 768.0 / sr, t0, t0 + late, sr, 512);
  std::snprintf(msg, sizeof(msg), "a late callback is squeezed in: 512 -> %d, 768 -> %d", p1, p2);
  check(p1 == 256 && p2 == 384, msg);

  // A stall of 100 blocks: only the last 32 keep their spacing; older lands
  // on 0 -- KEPT, not dropped.
  const double stall = 100.0 * block;
  const int old = MidiMessageCollector::samplePosition(t0 + 10.0 * block, t0, t0 + stall, sr, 512);
  const int recent = MidiMessageCollector::samplePosition(t0 + 84.0 * block, t0, t0 + stall, sr, 512);
  std::snprintf(msg, sizeof(msg), "after a 100-block stall: block 10 -> %d, block 84 -> %d", old, recent);
  check(old == 0 && recent == 256, msg);

  // Through the queue: eight events 64 samples apart.
  MidiMessageCollector c;
  c.reset(sr, t0);
  for (int k = 0; k < 8; ++k)
    c.addMessageToQueue(MidiMessage::noteOn(0, 60 + k, 100), t0 + (double) (k * 64) / sr);
  MidiBuffer out;
  c.removeNextBlockOfMessages(out, 512, t0 + block);
  bool spaced = out.getNumEvents() == 8;
  int k = 0;
  for (const auto& e : out) {
    spaced = spaced && e.samplePosition == k * 64 && e.getMessage().getNoteNumber() == 60 + k;
    ++k;
  }
  check(spaced, "eight events stamped 64 samples apart land 64 frames apart, in order");
  check(!c.addMessageToQueue(MidiMessage(0x05, 0, 0), t0), "a data byte is not queued as a message");

  // The next block's interval starts where this one ended.
  out.clear();
  c.addMessageToQueue(MidiMessage::noteOff(0, 60), t0 + block + 32.0 / sr);
  c.removeNextBlockOfMessages(out, 512, t0 + 2.0 * block);
  check(out.getNumEvents() == 1 && out.begin()->samplePosition == 32,
        "the next block measures from the end of the previous one");

  // A block with no room: what does not fit waits, and is not lost.
  MidiMessageCollector full;
  full.reset(sr, t0);
  for (int i = 0; i < MidiBuffer::kMaxEvents + 76; ++i)
    full.addMessageToQueue(MidiMessage::controlChange(0, 1, i & 127), t0 + 1.0 / sr);
  out.clear();
  full.removeNextBlockOfMessages(out, 512, t0 + block);
  const int first = out.getNumEvents();
  out.clear();
  full.removeNextBlockOfMessages(out, 512, t0 + 2.0 * block);
  std::snprintf(msg, sizeof(msg), "%d events into a %d-event block: %d now, %d in the next",
                MidiBuffer::kMaxEvents + 76, MidiBuffer::kMaxEvents, first, out.getNumEvents());
  check(first == MidiBuffer::kMaxEvents && out.getNumEvents() == 76 && out.getNumDropped() == 0, msg);
  check(out.begin()->samplePosition == 0, "and the ones that waited are at frame 0");

  // SysEx rides the same clock.
  const uint8_t sx[] = {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7};
  MidiMessageCollector cs;
  cs.reset(sr, t0);
  cs.addSysexToQueue(sx, sizeof(sx), t0 + 200.0 / sr);
  out.clear();
  cs.removeNextBlockOfMessages(out, 512, t0 + block);
  check(out.getNumSysex() == 1 && out.sysexBegin()->samplePosition == 200 &&
            std::memcmp(out.sysexData(*out.sysexBegin()), sx, sizeof(sx)) == 0,
        "a SysEx is positioned by its stamp too");

  // The drain does not allocate.
  MidiMessageCollector rt;
  rt.reset(sr);
  for (int i = 0; i < 300; ++i) rt.addMessageToQueue(MidiMessage::noteOn(1, i & 127, 90));
  rt.addSysexToQueue(sx, sizeof(sx));
  out.clear();
  g_allocations = 0;
  g_armed = true;
  rt.removeNextBlockOfMessages(out, 256);
  rt.removeNextBlockOfMessages(out, 256);
  g_armed = false;
  std::snprintf(msg, sizeof(msg), "the audio-thread drain made %ld allocations",
                (long) g_allocations.load());
  check(g_allocations.load() == 0 && out.getNumEvents() == 300, msg);
  check(std::atomic<size_t>::is_always_lock_free, "and its hand-over is a lock-free atomic here");

  // Three producers race a consumer. Nothing lost, nothing reordered within
  // a producer, every position inside its block.
  const int perThread = 20000;
  MidiMessageCollector race;
  race.reset(sr);
  std::atomic<bool> go{false};
  std::vector<std::thread> producers;
  for (int t = 0; t < 3; ++t)
    producers.emplace_back([&, t] {
      while (!go.load()) std::this_thread::yield();
      for (int i = 0; i < perThread; ++i) {
        // Sequence in note and velocity; channel says who sent it.
        const MidiMessage m(0x90 | t, i & 127, (i >> 7) & 127);
        while (!race.addMessageToQueue(m)) std::this_thread::yield();
      }
    });
  go = true;
  int received[3] = {0, 0, 0};
  bool ordered = true, inBlock = true;
  const auto start = std::chrono::steady_clock::now();
  MidiBuffer blockMidi;
  while (received[0] + received[1] + received[2] < 3 * perThread) {
    blockMidi.clear();
    race.removeNextBlockOfMessages(blockMidi, 256);
    int lastPos = 0;
    for (const auto& e : blockMidi) {
      const int t = e.getMessage().getRawStatus() & 0x0F;
      const int seq = e.getMessage().getNoteNumber() | (e.getMessage().getVelocity() << 7);
      if (t > 2 || seq != (received[t] & 0x3FFF)) ordered = false;
      if (t <= 2) ++received[t];
      if (e.samplePosition < lastPos || e.samplePosition < 0 || e.samplePosition > 255) inBlock = false;
      lastPos = e.samplePosition;
    }
    if (std::chrono::steady_clock::now() - start > std::chrono::seconds(20)) break;
  }
  for (auto& th : producers) th.join();
  std::snprintf(msg, sizeof(msg), "three producers, %d each: received %d, %d, %d", perThread,
                received[0], received[1], received[2]);
  check(received[0] == perThread && received[1] == perThread && received[2] == perThread, msg);
  check(ordered, "each producer's messages arrive in the order it sent them");
  check(inBlock, "every position is inside its block, and a block's are sorted");
}

// ═══ OSC address patterns ════════════════════════════════════════════════════

static void oscPatterns() {
  std::printf("\n[OSC address patterns]\n");
  struct Case {
    const char* pattern;
    const char* address;
    bool expected;
    const char* rule;
  };
  const Case table[] = {
      // '?' -- any single character
      {"/synth/?/cutoff", "/synth/1/cutoff", true, "? matches one character"},
      {"/a/?", "/a/bc", false, "? does not match two"},
      {"/a/b?", "/a/b", false, "? does not match none"},
      // '*' -- any sequence, including none, never across '/'
      {"/synth/*/cutoff", "/synth/1/cutoff", true, "* matches a part"},
      {"/synth/*/cutoff", "/synth/12/cutoff", true, "* matches several characters"},
      {"/a/b*", "/a/b", true, "* matches nothing"},
      {"/a/*b", "/a/abcb", true, "* then a literal, backing off to the last b"},
      {"/synth/*/cutoff", "/synth/1/2/cutoff", false, "* never crosses a /"},
      {"/a*", "/a/b", false, "nor at the end of a part"},
      {"/*/*", "/a/b", true, "one * per part"},
      {"/*a*a*a*b", "/aaaab", true, "several stars"},
      {"/*a*a*a*b", "/aaaa", false, "several stars, no b"},
      // [...] -- one character from a set
      {"/[abc]", "/b", true, "[abc] matches b"},
      {"/[abc]", "/d", false, "[abc] does not match d"},
      {"/[a-c]x", "/bx", true, "a-z inside brackets is a range"},
      {"/[a-c]x", "/dx", false, "and stops where it says"},
      {"/[!a-c]x", "/dx", true, "! first negates"},
      {"/[!a-c]x", "/bx", false, "a negated range excludes its members"},
      {"/[a-]", "/-", true, "- last is a literal minus"},
      {"/[-a]", "/-", true, "- first is a literal minus"},
      {"/[a!]", "/!", true, "! not first is a literal"},
      {"/[z-a]", "/m", true, "a range written backwards still means the letters between"},
      // {...} -- one string from a list
      {"/{foo,bar}", "/bar", true, "{foo,bar} matches bar"},
      {"/{foo,bar}", "/baz", false, "and not baz"},
      {"/{foo,bar}", "/foobar", false, "one alternative, not two"},
      {"/x{foo,bar}y", "/xfooy", true, "alternatives in the middle of a part"},
      {"/{,pre}fix", "/fix", true, "an empty alternative"},
      {"/mixer/{bass,drums}/mute", "/mixer/drums/mute", true, "the case a template sends"},
      // Plain addresses
      {"/a/b", "/a/b", true, "an address matches itself"},
      {"/a/b", "/a/b/c", false, "the number of parts must agree"},
      {"/a/b/c", "/a/b", false, "both ways"},
      // Malformed patterns match nothing
      {"/[abc", "/a", false, "an unclosed [ is malformed"},
      {"/{foo", "/foo", false, "an unclosed { is malformed"},
      {"/[]", "/a", false, "an empty set is malformed"},
      {"/[!]", "/a", false, "and so is an empty negated one"},
      {"/a]", "/a]", false, "a ] with no [ is malformed"},
      {"/a}", "/a}", false, "a } with no { is malformed"},
      {"/a,b", "/a,b", false, "a comma outside braces is malformed"},
      {"/a b", "/a b", false, "a space is not allowed"},
      {"/a#", "/a#", false, "nor a #"},
      {"//a", "//a", false, "an empty part is malformed (OSC 1.1's // is not 1.0)"},
      {"/a/", "/a/", false, "and so is a trailing /"},
      {"a/b", "a/b", false, "an address starts with /"},
      {"/*", "/a*", false, "a pattern does not match an address with a wildcard in it"},
  };
  int passed = 0;
  const int total = (int) (sizeof(table) / sizeof(table[0]));
  for (const Case& c : table) {
    const bool got = osc::matchesPattern(c.pattern, c.address);
    if (got == c.expected) {
      ++passed;
    } else {
      char msg[200];
      std::snprintf(msg, sizeof(msg), "%s ~ %s gave %s: %s", c.pattern, c.address,
                    got ? "true" : "false", c.rule);
      check(false, msg);
    }
  }
  char msg[200];
  std::snprintf(msg, sizeof(msg), "%d of %d pattern cases from the OSC 1.0 rules", passed, total);
  check(passed == total, msg);

  // A part longer than the matcher's stack buffer.
  std::string longAddress = "/" + std::string(300, 'y') + "z";
  check(osc::matchesPattern("/*z", longAddress), "a 301-character part takes the long path");

  // The pathological pattern a backtracking matcher takes exponential time on.
  std::string as = "/" + std::string(400, 'a');
  const auto t0 = std::chrono::steady_clock::now();
  const bool slow = osc::matchesPattern("/*a*a*a*a*a*a*a*a*a*a*a*a*b", as);
  const double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::snprintf(msg, sizeof(msg), "*a*a*...*b against 400 a's: no match, in %.2f ms", ms);
  check(!slow && ms < 250.0, msg);

  osc::OSCAddressPattern exact("/synth/1/cutoff");
  check(exact.isValid() && !exact.containsWildcards(), "a plain address is a valid pattern with no wildcards");
  check(osc::isValidAddress("/synth/1/cutoff") && !osc::isValidAddress("/synth/*/cutoff") &&
            !osc::isValidAddress("/"),
        "isValidAddress refuses wildcards and the bare root");

  // ── Dispatch ──
  osc::Dispatcher d;
  int c1 = 0, c2 = 0, r1 = 0;
  check(d.addListener("/synth/1/cutoff", [&](const osc::Message&) { ++c1; }) &&
            d.addListener("/synth/2/cutoff", [&](const osc::Message&) { ++c2; }) &&
            d.addListener("/synth/1/res", [&](const osc::Message&) { ++r1; }),
        "three listeners");
  check(!d.addListener("/synth/*/cutoff", [](const osc::Message&) {}),
        "a listener cannot be at a pattern: patterns are what senders write");
  osc::Message m;
  m.address = "/synth/*/cutoff";
  check(d.dispatch(m) == 2 && c1 == 1 && c2 == 1 && r1 == 0, "/synth/*/cutoff reaches both cutoffs");
  m.address = "/synth/1/res";
  check(d.dispatch(m) == 1 && r1 == 1, "an exact address reaches exactly its listener");
  m.address = "/synth/[12]/*";
  check(d.dispatch(m) == 3, "/synth/[12]/* reaches all three");
  m.address = "/synth/3/cutoff";
  check(d.dispatch(m) == 0, "an address nobody listens at reaches nobody");
  m.address = "/synth/[1/cutoff";
  check(d.dispatch(m) == 0, "a malformed pattern reaches nobody");
  // A handler that changes the listeners while being called.
  int added = 0;
  d.addListener("/self", [&](const osc::Message&) {
    ++added;
    d.addListener("/self", [&](const osc::Message&) { ++added; });
    d.removeListeners("/synth/1/cutoff");
  });
  m.address = "/self";
  d.dispatch(m);
  check(added == 1 && d.numAddresses() == 3, "a handler may add and remove listeners mid-dispatch");

  // Over a real socket: a pattern sent on the loopback, dispatched on poll.
  osc::Receiver rx;
  osc::Sender tx;
  if (rx.open(0) && tx.open("127.0.0.1", rx.port())) {
    osc::Dispatcher net;
    int hits = 0;
    net.addListener("/fx/1/mix", [&](const osc::Message&) { ++hits; });
    net.addListener("/fx/2/mix", [&](const osc::Message&) { ++hits; });
    osc::Message sent;
    sent.address = "/fx/{1,2}/mix";
    sent.arguments.push_back(osc::Argument::makeFloat(0.5f));
    tx.send(sent);
    int calls = 0;
    for (int i = 0; i < 100 && calls == 0; ++i) {
      calls = rx.pollAndDispatch(net);
      if (!calls) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(calls == 2 && hits == 2, "a pattern over UDP loopback reaches both listeners on poll");
  } else {
    // As sdk_tests does: a sandbox with no sockets is not a matcher bug.
    std::printf("  SKIPPED the OSC socket dispatch: %s\n", rx.error().c_str());
  }
}

// ═══ OSC sender addresses ═══════════════════════════════════════════════════
//
// Sender::open parses the address itself now, rather than through inet_pton:
// ws2tcpip.h, where Windows declares it, cannot be included after a plain
// <windows.h> (see osc.h). These are inet_pton(AF_INET)'s answers on Linux.

static void oscAddresses() {
  std::printf("\n[OSC sender addresses]\n");
  struct Case {
    const char* text;
    bool ok;
    uint32_t value;
  };
  const Case cases[] = {
      {"127.0.0.1", true, 0x7F000001u},     {"192.168.1.20", true, 0xC0A80114u},
      {"0.0.0.0", true, 0u},                {"255.255.255.255", true, 0xFFFFFFFFu},
      {"10.0.0.255", true, 0x0A0000FFu},    {"256.0.0.1", false, 0},
      {"1.2.3", false, 0},                  {"1.2.3.4.5", false, 0},
      {"1.2.3.4 ", false, 0},               {" 1.2.3.4", false, 0},
      {"1..3.4", false, 0},                 {"01.2.3.4", false, 0},
      {"1.2.3.00", false, 0},               {"1.2.3.-4", false, 0},
      {"localhost", false, 0},              {"", false, 0},
      {"1.2.3.4x", false, 0},               {"9999.1.1.1", false, 0},
  };
  int wrong = 0;
  for (const Case& c : cases) {
    uint32_t got = 0xDEADBEEFu;
    const bool ok = osc::detail::parseIPv4(c.text, &got);
    if (ok != c.ok || (ok && got != c.value)) {
      ++wrong;
      std::printf("  ---- \"%s\": %s 0x%08X\n", c.text, ok ? "accepted" : "refused", (unsigned) got);
    }
  }
  check(wrong == 0, "dotted quads parse to their value; short, long, padded, leading-zero, "
                    "out-of-range and named ones are refused (18 cases)");
  check(!osc::detail::parseIPv4(nullptr, nullptr), "a null address is refused");
  osc::Receiver canListen;
  if (canListen.open(0)) {
    osc::Sender tx;
    check(!tx.open("localhost", 9000), "a sender given a name, not an address, does not open");
    check(tx.open(nullptr, 9000), "and one given none opens, to 127.0.0.1");
    // A refused open, then one that works: error() is about the latest.
    osc::Receiver second;
    const bool taken = !second.open(canListen.port());
    const std::string refusal = second.error();
    const bool reopened = second.open(0);
    char msg[200];
    std::snprintf(msg, sizeof(msg),
                  "a receiver refused a taken port (\"%s\") and then opened says nothing is "
                  "wrong (error() \"%s\")",
                  refusal.c_str(), second.error().c_str());
    check(taken && !refusal.empty() && reopened && second.error().empty(), msg);
  } else {
    std::printf("  SKIPPED the sender's open: %s\n", canListen.error().c_str());
  }
}

// ═══ OSC colour ═════════════════════════════════════════════════════════════

static void oscColour() {
  std::printf("\n[OSC colour]\n");
  const osc::OSCColour c = osc::OSCColour::fromInt32RGBA(0x11223344u);
  check(c.red == 0x11 && c.green == 0x22 && c.blue == 0x33 && c.alpha == 0x44,
        "0x11223344 is red 11, green 22, blue 33, alpha 44: red in the HIGH byte");
  check(c.toInt32() == 0x11223344u, "and back");

  osc::Message m;
  m.address = "/light/colour";
  m.arguments.push_back(osc::Argument::makeColour(c));
  const std::vector<uint8_t> bytes = osc::encode(m);
  // "/light/colour" is 13 bytes, padded to 16; ",r" padded to 4; then RGBA.
  const uint8_t want[] = {'/', 'l', 'i', 'g', 'h', 't', '/', 'c', 'o', 'l', 'o', 'u',
                          'r', 0,   0,   0,   ',', 'r', 0,   0,   0x11, 0x22, 0x33, 0x44};
  check(bytes.size() == sizeof(want) && std::memcmp(bytes.data(), want, sizeof(want)) == 0,
        "a colour argument on the wire, bit-exact: tag 'r', then R G B A big-endian");

  osc::Message back;
  check(osc::decodeMessage(bytes.data(), bytes.size(), &back) && back.arguments.size() == 1 &&
            back.arguments[0].isColour() && back.arguments[0].getColour() == c,
        "decodes to the same colour, through the typed accessor");
  const osc::Argument notColour = osc::Argument::makeInt(0x11223344);
  check(!notColour.isColour() && notColour.getColour() == osc::OSCColour{},
        "an int is not read as a colour");

  // In a bundle with other types.
  osc::Message mixed;
  mixed.address = "/scene";
  mixed.arguments.push_back(osc::Argument::makeString("intro"));
  mixed.arguments.push_back(osc::Argument::makeColour(osc::OSCColour::fromInt32RGBA(0xFF8000FFu)));
  mixed.arguments.push_back(osc::Argument::makeFloat(0.25f));
  const std::vector<uint8_t> mb = osc::encode(mixed);
  osc::Message mback;
  check(osc::decodeMessage(mb.data(), mb.size(), &mback) && mback.arguments.size() == 3 &&
            mback.arguments[1].getColour().red == 0xFF && mback.arguments[1].getColour().green == 0x80 &&
            mback.arguments[2].floatValue == 0.25f,
        "between a string and a float, everything stays aligned");
}

// ═══ OSC bundles inside bundles ═════════════════════════════════════════════
//
// Found in review: decodePacket recursed once per level of nesting, with no
// limit, and moved every message up through every level above it. One UDP
// datagram of bundles each holding only the next (3275 fit in 64 KB)
// overflowed the stack of a 512 KB thread, or of the main thread in a Debug
// build; a packet from a stream transport, which has no 64 KB ceiling, did it
// on any thread.

/** `depth` bundles, each the only element of the one around it, the innermost
 *  holding `messages` copies of the smallest message there is ("/", no
 *  arguments). Written front to back: every level's length is what follows. */
static std::vector<uint8_t> nestedBundles(int depth, int messages) {
  const uint8_t message[8] = {'/', 0, 0, 0, ',', 0, 0, 0};
  const size_t inner = 16 + (size_t) messages * 12; // the innermost bundle
  std::vector<uint8_t> out((size_t) (depth - 1) * 20 + inner, 0);
  for (int level = 0; level < depth; ++level) {
    uint8_t* at = out.data() + (size_t) level * 20;
    std::memcpy(at, "#bundle", 8);
    if (level + 1 < depth) {
      const uint32_t length = (uint32_t) (out.size() - (size_t) (level + 1) * 20);
      for (int k = 0; k < 4; ++k) at[16 + k] = (uint8_t) (length >> (24 - 8 * k));
    }
  }
  uint8_t* p = out.data() + (size_t) (depth - 1) * 20 + 16;
  for (int i = 0; i < messages; ++i, p += 12) {
    p[3] = 8;
    std::memcpy(p + 4, message, 8);
  }
  return out;
}

static void oscBundleDepth() {
  std::printf("\n[OSC bundles inside bundles]\n");
  std::vector<osc::Message> out;
  const std::vector<uint8_t> flat = nestedBundles(1, 3);
  check(osc::decodePacket(flat.data(), flat.size(), &out) && out.size() == 3 && out[2].address == "/",
        "a bundle of three messages decodes (the builder below is right)");

  out.clear();
  const std::vector<uint8_t> deepest = nestedBundles(osc::detail::kMaxBundleDepth, 5);
  check(osc::decodePacket(deepest.data(), deepest.size(), &out) && out.size() == 5,
        "bundles nested to the limit still decode, every message delivered");
  out.clear();
  const std::vector<uint8_t> tooDeep = nestedBundles(osc::detail::kMaxBundleDepth + 1, 5);
  check(!osc::decodePacket(tooDeep.data(), tooDeep.size(), &out) && out.empty(),
        "one level more is refused, delivering nothing");

  // The deepest single datagram there is: 3275 levels in 65,508 bytes.
  out.clear();
  const std::vector<uint8_t> datagram = nestedBundles(3275, 1);
  check(datagram.size() <= 65508 && !osc::decodePacket(datagram.data(), datagram.size(), &out) && out.empty(),
        "a 64 KB datagram of 3275 nested bundles is refused, not recursed into");

  // 200,000 levels: about 30 MB of stack before, past any thread's.
  out.clear();
  const std::vector<uint8_t> stream = nestedBundles(200000, 1);
  check(!osc::decodePacket(stream.data(), stream.size(), &out) && out.empty(),
        "a 4 MB packet nested 200,000 deep (a stream transport has no 64 KB ceiling) is refused, "
        "not a stack overflow");

  // Within the limit, many messages under many levels arrive in order, and a
  // failure leaves what the caller already had.
  std::vector<uint8_t> wide = nestedBundles(osc::detail::kMaxBundleDepth, 5000);
  out.assign(2, osc::Message());
  out[0].address = "/kept";
  const bool wideOk = osc::decodePacket(wide.data(), wide.size(), &out);
  check(wideOk && out.size() == 5002 && out[0].address == "/kept" && out[5001].address == "/",
        "5000 messages under 32 levels: all of them, appended after what was there");
  wide[wide.size() - 4] = 'x'; // the last message's type tags lose their ','
  out.resize(2);
  check(!osc::decodePacket(wide.data(), wide.size(), &out) && out.size() == 2 && out[0].address == "/kept",
        "...and when its last message is broken, none of the 5000, and what was there stays");
}

// ═══ MPE keyboard ═══════════════════════════════════════════════════════════

static bool isOrange(const gfx::PremulColour& p) { return p.r > 200 && p.g > 110 && p.g < 190 && p.b < 120; }

static int countOrange(const gfx::Bitmap& bmp) {
  int n = 0;
  for (int y = 0; y < bmp.height(); ++y)
    for (int x = 0; x < bmp.width(); ++x)
      if (isOrange(bmp.pixelAt(x, y))) ++n;
  return n;
}

static void render(gfx::MPEKeyboardComponent& kb, gfx::Bitmap& bmp) {
  bmp.clear(gfx::Colour(0, 0, 0, 255));
  gfx::Graphics g(bmp);
  kb.paint(g);
}

static void mpeKeyboard() {
  std::printf("\n[MPE keyboard]\n");
  using namespace sonore::gfx;
#if defined(__APPLE__)
  using Rect = sonore::gfx::Rect; // MacTypes.h has one too
#endif
  char msg[220];
  MPEKeyboardComponent kb;
  kb.setRange(60, 72); // C4..C5: eight white keys
  kb.setBounds({0.0f, 0.0f, 280.0f, 100.0f});
  check(std::fabs(kb.whiteKeyWidth() - 35.0f) < 0.01f, "eight white keys of 35 px");

  // Pitch to x: whole pitches are key centres, and it is monotonic.
  check(std::fabs(kb.xForPitch(60.0f) - 17.5f) < 0.01f && std::fabs(kb.xForPitch(62.0f) - 52.5f) < 0.01f,
        "C4 and D4 are at their key centres");
  bool monotonic = true;
  for (float p = 59.0f; p < 74.0f; p += 0.05f)
    if (kb.xForPitch(p + 0.05f) <= kb.xForPitch(p)) monotonic = false;
  check(monotonic, "x rises with pitch everywhere, across black keys and past both ends");
  bool inverse = true;
  for (float p = 58.5f; p < 74.5f; p += 0.25f)
    if (std::fabs(kb.pitchForX(kb.xForPitch(p)) - p) > 0.001f) inverse = false;
  check(inverse, "pitchForX inverts xForPitch");

  // The scripted controller: an MPE Configuration Message for a lower zone
  // of 15 (RPN 6 on channel 1), then a note on member channel 2 with its
  // expression sent first, as MPE orders it.
  MidiBuffer midi;
  midi.addEvent(MidiMessage::controlChange(0, 101, 0), 0);
  midi.addEvent(MidiMessage::controlChange(0, 100, 6), 0);
  midi.addEvent(MidiMessage::controlChange(0, 6, 15), 0);
  midi.addEvent(MidiMessage::pitchBend(1, 8192), 1);
  midi.addEvent(MidiMessage::controlChange(1, 74, 127), 1);
  midi.addEvent(MidiMessage::channelPressure(1, 0), 1);
  midi.addEvent(MidiMessage::noteOn(1, 60, 127), 2);
  kb.processMidi(midi);
  check(kb.numNotes() == 1, "one note sounding");
  const MPEKeyboardComponent::Note* n = kb.findNote(1, 60);
  check(n && std::fabs(n->timbre - 1.0f) < 1e-6f && n->pressure == 0.0f && n->bend == 0.0f,
        "it began with the timbre its channel was given BEFORE the note-on");

  Bitmap bmp(280, 100);
  render(kb, bmp);
  Rect circle = kb.noteCircle(*n);
  Point c = circle.centre();
  std::snprintf(msg, sizeof(msg), "the note is drawn at (%.1f, %.1f): the C key, at the top for full timbre",
                c.x, c.y);
  check(std::fabs(c.x - 17.5f) < 0.01f && c.y < 30.0f && isOrange(bmp.pixelAt((int) c.x, (int) c.y)), msg);
  const int smallArea = countOrange(bmp);

  // Bend it up two semitones: 8192 + 2/48 of the half range.
  MidiBuffer bend;
  bend.addEvent(MidiMessage::pitchBend(1, 8192 + (int) std::lround(2.0 / 48.0 * 8192.0)), 0);
  kb.processMidi(bend);
  n = kb.findNote(1, 60);
  check(n && std::fabs(n->bend - 2.0f) < 0.01f, "a bend of two semitones on its channel");
  render(kb, bmp);
  c = kb.noteCircle(*n).centre();
  std::snprintf(msg, sizeof(msg), "the circle moved to x=%.1f, over D", c.x);
  check(std::fabs(c.x - 52.5f) < 0.2f && isOrange(bmp.pixelAt((int) c.x, (int) c.y)), msg);
  check(!isOrange(bmp.pixelAt(17, (int) c.y)), "and is no longer over C");
  const PremulColour under = bmp.pixelAt(10, 90);
  std::snprintf(msg, sizeof(msg), "the key it was struck on stays lit underneath (%d,%d,%d)", under.r,
                under.g, under.b);
  check(under.r < 160 && under.b > under.r, msg);

  // Press: the circle swells.
  MidiBuffer press;
  press.addEvent(MidiMessage::channelPressure(1, 127), 0);
  kb.processMidi(press);
  render(kb, bmp);
  const int bigArea = countOrange(bmp);
  std::snprintf(msg, sizeof(msg), "full pressure grows the drawn note from %d to %d pixels", smallArea, bigArea);
  check(bigArea > smallArea * 4, msg);

  // Timbre down: the circle drops.
  MidiBuffer dark;
  dark.addEvent(MidiMessage::controlChange(1, 74, 0), 0);
  kb.processMidi(dark);
  n = kb.findNote(1, 60);
  check(n && kb.noteCircle(*n).centre().y > 70.0f, "timbre 0 puts it at the bottom");

  // A master-channel bend moves every note in the zone, on top of its own.
  MidiBuffer master;
  master.addEvent(MidiMessage::pitchBend(0, 8192 + 4096), 0); // +1 of the master's 2
  kb.processMidi(master);
  n = kb.findNote(1, 60);
  std::snprintf(msg, sizeof(msg), "a zone bend of +1 on top of the note's +2 sounds at %.2f",
                n ? kb.pitchOf(*n) : 0.0f);
  check(n && std::fabs(kb.pitchOf(*n) - 63.0f) < 0.01f && std::fabs(n->bend - 2.0f) < 0.01f, msg);
  // The finger moving under it: the note's own bend is the finger's alone,
  // and the zone's is still added once -- the decoder's tuning, which holds
  // both for a DSP, must not be taken for the note's own.
  MidiBuffer finger;
  finger.addEvent(MidiMessage::pitchBend(1, 8192 + (int) std::lround(3.0 / 48.0 * 8192.0)), 0);
  kb.processMidi(finger);
  n = kb.findNote(1, 60);
  std::snprintf(msg, sizeof(msg), "its own bend moving to +3 under the zone's +1 sounds at %.2f",
                n ? kb.pitchOf(*n) : 0.0f);
  check(n && std::fabs(kb.pitchOf(*n) - 64.0f) < 0.01f && std::fabs(n->bend - 3.0f) < 0.01f, msg);

  // A host's per-note expression (CLAP, VST3) without any MIDI.
  NoteExpressionBuffer expr;
  NoteExpressionBuffer::Entry e;
  e.key = 60;
  e.channel = 1;
  e.expression = kExprBrightness;
  e.value = 0.5f;
  expr.addEvent(e);
  kb.applyExpression(expr);
  check(kb.findNote(1, 60) && std::fabs(kb.findNote(1, 60)->timbre - 0.5f) < 1e-6f,
        "per-note expression from a host lands on the note");

  // ── The mouse ──
  Component root;
  root.setBounds({0.0f, 0.0f, 280.0f, 100.0f});
  root.addChild(&kb);
  MouseRouter router(root);
  std::vector<MidiMessage> sent;
  kb.onMpeMessage = [&](const MidiMessage& m) { sent.push_back(m); };
  int plainNotes = 0;
  kb.onNoteOn = [&](int, float) { ++plainNotes; };

  router.mouseDown({87.5f, 90.0f}); // E4, near the bottom
  check(sent.size() == 4, "a press sends four messages");
  if (sent.size() == 4) {
    const int ch = sent[3].getChannel() - 1;
    std::snprintf(msg, sizeof(msg), "on MIDI channel %d: channel 2 holds the host's note, 3 is the first free member",
                  ch + 1);
    check(ch == 2, msg);
    check(sent[0].isPitchWheel() && sent[0].getPitchWheelValue() == 8192 && sent[1].isController() &&
              sent[1].getControllerNumber() == 74 && sent[2].isChannelPressure() && sent[3].isNoteOn() &&
              sent[3].getNoteNumber() == 64,
          "bend, CC 74 and pressure BEFORE the note-on, as MPE orders them");
    check(sent[2].getChannelPressureValue() == 114, "pressure from the height: 90% down is 114");
    check(plainNotes == 0, "and the plain note callback stays silent while MPE is wired");
  }
  check(kb.findNote(2, 64) && kb.findNote(2, 64)->fromMouse, "the mouse note is drawn at once");

  sent.clear();
  router.mouseMove({122.5f, 90.0f}); // one white key right: E to F is one semitone
  int bendValue = -1;
  for (const auto& m : sent)
    if (m.isPitchWheel()) bendValue = m.getPitchWheelValue();
  std::snprintf(msg, sizeof(msg), "dragging one semitone right bends to %d (8192 + 8192/48 = 8363)", bendValue);
  check(bendValue == 8363, msg);
  check(kb.findNote(2, 64) && std::fabs(kb.findNote(2, 64)->bend - 1.0f) < 0.01f,
        "and the drawn note follows");

  sent.clear();
  router.mouseMove({122.5f, 50.0f}); // up: less pressure
  int pressure = -1;
  for (const auto& m : sent)
    if (m.isChannelPressure()) pressure = m.getChannelPressureValue();
  check(pressure == 64, "dragging up to the middle lowers pressure to 64");

  sent.clear();
  router.mouseUp({122.5f, 50.0f});
  check(sent.size() == 1 && sent[0].isNoteOff() && sent[0].getChannel() == 3 && sent[0].getNoteNumber() == 64,
        "release: one note-off, same channel, same key");
  check(!kb.findNote(2, 64), "and the note is gone");

  sent.clear();
  router.mouseDown({87.5f, 90.0f});
  check(sent.size() == 4 && sent[3].getChannel() - 1 == 3,
        "the next press takes the channel used longest ago, not the one just released");
  router.mouseUp({87.5f, 90.0f});

  // Timbre on the vertical instead.
  kb.setVerticalGesture(MPEKeyboardComponent::VerticalGesture::Timbre);
  sent.clear();
  router.mouseDown({87.5f, 90.0f});
  router.mouseMove({87.5f, 10.0f});
  int cc74 = -1;
  for (const auto& m : sent)
    if (m.isController() && m.getControllerNumber() == 74) cc74 = m.getControllerValue();
  check(cc74 > 110, "with the timbre gesture, dragging up raises CC 74");
  router.mouseUp({87.5f, 10.0f});

  // Unwired, it is a plain keyboard.
  kb.onMpeMessage = nullptr;
  router.mouseDown({87.5f, 90.0f});
  router.mouseUp({87.5f, 90.0f});
  check(plainNotes == 1, "without onMpeMessage the plain note callbacks fire");

  // Note-off and all-notes-off from the host.
  MidiBuffer off;
  off.addEvent(MidiMessage::noteOff(1, 60), 0);
  kb.processMidi(off);
  check(kb.numNotes() == 0, "the host's note-off removes its note");
  MidiBuffer many;
  for (int i = 0; i < 5; ++i) many.addEvent(MidiMessage::noteOn(1 + i, 60 + i, 100), 0);
  many.addEvent(MidiMessage::controlChange(0, 123, 0), 1); // all notes off on the master
  kb.processMidi(many);
  check(kb.numNotes() == 0, "all-notes-off on the master channel clears the zone");
  root.removeChild(&kb);
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("midi_osc_test\n");
  flexData();
  umpStream();
  sysex8();
  collector();
  oscPatterns();
  oscAddresses();
  oscColour();
  oscBundleDepth();
  mpeKeyboard();
  std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
  std::printf("%s\n", g_failures == 0 ? "MIDI-OSC TESTS PASSED" : "MIDI-OSC TESTS FAILED");
  return g_failures == 0 ? 0 : 1;
}
