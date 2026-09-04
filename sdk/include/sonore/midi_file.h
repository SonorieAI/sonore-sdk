// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: Standard MIDI Files.
//
// The interchange format for patterns: what an arpeggiator exports, what a
// chord generator imports, what a user drags out of a DAW and into a plugin.
// Reading and writing SMF format 0 and 1.
//
// Two things in this format break every naive parser, and both are handled
// explicitly below:
//
//   VARIABLE-LENGTH QUANTITIES: delta times are 7 bits per byte with the top
//   bit meaning "another byte follows". Assuming one byte works on short
//   patterns and silently derails on anything longer than 127 ticks.
//
//   RUNNING STATUS: a data byte where a status byte was expected means
//   "reuse the previous status". A parser that does not implement it reads
//   garbage from the moment a file uses it, which most real files do.
//
// Offline by design, like wav.h: this allocates and does file IO.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "audio.h"

namespace sonore {

/** One event at an absolute position, in ticks from the start of its track. */
struct MidiFileEvent {
  uint32_t tick = 0;
  MidiMessage message;
};

/** A tempo change, so a reader can convert ticks to seconds honestly rather
 *  than assuming 120 BPM the way a lazy importer does. */
struct MidiTempoEvent {
  uint32_t tick = 0;
  double bpm = 120.0;
};

struct MidiTrack {
  std::string name;
  std::vector<MidiFileEvent> events; // sorted by tick
};

struct MidiFileData {
  /** Ticks per quarter note. The other SMF timing mode (SMPTE frames) is
   *  vanishingly rare in musical files and is refused rather than
   *  misinterpreted as a tick count. */
  uint16_t ticksPerQuarter = 480;
  std::vector<MidiTrack> tracks;
  std::vector<MidiTempoEvent> tempoChanges;

  /** Seconds at a given tick, following the tempo map. With no tempo events
   *  the file means 120 BPM, which is what the specification says. */
  double tickToSeconds(uint32_t tick) const {
    const double ticks = ticksPerQuarter > 0 ? (double) ticksPerQuarter : 480.0;
    double seconds = 0.0;
    double bpm = 120.0;
    uint32_t last = 0;
    for (const MidiTempoEvent& t : tempoChanges) {
      if (t.tick >= tick) break;
      seconds += ((double) (t.tick - last) / ticks) * (60.0 / bpm);
      bpm = t.bpm > 0.0 ? t.bpm : bpm;
      last = t.tick;
    }
    seconds += ((double) (tick - last) / ticks) * (60.0 / bpm);
    return seconds;
  }
};

namespace midifile {

// ── Variable-length quantities ───────────────────────────────────────────────

inline void writeVarLen(std::vector<uint8_t>& out, uint32_t value) {
  // Build the 7-bit groups back to front, then emit them big end first with
  // the continuation bit set on all but the last.
  uint8_t buffer[5];
  int count = 0;
  buffer[count++] = (uint8_t) (value & 0x7f);
  value >>= 7;
  while (value > 0 && count < 5) {
    buffer[count++] = (uint8_t) ((value & 0x7f) | 0x80);
    value >>= 7;
  }
  while (count > 0) out.push_back(buffer[--count]);
}

inline bool readVarLen(const uint8_t* data, size_t size, size_t& pos, uint32_t* out) {
  uint32_t value = 0;
  for (int i = 0; i < 4; ++i) { // 4 bytes max: 28 bits, per the specification
    if (pos >= size) return false;
    const uint8_t byte = data[pos++];
    value = (value << 7) | (uint32_t) (byte & 0x7f);
    if ((byte & 0x80) == 0) {
      *out = value;
      return true;
    }
  }
  return false; // a fifth continuation byte is a corrupt file
}

inline void writeU16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back((uint8_t) (v >> 8));
  out.push_back((uint8_t) (v & 0xff));
}

inline void writeU32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back((uint8_t) (v >> 24));
  out.push_back((uint8_t) ((v >> 16) & 0xff));
  out.push_back((uint8_t) ((v >> 8) & 0xff));
  out.push_back((uint8_t) (v & 0xff));
}

inline uint16_t readU16(const uint8_t* p) { return (uint16_t) ((p[0] << 8) | p[1]); }

inline uint32_t readU32(const uint8_t* p) {
  return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) |
         (uint32_t) p[3];
}

/** How many data bytes a channel status takes. Program change and channel
 *  pressure are TWO-byte messages; treating everything as three is the other
 *  classic way a parser slides out of alignment. */
inline int dataBytesFor(uint8_t status) {
  const uint8_t high = status & 0xf0;
  return (high == 0xC0 || high == 0xD0) ? 1 : 2;
}

