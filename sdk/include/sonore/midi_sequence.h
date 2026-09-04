// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: MIDI as a timeline rather than as one block's worth.
//
// MidiBuffer is what a plugin sees: the events inside THIS block, offset in
// samples from its start. That is the right shape for processing and the
// wrong one for everything else. An arpeggiator pattern, a MIDI file being
// played back, a phrase an editor is drawing: all of them are a whole piece
// of music at once, in seconds, and none of them fits in a buffer that ends
// after 128 samples.
//
// The half that is genuinely awkward is PAIRING. A note-on and its note-off
// are two unrelated events in a flat list, and almost everything worth doing
// needs them joined: how long is this note, what is sounding at bar 3, drag
// this note later and take its end with it. Pairing looks trivial until two
// notes of the same pitch overlap on the same channel: a legato repeat, or a
// sustain pedal part written badly, and then a naive "find the next note-off
// for this pitch" gives the first note the second note's ending and leaves the
// second unmatched for ever.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "audio.h"
#include "midi_file.h"

namespace sonore {

/** One message at a point in time, in SECONDS from the start of the sequence.
 *
 *  Seconds, not ticks and not samples. Ticks need a tempo map to mean
 *  anything and samples need a rate, and a sequence outlives both -- the
 *  session's rate can change under it and the music must not move. */
struct SequenceEvent {
  double time = 0.0;
  MidiMessage message;
  /** Index of the note-off that ends this note-on, or -1.
   *
   *  -1 on a note-on means the note never ends, which is a real thing a file
   *  can contain and must not be silently repaired into a length of zero. */
  int pairedWith = -1;
};

/**
 * A piece of music, in order.
 *
 * Kept sorted by time. Insertion is allowed to break that, and everything
 * that depends on order calls sort() first rather than assuming -- a
 * sequence that is quietly out of order gives an event iterator that skips
 * things, which looks like dropped notes and is diagnosed as anything else.
 */
class MidiSequence {
public:
  void clear() {
    events_.clear();
    sorted_ = true;
  }

  size_t size() const { return events_.size(); }
  bool empty() const { return events_.empty(); }
  const SequenceEvent& operator[](size_t index) const { return events_[index]; }
  const std::vector<SequenceEvent>& events() const { return events_; }

  void addEvent(const MidiMessage& message, double time) {
    SequenceEvent event;
    event.time = time < 0.0 ? 0.0 : time;
    event.message = message;
    events_.push_back(event);
    sorted_ = false;
  }

  /** Stable, so two events at the same instant keep the order they were
   *  added in. That matters more than it looks: a note-off and the note-on
   *  that reuses the same pitch land on the same tick constantly, and
   *  swapping them turns a repeated note into a stuck one. */
  void sort() {
    if (sorted_) return;
    std::stable_sort(events_.begin(), events_.end(),
                     [](const SequenceEvent& a, const SequenceEvent& b) { return a.time < b.time; });
    sorted_ = true;
  }

  double duration() {
    sort();
    return events_.empty() ? 0.0 : events_.back().time;
  }

  /**
   * Join every note-on to the note-off that ends it.
   *
   * The rule that makes overlapping notes work: a note-off ends the OLDEST
   * unmatched note-on of that pitch and channel. The obvious alternative --
   * each note-on takes the next note-off it can find -- gives the first note
   * the second note's ending when two of the same pitch overlap, and leaves
   * the second unmatched for ever.
   *
   * A note-on with velocity zero IS a note-off; the format has said so since
   * 1983 and files rely on it heavily, because running status makes it
   * cheaper than a real note-off.
   */
  void updateMatchedPairs() {
    sort();
    for (SequenceEvent& event : events_) event.pairedWith = -1;

    // One queue of pending note-ons per pitch and channel, oldest first.
    std::vector<std::vector<int>> pending((size_t) 16 * 128);
    for (size_t i = 0; i < events_.size(); ++i) {
      const MidiMessage& m = events_[i].message;
      // getChannel() is ONE-BASED -- MIDI convention, and stated as such where
      // it is defined. Indexing a 16x128 table with it directly runs one whole
      // channel past the end on channel 16, which is a buffer overflow that
      // would sit there until somebody used the drum channel.
      const int channel = m.getChannel() - 1;
      const int note = m.getNoteNumber();
      if (channel < 0 || channel > 15 || note < 0 || note > 127) continue;
      const size_t slot = (size_t) channel * 128 + (size_t) note;

      if (isNoteOn(m)) {
        pending[slot].push_back((int) i);
      } else if (isNoteOff(m)) {
        if (pending[slot].empty()) continue; // an ending with no beginning
        const int onIndex = pending[slot].front();
        pending[slot].erase(pending[slot].begin());
        events_[(size_t) onIndex].pairedWith = (int) i;
      }
    }
  }

