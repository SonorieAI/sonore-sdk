// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the MIDI stack's portable half, against the inputs that broke it.
//
// Each section is a defect a review found with a probe, kept here so it stays
// found: the file reader, the sequence that feeds a block, the writer, the
// input queue, the UMP assembler and the filter in front of a DSP.
#include <sonore/audio_asio.h>
#include <sonore/midi_file.h>
#include <sonore/midi_input.h>
#include <sonore/midi_output.h>
#include <sonore/midi_sequence.h>
#include <sonore/ump.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32) && !defined(SONORE_APPLE_SYNTAX_CHECK)
namespace sonore {
namespace midiin {
/** The winmm Device's driver side, with a fake winmm: the callback is what
 *  the driver calls, and midiInAddBuffer is where a SysEx buffer goes back
 *  to it. The handle is never handed to winmm. */
struct DeviceTest {
  static int added;
  static MMRESULT WINAPI addBuffer(HMIDIIN, MIDIHDR*, UINT) {
    ++added;
    return MMSYSERR_NOERROR;
  }
  static void attach(Device& device, MessageQueue* queue) {
    device.queue_ = queue;
    device.handle_ = (HMIDIIN) (uintptr_t) 1;
    device.midiInAddBuffer_ = &addBuffer;
  }
  static void detach(Device& device) {
    device.handle_ = nullptr;
    device.midiInAddBuffer_ = nullptr;
  }
  static void deliver(Device& device, UINT msg, MIDIHDR* header) {
    Device::callback(nullptr, msg, (DWORD_PTR) &device, (DWORD_PTR) header, 0);
  }
};
int DeviceTest::added = 0;
} // namespace midiin
} // namespace sonore
#endif

using namespace sonore;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

static void writeBytes(const char* path, const std::vector<uint8_t>& bytes) {
  std::FILE* f = std::fopen(path, "wb");
  if (!f) return;
  std::fwrite(bytes.data(), 1, bytes.size(), f);
  std::fclose(f);
}

/** readMidiFile on a thread, given two seconds: a reader that spins for ever
 *  is reported and the process ends, rather than the test hanging until
 *  ctest's own timeout says nothing useful. */
static bool readWithin(const char* path, bool* result) {
  std::atomic<bool> done{false};
  bool ok = false;
  std::thread reader([&] {
    MidiFileData data;
    ok = readMidiFile(path, &data);
    done = true;
  });
  for (int i = 0; i < 200 && !done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  if (!done) {
    std::printf("  FAIL readMidiFile(%s) is still running after 2 s\n", path);
    std::fflush(stdout);
    std::_Exit(1);
  }
  reader.join();
  *result = ok;
  return true;
}

static void smfLengths() {
  std::printf("\n-- SMF chunk lengths, summed in 64 bits --\n");
  const std::vector<uint8_t> header = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, 2, 0x01, 0xE0};
  const std::vector<uint8_t> track = {'M', 'T', 'r', 'k', 0, 0, 0, 4, 0x00, 0xFF, 0x2F, 0x00};

  // An unknown chunk of 0xFFFFFFF8 bytes: `8 + length` in 32 bits is 0, and
  // the walk added nothing to its position, for ever.
  std::vector<uint8_t> junk = header;
  const uint8_t junkChunk[] = {'J', 'U', 'N', 'K', 0xFF, 0xFF, 0xFF, 0xF8};
  junk.insert(junk.end(), junkChunk, junkChunk + 8);
  junk.insert(junk.end(), track.begin(), track.end());
  writeBytes("midi_stack_junk.mid", junk);
  bool ok = true;
  readWithin("midi_stack_junk.mid", &ok);
  check(true, "an unknown chunk claiming 0xFFFFFFF8 bytes does not hang the reader");

  // The header's own length, the same way.
  std::vector<uint8_t> hdr = {'M', 'T', 'h', 'd', 0xFF, 0xFF, 0xFF, 0xF8, 0, 1, 0, 2, 0x01, 0xE0};
  hdr.insert(hdr.end(), track.begin(), track.end());
  writeBytes("midi_stack_hdr.mid", hdr);
  readWithin("midi_stack_hdr.mid", &ok);
  check(!ok, "nor does a header claiming 0xFFFFFFF8, and the file is refused");

  // A track that claims far more than the file holds is refused, not read.
  std::vector<uint8_t> big = header;
  const uint8_t bigTrack[] = {'M', 'T', 'r', 'k', 0xFF, 0xFF, 0xFF, 0xF0, 0x00, 0x90, 0x3C, 0x64};
  big.insert(big.end(), bigTrack, bigTrack + sizeof(bigTrack));
  writeBytes("midi_stack_big.mid", big);
  readWithin("midi_stack_big.mid", &ok);
  check(!ok, "a track claiming 0xFFFFFFF0 bytes of a 26-byte file is refused");

  std::remove("midi_stack_junk.mid");
  std::remove("midi_stack_hdr.mid");
  std::remove("midi_stack_big.mid");
}