/** Parse one track chunk into events, following running status. */
inline bool parseTrack(const uint8_t* data, size_t size, MidiTrack* track,
                       std::vector<MidiTempoEvent>* tempos) {
  size_t pos = 0;
  uint32_t tick = 0;
  uint8_t runningStatus = 0;

  while (pos < size) {
    uint32_t delta = 0;
    if (!readVarLen(data, size, pos, &delta)) return false;
    tick += delta;
    if (pos >= size) return false;

    uint8_t status = data[pos];
    if (status & 0x80) {
      ++pos;
      // System messages clear running status; channel messages set it.
      if (status < 0xf0) runningStatus = status;
    } else {
      if (runningStatus == 0) return false; // data with no status ever seen
      status = runningStatus;
    }

    if (status == 0xff) { // meta event
      if (pos >= size) return false;
      const uint8_t type = data[pos++];
      uint32_t length = 0;
      if (!readVarLen(data, size, pos, &length)) return false;
      if (pos + length > size) return false;
      if (type == 0x51 && length == 3) {
        const uint32_t microseconds =
            ((uint32_t) data[pos] << 16) | ((uint32_t) data[pos + 1] << 8) | data[pos + 2];
        if (microseconds > 0 && tempos)
          tempos->push_back({tick, 60000000.0 / (double) microseconds});
      } else if (type == 0x03 && length > 0) { // track name
        track->name.assign((const char*) data + pos, length);
      } else if (type == 0x2f) {
        return true; // end of track, and anything after it is padding
      }
      pos += length;
      continue;
    }

    if (status == 0xf0 || status == 0xf7) { // sysex: skipped, never guessed at
      uint32_t length = 0;
      if (!readVarLen(data, size, pos, &length)) return false;
      if (pos + length > size) return false;
      pos += length;
      continue;
    }

    const int dataBytes = dataBytesFor(status);
    if (pos + (size_t) dataBytes > size) return false;
    const uint8_t d1 = data[pos++];
    const uint8_t d2 = dataBytes == 2 ? data[pos++] : 0;
    track->events.push_back({tick, MidiMessage(status, d1, d2)});
  }
  return true;
}

} // namespace midifile

/** Read a Standard MIDI File. Returns false on anything malformed rather than
 *  producing a half-parsed sequence. */
inline bool readMidiFile(const char* path, MidiFileData* out) {
  if (!path || !out) return false;
  std::FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size < 14) {
    std::fclose(f);
    return false;
  }
  std::vector<uint8_t> data((size_t) size);
  const bool ok = std::fread(data.data(), 1, (size_t) size, f) == (size_t) size;
  std::fclose(f);
  if (!ok || std::memcmp(data.data(), "MThd", 4) != 0) return false;

  using namespace midifile;
  const uint32_t headerLength = readU32(data.data() + 4);
  if (headerLength < 6 || 8 + headerLength > data.size()) return false;
  const uint16_t division = readU16(data.data() + 12);
  // The top bit means SMPTE timing, which is not a tick count and must not be
  // read as one.
  if (division & 0x8000) return false;

  out->ticksPerQuarter = division ? division : 480;
  out->tracks.clear();
  out->tempoChanges.clear();

  size_t pos = 8 + headerLength;
  while (pos + 8 <= data.size()) {
    const uint32_t chunkLength = readU32(data.data() + pos + 4);
    if (std::memcmp(data.data() + pos, "MTrk", 4) != 0) {
      // An unknown chunk is skipped by its declared length, which is exactly
      // what the specification asks for.
      pos += 8 + chunkLength;
      continue;
    }
    if (pos + 8 + chunkLength > data.size()) return false;
    MidiTrack track;
    if (!parseTrack(data.data() + pos + 8, chunkLength, &track, &out->tempoChanges)) return false;
    out->tracks.push_back(std::move(track));
    pos += 8 + chunkLength;
  }
  return !out->tracks.empty();
}

/** Write a Standard MIDI File: format 0 for one track, format 1 for several.
 *  Events are assumed sorted by tick; anything out of order would produce a
 *  negative delta, so it is clamped rather than wrapped. */
inline bool writeMidiFile(const char* path, const MidiFileData& file) {
  if (!path || file.tracks.empty()) return false;
  using namespace midifile;

  std::vector<uint8_t> out;
  out.insert(out.end(), {'M', 'T', 'h', 'd'});
  writeU32(out, 6);
  writeU16(out, file.tracks.size() > 1 ? 1 : 0);
  writeU16(out, (uint16_t) file.tracks.size());
  writeU16(out, file.ticksPerQuarter ? file.ticksPerQuarter : 480);

  for (size_t t = 0; t < file.tracks.size(); ++t) {
    const MidiTrack& track = file.tracks[t];
    std::vector<uint8_t> body;

    if (!track.name.empty()) {
      writeVarLen(body, 0);
      body.push_back(0xff);
      body.push_back(0x03);
      writeVarLen(body, (uint32_t) track.name.size());
      body.insert(body.end(), track.name.begin(), track.name.end());
    }

    // Tempo belongs on the first track, which is where every host looks.
    if (t == 0)
      for (const MidiTempoEvent& tempo : file.tempoChanges) {
        writeVarLen(body, 0); // written at the top; a tempo MAP would interleave
        body.push_back(0xff);
        body.push_back(0x51);
        body.push_back(3);
        const uint32_t microseconds =
            (uint32_t) (60000000.0 / (tempo.bpm > 1.0 ? tempo.bpm : 120.0));
        body.push_back((uint8_t) ((microseconds >> 16) & 0xff));
        body.push_back((uint8_t) ((microseconds >> 8) & 0xff));
        body.push_back((uint8_t) (microseconds & 0xff));
      }

    uint32_t last = 0;
    for (const MidiFileEvent& e : track.events) {
      const uint32_t delta = e.tick > last ? e.tick - last : 0;
      writeVarLen(body, delta);
      last = e.tick > last ? e.tick : last;
      const uint8_t status = (uint8_t) e.message.getRawStatus();
      body.push_back(status);
      body.push_back((uint8_t) e.message.getRawData1());
      // Status is written every time: running status would save bytes and buy
      // a whole class of compatibility bug for a file nobody measures.
      if (dataBytesFor(status) == 2) body.push_back((uint8_t) e.message.getRawData2());
    }

    writeVarLen(body, 0);
    body.push_back(0xff);
    body.push_back(0x2f);
    body.push_back(0x00);

    out.insert(out.end(), {'M', 'T', 'r', 'k'});
    writeU32(out, (uint32_t) body.size());
    out.insert(out.end(), body.begin(), body.end());
  }

  std::FILE* f = std::fopen(path, "wb");
  if (!f) return false;
  const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  return ok;
}

} // namespace sonore