  /** How long a note lasts, or -1 if it never ends.
   *
   *  Minus one rather than zero: a note that runs off the end of the
   *  sequence is a different thing from a note of no length, and a caller
   *  drawing them needs to tell them apart. */
  double noteLength(size_t index) const {
    if (index >= events_.size()) return -1.0;
    const int paired = events_[index].pairedWith;
    if (paired < 0 || (size_t) paired >= events_.size()) return -1.0;
    return events_[(size_t) paired].time - events_[index].time;
  }

  /** The first event at or after `time`. size() if there is none. */
  size_t indexAtOrAfter(double time) {
    sort();
    size_t lo = 0, hi = events_.size();
    while (lo < hi) {
      const size_t mid = (lo + hi) / 2;
      if (events_[mid].time < time)
        lo = mid + 1;
      else
        hi = mid;
    }
    return lo;
  }

  /**
   * Fill a MidiBuffer with the events in one audio block.
   *
   * The bridge between a timeline and a plugin: `blockStart` is where this
   * block begins in the sequence's own seconds, and every event inside it
   * comes out with a sample offset a plugin can use.
   *
   * Half-open on purpose -- [blockStart, blockStart + length). An event
   * exactly on the boundary belongs to the block that STARTS there, and
   * including it in both is how a note gets played twice at every block
   * boundary it happens to land on.
   */
  void fillBuffer(MidiBuffer& out, double blockStart, uint32_t numSamples, double sampleRate) {
    if (sampleRate <= 0.0 || numSamples == 0) return;
    sort();
    const double blockEnd = blockStart + (double) numSamples / sampleRate;
    for (size_t i = indexAtOrAfter(blockStart); i < events_.size(); ++i) {
      if (events_[i].time >= blockEnd) break;
      int offset = (int) ((events_[i].time - blockStart) * sampleRate);
      if (offset < 0) offset = 0;
      if (offset >= (int) numSamples) offset = (int) numSamples - 1;
      out.addEvent(events_[i].message, offset);
    }
  }

  /** Move every note by `semitones`, leaving everything else alone.
   *
   *  Notes that would fall outside 0..127 are DROPPED rather than clamped:
   *  clamping piles a transposed run onto one pitch at the top of the range,
   *  which is a chord nobody wrote. */
  void transpose(int semitones) {
    std::vector<SequenceEvent> kept;
    kept.reserve(events_.size());
    for (SequenceEvent& event : events_) {
      const MidiMessage& m = event.message;
      if (!isNoteOn(m) && !isNoteOff(m)) {
        kept.push_back(event);
        continue;
      }
      const int note = m.getNoteNumber() + semitones;
      if (note < 0 || note > 127) continue;
      SequenceEvent moved = event;
      moved.message = MidiMessage(m.getRawStatus(), note, m.getRawData2());
      moved.pairedWith = -1;
      kept.push_back(moved);
    }
    events_.swap(kept);
    updateMatchedPairs();
  }

  /**
   * Everything in a Standard MIDI File, as one timeline in seconds.
   *
   * The tracks are merged, because a plugin has one MIDI input and a file's
   * track layout is a convenience for the editor that wrote it. The tempo map
   * is followed rather than assumed: a file with a tempo change in it comes
   * out at the right times, and one with no tempo events means 120 BPM
   * because that is what the specification says.
   */
  static MidiSequence fromMidiFile(const MidiFileData& file) {
    MidiSequence sequence;
    for (const MidiTrack& track : file.tracks)
      for (const MidiFileEvent& event : track.events)
        sequence.addEvent(event.message, file.tickToSeconds(event.tick));
    sequence.updateMatchedPairs();
    return sequence;
  }

private:
  // MidiMessage already gets the velocity-zero rule right -- isNoteOn() is
  // false for a 0x90 with velocity 0 and isNoteOff() is true for it. Spelled
  // out here anyway rather than assumed, because pairing is exactly where
  // getting it wrong produces stuck notes and it is worth being able to see
  // that the question was asked.
  static bool isNoteOn(const MidiMessage& m) { return m.isNoteOn(); }
  static bool isNoteOff(const MidiMessage& m) { return m.isNoteOff(); }

  std::vector<SequenceEvent> events_;
  bool sorted_ = true;
};

} // namespace sonore