static void sequenceBlocks() {
  std::printf("\n-- a sequence played block by block delivers each event once --\n");
  // Thirty-second notes for two minutes at a handful of tempi, rates and
  // block sizes: the grid the review found doubled and lost events on.
  const double bpms[] = {120.0, 100.0, 90.0};
  const uint16_t tpqs[] = {96, 480};
  const double rates[] = {44100.0, 48000.0};
  const uint32_t blocks[] = {64, 128, 512};
  long twice = 0, never = 0, configs = 0;
  static MidiBuffer buffer;
  for (double bpm : bpms)
    for (uint16_t tpq : tpqs) {
      MidiFileData file;
      file.ticksPerQuarter = tpq;
      file.tempoChanges = {{0, bpm}};
      MidiTrack t;
      const uint32_t step = tpq / 8;
      const uint32_t count = (uint32_t) (120.0 * bpm / 60.0 * 8);
      for (uint32_t i = 0; i < count; ++i)
        t.events.push_back(
            {i * step, MidiMessage::controlChange(0, (int) (i / 128 % 128), (int) (i % 128))});
      file.tracks.push_back(t);
      const MidiSequence seq = MidiSequence::fromMidiFile(file);
      for (double sr : rates)
        for (uint32_t n : blocks) {
          ++configs;
          std::vector<int> seen(seq.size(), 0);
          // As the standalone plays a file: a frame counter, block after block.
          for (uint64_t frame = 0; frame < (uint64_t) (121.0 * sr); frame += n) {
            buffer.clear();
            seq.fillBufferFrames(buffer, (int64_t) frame, n, sr);
            for (const auto& e : buffer)
              ++seen[(size_t) (e.getMessage().getControllerNumber() * 128 +
                               e.getMessage().getControllerValue())];
          }
          for (size_t i = 0; i < seen.size(); ++i) {
            twice += seen[i] > 1;
            never += seen[i] == 0;
          }
        }
    }
  std::printf("  ---- %ld configurations: %ld events delivered twice, %ld never ----\n", configs,
              twice, never);
  check(twice == 0 && never == 0, "every event in exactly one block, at every tempo, rate and size");

  // The seconds form, as a caller computing (frame / rate) passes it, agrees.
  MidiSequence seq;
  seq.addEvent(MidiMessage::noteOn(0, 60, 100), 0.6);
  int hits = 0;
  for (uint64_t frame = 0; frame < 48000; frame += 128) {
    buffer.clear();
    seq.fillBuffer(buffer, (double) frame / 48000.0, 128, 48000.0);
    hits += buffer.getNumEvents();
  }
  check(hits == 1, "a note at 0.6 s, played in seconds-addressed blocks, comes exactly once");
}

static void sequenceTempoMap() {
  std::printf("\n-- a file's tempo map, walked once --\n");
  // By hand: 480 ticks a quarter, 120 BPM, then 60 from tick 960. Tick 1440
  // is two quarters at 120 (1 s) and one at 60 (1 s); a change ON a tick
  // starts there, so tick 960 itself is still 1 s.
  {
    MidiFileData file;
    file.ticksPerQuarter = 480;
    file.tempoChanges = {{0, 120.0}, {960, 60.0}};
    MidiTrack t;
    t.events = {{1440, MidiMessage::noteOn(0, 60, 100)}, {960, MidiMessage::noteOn(0, 62, 100)},
                {240, MidiMessage::noteOn(0, 64, 100)}};
    file.tracks.push_back(t);
    const MidiSequence seq = MidiSequence::fromMidiFile(file);
    check(seq.size() == 3 && seq[0].time == 0.25 && seq[1].time == 1.0 && seq[2].time == 2.0,
          "ticks 240, 960 and 1440 across a change from 120 to 60 BPM are 0.25, 1 and 2 s");
  }
  // And to the bit what tickToSeconds says, on a map built by hand with
  // everything it forgives: out of order, two changes on one tick, a tempo
  // of zero, a change on an event's own tick and one past the last event.
  {
    MidiFileData file;
    file.ticksPerQuarter = 96;
    file.tempoChanges = {{0, 97.0},   {300, 131.5}, {200, 80.0}, {300, 75.25}, {500, 0.0},
                         {777, 61.0}, {1000, 240.0}, {90000, 33.0}};
    MidiTrack t;
    for (uint32_t tick = 0; tick < 2000; tick += 37) t.events.push_back({tick, MidiMessage::noteOn(0, 60, 1)});
    t.events.push_back({777, MidiMessage::noteOn(0, 61, 1)});
    file.tracks.push_back(t);
    const MidiSequence seq = MidiSequence::fromMidiFile(file);
    std::vector<double> want;
    for (const MidiFileEvent& e : t.events) want.push_back(file.tickToSeconds(e.tick));
    std::sort(want.begin(), want.end());
    bool same = seq.size() == want.size();
    for (size_t i = 0; same && i < want.size(); ++i) same = seq[i].time == want[i];
    check(same, "every event's time is exactly the one tickToSeconds gives");
  }
  // A file of 100,000 tempo changes and 100,000 notes, 1.1 MB: walking the
  // whole map for every event took 13 seconds here to open it.
  {
    std::vector<uint8_t> bytes = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xE0};
    std::vector<uint8_t> body;
    const int kEach = 100000;
    for (int i = 0; i < kEach; ++i) {
      const uint32_t us = 500000u + (uint32_t) (i % 7) * 1000u;
      const uint8_t tempo[] = {0x01, 0xFF, 0x51, 0x03, (uint8_t) (us >> 16), (uint8_t) (us >> 8),
                               (uint8_t) us};
      body.insert(body.end(), tempo, tempo + sizeof(tempo));
      const uint8_t note[] = {0x00, 0x90, 0x3C, (uint8_t) (i % 2 ? 0 : 0x40)};
      body.insert(body.end(), note, note + sizeof(note));
    }
    const uint8_t end[] = {0x00, 0xFF, 0x2F, 0x00};
    body.insert(body.end(), end, end + sizeof(end));
    const uint8_t chunk[] = {'M', 'T', 'r', 'k', (uint8_t) (body.size() >> 24), (uint8_t) (body.size() >> 16),
                             (uint8_t) (body.size() >> 8), (uint8_t) body.size()};
    bytes.insert(bytes.end(), chunk, chunk + sizeof(chunk));
    bytes.insert(bytes.end(), body.begin(), body.end());
    writeBytes("midi_stack_tempi.mid", bytes);
    MidiFileData file;
    const auto start = std::chrono::steady_clock::now();
    const bool read = readMidiFile("midi_stack_tempi.mid", &file);
    const MidiSequence seq = MidiSequence::fromMidiFile(file);
    const double took =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("  ---- %d tempo changes and %zu events opened in %.3f s ----\n", kEach, seq.size(),
                took);
    check(read && seq.size() == (size_t) kEach && file.tempoChanges.size() == (size_t) kEach,
          "a file of 100,000 tempo changes and notes reads whole");
    check(took < 2.0, "...and opens in well under the 13 s that a walk per event took");
    check(read && std::fabs(seq[seq.size() - 1].time - file.tickToSeconds((uint32_t) kEach)) < 1e-9,
          "...with its last note where tickToSeconds puts it");
    std::remove("midi_stack_tempi.mid");
  }
}

static void inputQueue() {
  std::printf("\n-- the input queue: SysEx that waits, and what reaches a DSP --\n");
  // Two 1500-byte SysEx in one block: a block's arena holds 2048 bytes, so
  // one fits now and the other must wait -- not vanish uncounted.
  static midiin::MessageQueue queue;
  std::vector<uint8_t> big(1500, 0x11);
  big.front() = 0xF0;
  big.back() = 0xF7;
  int accepted = 0;
  for (int i = 0; i < 4; ++i) accepted += queue.pushSysex(big.data(), big.size()) ? 1 : 0;
  check(accepted == 4, "four SysEx messages fit in the ring, as its comment promises");
  static MidiBuffer block;
  int delivered = 0;
  for (int b = 0; b < 6; ++b) {
    block.clear();
    queue.drain(block);
    delivered += block.getNumSysex();
  }
  char msg[160];
  std::snprintf(msg, sizeof(msg),
                "all four are delivered over the next blocks (%d), none dropped (%u)", delivered,
                queue.droppedSysex());
  check(delivered == 4 && queue.droppedSysex() == 0, msg);

  // Undefined status bytes are not messages.
  check(!deliverableToDsp(0xF4) && !deliverableToDsp(0xF5) && !deliverableToDsp(0xF7),
        "0xF4, 0xF5 and a stray 0xF7 are not handed to a DSP");
  check(deliverableToDsp(0xF1) && deliverableToDsp(0xF3) && deliverableToDsp(0xF6) &&
            deliverableToDsp(0xF8) && deliverableToDsp(0x90),
        "MTC, song select, tune request, clock and a note still are");
  queue.push(0xF5, 0x3C, 0x00);
  queue.push(0x90, 60, 100);
  block.clear();
  queue.drain(block);
  check(block.getNumEvents() == 1 && block.begin()->getMessage().isNoteOn(),
        "a queued 0xF5 is skipped and the note behind it arrives");
}

static void smfWriter() {
  std::printf("\n-- the SMF writer: a tempo map, and only what a file can hold --\n");
  MidiFileData file;
  file.ticksPerQuarter = 480;
  file.tempoChanges = {{0, 120.0}, {1920, 60.0}}; // 120 for a bar of 4/4, then 60
  MidiTrack track;
  track.events.push_back({0, MidiMessage::noteOn(0, 60, 100)});
  track.events.push_back({2880, MidiMessage::noteOn(0, 62, 100)}); // bar 2, beat 3
  track.events.push_back({2880, MidiMessage(0xF8, 0, 0)});         // a clock: not a file event
  track.events.push_back({2880, MidiMessage(0xFF, 0, 0)});         // a reset: neither
  track.events.push_back({3360, MidiMessage::noteOff(0, 62)});
  file.tracks.push_back(track);
  check(writeMidiFile("midi_stack_write.mid", file), "a file with a tempo map writes");

  MidiFileData back;
  const bool read = readMidiFile("midi_stack_write.mid", &back);
  std::remove("midi_stack_write.mid");
  check(read && back.tracks.size() == 1, "and reads back");
  if (!read || back.tracks.empty()) return;
  std::printf("  ---- tempo changes read back:");
  for (const auto& t : back.tempoChanges) std::printf(" (%u, %.1f)", t.tick, t.bpm);
  std::printf(" ----\n");
  check(back.tempoChanges.size() == 2 && back.tempoChanges[1].tick == 1920 &&
            std::fabs(back.tempoChanges[1].bpm - 60.0) < 1e-6,
        "the change to 60 BPM is at tick 1920, where it was written, not at tick 0");
  // Four quarters at 120 are two seconds, then two quarters at 60 two more:
  // four. With both changes at tick 0 the whole file ran at 60, and it was six.
  const double at = back.tickToSeconds(2880);
  char msg[120];
  std::snprintf(msg, sizeof(msg), "so the note at tick 2880 plays at %.3f s, not 6", at);
  check(std::fabs(at - 4.0) < 1e-6, msg);
  check(back.tracks[0].events.size() == 3,
        "the clock and the reset are left out: three channel events come back, nothing else");

  // A tempo the 24-bit field cannot hold is the nearest one it can. 2 BPM
  // was cut to its low 24 bits and came back as 4.54; 0.5 BPM came back as
  // 120. The slowest a file can say is 60,000,000 / 16,777,215 BPM.
  MidiFileData slow;
  slow.tempoChanges = {{0, 2.0}, {480, 0.5}, {960, 90.0}};
  slow.tracks.push_back(track);
  check(writeMidiFile("midi_stack_slow.mid", slow), "a file with tempos slower than 3.58 BPM writes");
  MidiFileData slowBack;
  const bool slowRead = readMidiFile("midi_stack_slow.mid", &slowBack);
  std::remove("midi_stack_slow.mid");
  const double slowest = 60000000.0 / 16777215.0;
  std::snprintf(msg, sizeof(msg), "...and reads back at the slowest a file can say (%.3f and %.3f BPM)",
                slowRead && slowBack.tempoChanges.size() == 3 ? slowBack.tempoChanges[0].bpm : -1.0,
                slowRead && slowBack.tempoChanges.size() == 3 ? slowBack.tempoChanges[1].bpm : -1.0);
  check(slowRead && slowBack.tempoChanges.size() == 3 &&
            std::fabs(slowBack.tempoChanges[0].bpm - slowest) < 1e-9 &&
            std::fabs(slowBack.tempoChanges[1].bpm - slowest) < 1e-9 &&
            std::fabs(slowBack.tempoChanges[2].bpm - 90.0) < 1e-3,
        msg);
}

static void smfLongDeltas() {
  std::printf("\n-- the SMF writer: a delta wider than four VLQ bytes --\n");
  // A delta above 0x0FFFFFFF needs five 7-bit groups, and the format (and
  // readVarLen) stops at four: the file was written and could not be read.
  MidiFileData file;
  MidiTrack track;
  const uint32_t ticks[] = {0, 0x0FFFFFFFu, 0x10000004u, 0x30000010u, 0xFFFFFFFFu};
  for (uint32_t t : ticks) track.events.push_back({t, MidiMessage::noteOn(0, 60, 100)});
  file.tracks.push_back(track);
  file.tempoChanges = {{0, 120.0}, {0x20000000u, 90.0}};
  check(writeMidiFile("midi_stack_long.mid", file), "a file with deltas past 0x0FFFFFFF writes");
  MidiFileData back;
  const bool read = readMidiFile("midi_stack_long.mid", &back);
  std::remove("midi_stack_long.mid");
  check(read, "...and reads back");
  bool same = read && back.tracks.size() == 1 && back.tracks[0].events.size() == 5;
  std::string got;
  char item[32];
  for (size_t i = 0; same && i < 5; ++i) {
    std::snprintf(item, sizeof(item), " %08X", back.tracks[0].events[i].tick);
    got += item;
    same = back.tracks[0].events[i].tick == ticks[i] && back.tracks[0].events[i].message.isNoteOn();
  }
  char msg[160];
  std::snprintf(msg, sizeof(msg), "every note at the tick it was written at (%s ), and only the notes",
                got.c_str());
  check(same, msg);
  check(read && back.tempoChanges.size() == 2 && back.tempoChanges[1].tick == 0x20000000u,
        "the tempo change between them keeps its tick too");
}

/** True when `s` is well-formed UTF-8 (no stray continuation, no truncated
 *  sequence, nothing over-long past what a two-byte lead allows). */
static bool validUtf8(const std::string& s) {
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = (unsigned char) s[i];
    const size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
    if (n == 0 || (n == 2 && c < 0xC2) || i + n > s.size()) return false;
    for (size_t k = 1; k < n; ++k)
      if ((((unsigned char) s[i + k]) & 0xC0) != 0x80) return false;
    i += n;
  }
  return true;
}

static void portNamesUtf8() {
#if defined(_WIN32) && !defined(SONORE_APPLE_SYNTAX_CHECK)
  std::printf("\n-- winmm and ASIO names come back as UTF-8 --\n");
  // What enumerate() and listDevices() now do with a caps struct's name.
  // Each UTF-16 unit was cast to char: this name came back as
  // 43 61 66 E9 20 F3 7D 20 3C B9 -- an E9 no UTF-8 reader takes, and one
  // unrelated byte per CJK character and per surrogate.
  MIDIOUTCAPSW caps{};
  const wchar_t name[] = L"Café 音楽 \U0001F3B9";
  std::memcpy(caps.szPname, name, sizeof(name));
  const std::string got = pathio::narrow(caps.szPname, MAXPNAMELEN);
  check(got == "Caf\xC3\xA9 \xE9\x9F\xB3\xE6\xA5\xBD \xF0\x9F\x8E\xB9",
        "an accented, a CJK and an astral name read as the UTF-8 they spell");
  // A driver may fill the field to the end; the name stops there and does
  // not run on into the struct's next member.
  MIDIINCAPSW full{};
  for (int k = 0; k < MAXPNAMELEN; ++k) full.szPname[k] = (WCHAR) (L'A' + k % 26);
  full.dwSupport = 0x00420042;
  check(pathio::narrow(full.szPname, MAXPNAMELEN).size() == (size_t) MAXPNAMELEN,
        "a name with no terminator is the field's 32 units, and no more");
  // And whatever this machine has, every name in every list is UTF-8.
  bool allUtf8 = true;
  size_t count = 0;
  for (const std::string& n : midiout::Device::enumerate()) allUtf8 = allUtf8 && validUtf8(n), ++count;
  for (const std::string& n : midiin::Device::listDevices()) allUtf8 = allUtf8 && validUtf8(n), ++count;
  for (const asio::DriverInfo& d : asio::listDrivers()) allUtf8 = allUtf8 && validUtf8(d.name), ++count;
  char msg[120];
  std::snprintf(msg, sizeof(msg), "every MIDI port and ASIO driver here (%zu) is named in UTF-8", count);
  check(allUtf8, msg);
#endif
}

/** What a parser made of a byte string, as text: "[F1 01] [F8] {F0 01 F7}". */
static std::string parse(midiin::StreamParser& parser, const std::vector<uint8_t>& bytes) {
  std::string out;
  char item[64];
  for (uint8_t b : bytes)
    parser.feed(
        b,
        [&](uint8_t s, uint8_t d1, uint8_t d2) {
          const int len = shortMessageLength(s);
          if (len == 1) std::snprintf(item, sizeof(item), "[%02X] ", s);
          else if (len == 2) std::snprintf(item, sizeof(item), "[%02X %02X] ", s, d1);
          else std::snprintf(item, sizeof(item), "[%02X %02X %02X] ", s, d1, d2);
          out += item;
        },
        [&](const uint8_t* data, size_t n) {
          out += "{";
          for (size_t i = 0; i < n; ++i) {
            std::snprintf(item, sizeof(item), i ? " %02X" : "%02X", data[i]);
            out += item;
          }
          out += "} ";
        });
  return out;
}

static void streamParser() {
  std::printf("\n-- the byte-stream parser behind ALSA and CoreMIDI --\n");
  struct Case {
    std::vector<uint8_t> in;
    const char* want;
    const char* what;
  };
  const Case cases[] = {
      {{0xF1, 0x01, 0xF1, 0x12, 0xF1, 0x23, 0xF8}, "[F1 01] [F1 12] [F1 23] [F8] ",
       "MTC quarter frames are two bytes each, and come out"},
      {{0xF3, 0x05, 0xF6, 0xF2, 0x10, 0x20}, "[F3 05] [F6] [F2 10 20] ",
       "song select, tune request and song position, each at its own length"},
      {{0xF0, 0x7E, 0x01, 0xF7, 0x3E, 0x64}, "{F0 7E 01 F7} ",
       "data bytes after a SysEx are not an [F7 xx yy] event"},
      {{0xF0, 0x01, 0xF8, 0x02, 0xF7}, "[F8] {F0 01 02 F7} ",
       "a clock inside a SysEx passes through, and the SysEx is whole"},
      {{0xF0, 0x01, 0xF7, 0x90, 0x3C, 0x64}, "{F0 01 F7} [90 3C 64] ",
       "a note after a SysEx's F7 in the same run arrives"},
      {{0x90, 0x3C, 0x64, 0x3E, 0x64}, "[90 3C 64] [90 3E 64] ", "running status still works"},
      {{0x90, 0x3C, 0x64, 0xF1, 0x01, 0x3E, 0x64}, "[90 3C 64] [F1 01] ",
       "and a system common message cancels it"},
      {{0xF4, 0x01, 0x02, 0xF5, 0x90, 0x3C, 0x64}, "[90 3C 64] ",
       "0xF4 and 0xF5 are nothing, and what follows them is not theirs"},
      {{0xF0, 0x01, 0x02, 0x90, 0x3C, 0x64}, "[90 3C 64] ",
       "a status byte that cuts a SysEx short still starts its own message"},
  };
  for (const Case& c : cases) {
    midiin::StreamParser parser;
    const std::string got = parse(parser, c.in);
    char msg[220];
    std::snprintf(msg, sizeof(msg), "%s: %s", c.what, got.c_str());
    check(got == c.want, msg);
  }

  // Two sources, two parsers: one keyboard's note in the middle of another
  // device's SysEx, split across packets, loses neither.
  midiin::StreamParser a, b;
  std::string got = parse(a, {0xF0, 0x01, 0x02});
  got += parse(b, {0x90, 0x3C, 0x64});
  got += parse(a, {0x03, 0xF7});
  check(got == "[90 3C 64] {F0 01 02 03 F7} ",
        "two sources interleaved: the note and the whole SysEx both arrive");
}

static void outputReopen() {
  std::printf("\n-- a MIDI output opened twice --\n");
  const std::vector<std::string> ports = midiout::Device::enumerate();
  if (ports.empty()) {
    std::printf("  skip no MIDI output on this machine\n");
    return;
  }
  // A second open() overwrote the handle and leaked the first: on a port
  // that takes one client -- the GS Wavetable Synth -- the second then failed.
  midiout::Device out;
  const bool first = out.open(0);
  // A listed port that does not open is not this bug: a CI runner lists the
  // GS Wavetable Synth with no audio device behind it, and Linux lists an
  // ALSA port with no rawmidi device. The reopen is only testable after one
  // open has succeeded.
  if (!first) {
    std::printf("  skip \"%s\" is listed but does not open here (%s)\n", ports[0].c_str(),
                out.error().c_str());
    return;
  }
  const bool second = out.open(0);
  char msg[200];
  std::snprintf(msg, sizeof(msg), "opening \"%s\" twice succeeds both times (%d, %d: %s)",
                ports[0].c_str(), (int) first, (int) second, out.error().c_str());
  check(first && second, msg);
  out.close();
}

static void staleOpenError() {
  std::printf("\n-- an open that succeeds after one that failed --\n");
  // error() is empty while healthy, and open() never cleared it: a port that
  // opened after a refusal went on reporting the refusal.
  const std::vector<std::string> ports = midiout::Device::enumerate();
  if (ports.empty()) {
    std::printf("  skip no MIDI output on this machine\n");
  } else {
    midiout::Device out;
    const bool refused = !out.open(-1);
    const std::string why = out.error();
    const bool opened = out.open(0);
    char msg[240];
    std::snprintf(msg, sizeof(msg), "output: open(-1) is refused (\"%s\"), then open(0) on \"%s\" "
                  "succeeds with error() empty (\"%s\")", why.c_str(), ports[0].c_str(),
                  out.error().c_str());
    if (opened) {
      check(refused && !why.empty() && out.error().empty(), msg);
      // A silent message (all notes off) the driver takes: nothing dropped,
      // and close() has nothing to report.
      for (int i = 0; i < 4; ++i) out.send(MidiMessage::controlChange(0, 123, 0), 0.0);
      const uint64_t lost = out.droppedEvents();
      out.close();
      check(lost == 0 && out.error().empty(),
            "four messages the port takes: none dropped, and error() is still empty after close()");
    } else {
      std::printf("  skip \"%s\" would not open: %s\n", ports[0].c_str(), out.error().c_str());
    }
    out.close();
  }
  // An input refuses nothing by index -- a bad one means the first device --
  // so the refusal is a second client on a port that takes one. Needs a MIDI
  // input; the machine this was written on has none, so it was compiled and
  // reasoned, not run, for inputs.
  const std::vector<std::string> inputs = midiin::Device::listDevices();
  if (inputs.empty()) {
    std::printf("  skip no MIDI input on this machine\n");
    return;
  }
  static midiin::MessageQueue q1, q2;
  midiin::Device first, second;
  const bool held = first.open(&q1, 0);
  if (!held || second.open(&q2, 0)) {
    std::printf("  skip \"%s\" takes two clients (or none): nothing refuses\n", inputs[0].c_str());
    return;
  }
  const std::string why = second.error();
  first.close();
  const bool opened = second.open(&q2, 0);
  char msg[240];
  std::snprintf(msg, sizeof(msg), "input: a second client is refused (\"%s\"), and once the first "
                "lets go it opens with error() empty (\"%s\")", why.c_str(), second.error().c_str());
  check(!why.empty() && opened && second.error().empty(), msg);
  second.close();
}

/** A rawmidi buffer as snd_rawmidi_write sees it on a NON-blocking handle:
 *  room for so many bytes, then -EAGAIN (-11 on Linux), never a wait. */
struct FakeRawmidi {
  size_t room = 0;
  int calls = 0;
  std::string wire;
  static long write(void* self, const void* data, size_t n) {
    FakeRawmidi& f = *(FakeRawmidi*) self;
    ++f.calls;
    if (f.room == 0) return -11;
    const size_t take = n < f.room ? n : f.room;
    f.wire.append((const char*) data, take);
    f.room -= take;
    return (long) take;
  }
};

static void portNeverWaits() {
  std::printf("\n-- a MIDI port whose buffer is full --\n");
  // The Linux output opened its rawmidi handle BLOCKING, and send() runs on
  // the audio thread: a full buffer parked the callback inside
  // snd_rawmidi_write. The handle is non-blocking now, and this is what
  // send() does with the answer -- the Linux Device itself needs libasound,
  // so its open mode was checked against a fake one off the tree.
  FakeRawmidi port;
  port.room = 7; // two whole note-ons and one byte of a third
  std::atomic<uint64_t> dropped{0};
  const uint8_t on[3] = {0x90, 0x3C, 0x64};
  const bool a = midiout::detail::writeShortMessage(&FakeRawmidi::write, &port, on, 3, dropped);
  const bool b = midiout::detail::writeShortMessage(&FakeRawmidi::write, &port, on, 3, dropped);
  const uint64_t afterTwo = dropped.load();
  const bool c = midiout::detail::writeShortMessage(&FakeRawmidi::write, &port, on, 3, dropped);
  const bool d = midiout::detail::writeShortMessage(&FakeRawmidi::write, &port, on, 3, dropped);
  check(a && b && afterTwo == 0 && port.wire.size() == 7,
        "two messages that fit go, and nothing is counted");
  check(!c && !d && dropped.load() == 2,
        "one cut short and one refused with EAGAIN are each counted as dropped");
  check(port.calls == 4, "...and each was ONE call: no retry, no spin on the audio thread");
  check(midiout::detail::droppedReport(2).find("2 MIDI message(s)") == 0,
        "close() reports the count in error(), as the file sink does for its ring");
}

static void liveFileCount() {
  std::printf("\n-- a live file sink counted while it drains --\n");
  // numEvents() read events_.size() while the sink's drain thread grew the
  // vector: a torn read of its pointers, and counts like 18446744073709360294
  // or one that went BACKWARDS, from a call its comment invited mid-run. A
  // race is not certain to show in one run; this one showed in most runs of
  // 400000 events before the count was published as an atomic of its own.
  long long reads = 0, torn = 0;
  bool finalRight = true;
  for (int run = 0; run < 8; ++run) {
    midiout::FileSink sink("midi_stack_live.mid");
    sink.startLive(1u << 16);
    std::atomic<bool> done{false};
    std::atomic<size_t> sent{0};
    std::thread producer([&] {
      for (int i = 0; i < 400000; ++i) {
        sink.send(MidiMessage::noteOn(0, 60, 100), i * 1e-5);
        sent.store((size_t) i + 1, std::memory_order_release);
      }
      done.store(true, std::memory_order_release);
    });
    size_t last = 0;
    while (!done.load(std::memory_order_acquire)) {
      const size_t n = sink.numEvents();
      const size_t ceiling = sent.load(std::memory_order_acquire);
      ++reads;
      if (n < last || n > ceiling) ++torn;
      else last = n;
    }
    producer.join();
    sink.close();
    std::remove("midi_stack_live.mid");
    if (sink.numEvents() + sink.droppedEvents() != 400000) finalRight = false;
  }
  char msg[200];
  std::snprintf(msg, sizeof(msg),
                "numEvents() never goes backwards or past what was sent while the drain runs "
                "(%lld reads, %lld torn)", reads, torn);
  check(torn == 0, msg);
  check(finalRight, "and once closed, kept plus dropped is every event sent");
}

static void umpGroups() {
  std::printf("\n-- SysEx7 on two UMP groups, interleaved --\n");
  // Groups are separate cables, and their SysEx7 streams may interleave
  // packet by packet. One shared buffer stitched two messages into one.
  std::vector<uint8_t> m0 = {0xF0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 0xF7};
  std::vector<uint8_t> m1 = {0xF0, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 0xF7};
  std::vector<std::vector<uint32_t>> p0, p1;
  ump::toSysex7(m0.data(), m0.size(),
                [&](const uint32_t* w, size_t n) { p0.emplace_back(w, w + n); }, 0);
  ump::toSysex7(m1.data(), m1.size(),
                [&](const uint32_t* w, size_t n) { p1.emplace_back(w, w + n); }, 1);
  static ump::Sysex7Assembler assembler;
  std::vector<std::vector<uint8_t>> out;
  std::vector<int> groups;
  for (size_t i = 0; i < p0.size() || i < p1.size(); ++i) {
    if (i < p0.size() && assembler.push(p0[i].data(), p0[i].size())) {
      out.emplace_back(assembler.data(), assembler.data() + assembler.size());
      groups.push_back(assembler.group());
    }
    if (i < p1.size() && assembler.push(p1[i].data(), p1[i].size())) {
      out.emplace_back(assembler.data(), assembler.data() + assembler.size());
      groups.push_back(assembler.group());
    }
  }
  char msg[160];
  std::snprintf(msg, sizeof(msg), "two messages come out whole (%zu, %zu bytes), each on its group",
                out.size() > 0 ? out[0].size() : 0, out.size() > 1 ? out[1].size() : 0);
  check(out.size() == 2 && out[0] == m0 && out[1] == m1 && groups[0] == 0 && groups[1] == 1, msg);
}

/** One SysEx7 packet on group 0 by hand, so its status and byte count can be
 *  ones toSysex7 would never write. */
static void sysex7Packet(uint32_t* w, uint8_t status, uint8_t count, const uint8_t* bytes) {
  w[0] = (0x3u << 28) | ((uint32_t) status << 20) | ((uint32_t) count << 16);
  w[1] = 0;
  for (int i = 0; i < 6 && i < count; ++i) {
    if (i < 2) w[0] |= (uint32_t) bytes[i] << (8 * (1 - i));
    else w[1] |= (uint32_t) bytes[i] << (8 * (3 - (i - 2)));
  }
}

static void umpMalformedSysex7() {
  std::printf("\n-- SysEx7: a malformed packet inside an open message --\n");
  const uint8_t head[6] = {0x7D, 1, 2, 3, 4, 5};
  const uint8_t junk[6] = {0x66, 0x66, 0x66, 0x66, 0x66, 0x66};
  const uint8_t tail[6] = {6, 7, 8, 9, 10, 11};
  uint32_t w[2];
  char msg[200];

  // Status nibbles 4 to F are reserved. The assembler appended such a
  // packet's bytes to the message open on its group: 20 bytes came out where
  // 14 went in, six of them nothing the sender meant as part of it.
  static ump::Sysex7Assembler assembler;
  assembler.reset();
  sysex7Packet(w, ump::kSysexStart, 6, head);
  assembler.push(w, 2);
  sysex7Packet(w, 0x5, 6, junk);
  const bool reservedDone = assembler.push(w, 2);
  sysex7Packet(w, ump::kSysexEnd, 6, tail);
  const bool done = assembler.push(w, 2);
  std::vector<uint8_t> want = {0xF0};
  want.insert(want.end(), head, head + 6);
  want.insert(want.end(), tail, tail + 6);
  want.push_back(0xF7);
  const std::vector<uint8_t> got(assembler.data(), assembler.data() + assembler.size());
  std::snprintf(msg, sizeof(msg),
                "a reserved-status packet adds nothing to the open message (%zu bytes, want %zu)",
                got.size(), want.size());
  check(!reservedDone && done && got == want, msg);

  // A continue claiming seven bytes -- six is the most a packet holds -- was
  // skipped, and the end then finished the message without it: handed on
  // whole-looking, with a hole where that packet was.
  assembler.reset();
  sysex7Packet(w, ump::kSysexStart, 6, head);
  assembler.push(w, 2);
  sysex7Packet(w, ump::kSysexContinue, 7, junk);
  assembler.push(w, 2);
  sysex7Packet(w, ump::kSysexEnd, 6, tail);
  const bool holed = assembler.push(w, 2);
  std::snprintf(msg, sizeof(msg),
                "a message whose middle packet was malformed is not handed on (%s, %u dropped)",
                holed ? "handed on" : "refused", assembler.dropped());
  check(!holed && assembler.dropped() == 1 && !assembler.inProgress(), msg);

  // And the group is usable straight after: the next message is whole.
  sysex7Packet(w, ump::kSysexStart, 6, head);
  assembler.push(w, 2);
  sysex7Packet(w, ump::kSysexEnd, 6, tail);
  check(assembler.push(w, 2) &&
            std::vector<uint8_t>(assembler.data(), assembler.data() + assembler.size()) == want,
        "...and the next message on that group comes out whole");
}

static void umpStatusBytes() {
  std::printf("\n-- UMP: the message type and the status byte agree --\n");
  // Message type 2 is MIDI 1.0 CHANNEL voice, statuses 0x80..0xEF; type 1
  // is system common and realtime. toMidi1 and toSystem checked only the
  // type, so a type-1 packet carrying 0x93 came back as a note-on, and a
  // type-2 packet carrying 0xF8 as a clock: messages made out of packets
  // that are not them, which both functions say they refuse.
  MidiMessage m;
  const bool noteFromSystem = ump::toSystem(0x10933C64u, &m);
  char msg[200];
  std::snprintf(msg, sizeof(msg), "a type-1 packet with status 0x93 is refused (%s 0x%02X)",
                noteFromSystem ? "read as" : "not read", m.getRawStatus());
  check(!noteFromSystem, msg);
  check(!ump::toMidi1(0x20F80000u, &m) && !ump::toMidi1(0x20003C64u, &m) &&
            !ump::toMidi1(0x20F03C64u, &m),
        "a type-2 packet with status 0xF8, 0x00 or 0xF0 is refused");
  check(!ump::toSystem(0x10F40000u, &m) && !ump::toSystem(0x10F70000u, &m) &&
            !ump::toSystem(0x10F00000u, &m),
        "and a type-1 packet with 0xF4, 0xF7 or 0xF0, which is no system message");

  // And the other way: fromMidi1 wrote a clock as a type-2 packet, which a
  // UMP receiver reads as a malformed channel message, never as a clock.
  const uint32_t clock = ump::fromMidi1(MidiMessage(0xF8, 0, 0), 5);
  uint8_t groupOut = 0;
  check(ump::messageType(clock) == ump::kMtSystem && ump::toSystem(clock, &m, &groupOut) &&
            m.getRawStatus() == 0xF8 && groupOut == 5,
        "fromMidi1 of a clock writes a type-1 packet, on its group, that reads back as a clock");
  const uint32_t spp = ump::fromMidi1(MidiMessage(0xF2, 0x10, 0x20));
  check(ump::toSystem(spp, &m) && m.getRawStatus() == 0xF2 && m.getRawData1() == 0x10 &&
            m.getRawData2() == 0x20,
        "...and a song position pointer keeps both its data bytes");
  // What every channel message did, it still does.
  bool channelsSurvive = true;
  for (int st = 0x80; st <= 0xEF; ++st) {
    const MidiMessage in(st, 0x7F, 0x01);
    const uint32_t w = ump::fromMidi1(in, 3);
    MidiMessage out;
    channelsSurvive = channelsSurvive && ump::messageType(w) == ump::kMtMidi1ChannelVoice &&
                      ump::toMidi1(w, &out) && out.getRawStatus() == st &&
                      out.getRawData1() == 0x7F && out.getRawData2() == 0x01;
  }
  check(channelsSurvive, "every channel status still makes the round trip as type 2");
}

static void winmmCutShortSysex() {
#if defined(_WIN32) && !defined(SONORE_APPLE_SYNTAX_CHECK)
  std::printf("\n-- winmm: a SysEx the driver saw cut short --\n");
  // A SysEx interrupted by a status byte comes back as MIM_LONGERROR, in the
  // buffer it was being collected in -- the application's again, to lend
  // back. The callback only answered MIM_LONGDATA, so each one cost a
  // buffer: after two, winmm held none and threw every SysEx away in
  // silence, MIDI-CI included, until the input was reopened.
  static midiin::MessageQueue queue;
  static midiin::Device device;
  midiin::DeviceTest::added = 0;
  midiin::DeviceTest::attach(device, &queue);
  uint8_t bytes[2][1024] = {};
  MIDIHDR headers[2] = {};
  for (int i = 0; i < 2; ++i) {
    const uint8_t cut[] = {0xF0, 0x7D, 0x01, 0x02};
    std::memcpy(bytes[i], cut, sizeof(cut));
    headers[i].lpData = (LPSTR) bytes[i];
    headers[i].dwBufferLength = sizeof(bytes[i]);
    headers[i].dwBytesRecorded = sizeof(cut);
    midiin::DeviceTest::deliver(device, MIM_LONGERROR, &headers[i]);
  }
  char msg[200];
  std::snprintf(msg, sizeof(msg),
                "both buffers of two cut-short SysEx are lent back to the driver (%d of 2)",
                midiin::DeviceTest::added);
  check(midiin::DeviceTest::added == 2, msg);

  // What they held is not a message; the next whole one is, on its own.
  const uint8_t whole[] = {0xF0, 0x7D, 0x05, 0x06, 0xF7};
  std::memcpy(bytes[0], whole, sizeof(whole));
  headers[0].dwBytesRecorded = sizeof(whole);
  midiin::DeviceTest::deliver(device, MIM_LONGDATA, &headers[0]);
  static MidiBuffer block;
  block.clear();
  queue.drain(block);
  check(block.getNumSysex() == 1 && block.sysexBegin()->length == sizeof(whole) &&
            std::memcmp(block.sysexData(*block.sysexBegin()), whole, sizeof(whole)) == 0,
        "the cut-short ones deliver nothing, and the whole SysEx after them arrives as sent");
  midiin::DeviceTest::detach(device);
#endif
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("midi_stack_test\n");
  umpGroups();
  umpMalformedSysex7();
  umpStatusBytes();
  winmmCutShortSysex();
  outputReopen();
  staleOpenError();
  portNeverWaits();
  liveFileCount();
  smfWriter();
  smfLongDeltas();
  portNamesUtf8();
  streamParser();
  smfLengths();
  sequenceBlocks();
  sequenceTempoMap();
  inputQueue();
  std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
