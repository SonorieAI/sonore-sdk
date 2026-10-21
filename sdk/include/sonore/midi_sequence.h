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
// notes of the same pitch overlap on the same channel (a legato repeat, or a
// sustain pedal part written badly), and then a naive "find the next note-off
// for this pitch" gives the first note the second note's ending and leaves the
// second unmatched for ever.
#pragma once

#include <algorithm>
#include <cmath>
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
  double time = 0.0;   ///< Seconds from the start, never negative.
  MidiMessage message; ///< The message itself.
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
  /** No events. */
  void clear() {
    events_.clear();
    sorted_ = true;
  }

  /** How many events it holds. */
  size_t size() const { return events_.size(); }
  /** True with no events. */
  bool empty() const { return events_.empty(); }
  /** Event `index`, which must be below size(): not checked. In time order
   *  only after sort(). */
  const SequenceEvent& operator[](size_t index) const { return events_[index]; }
  /** Every event, in time order only after sort(). The reference lasts until
   *  the next change. */
  const std::vector<SequenceEvent>& events() const { return events_; }

  /** Add `message` at `time` seconds, a negative time taken as 0. It goes at
   *  the end, unpaired: sort() and updateMatchedPairs() put it in its place. */
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
   *  swapping them turns a repeated note into a stuck one.
   *
   *  A pair is an INDEX, and sorting moves what it points at, so the pairs
   *  move with their events. They did not: one controller added before a
   *  paired note shifted everything after it by one, and the note-on's
   *  pairedWith then named the note-on itself -- noteLength() said 0 for a
   *  note a second long, and every other pair named its neighbour. Events
   *  added since the last updateMatchedPairs() stay unpaired until the next,
   *  as they were before the sort. */
  void sort() {
    if (sorted_) return;
    std::vector<size_t> order(events_.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [this](size_t a, size_t b) {
      return events_[a].time < events_[b].time;
    });
    std::vector<int> movedTo(events_.size());
    for (size_t k = 0; k < order.size(); ++k) movedTo[order[k]] = (int) k;
    std::vector<SequenceEvent> sorted;
    sorted.reserve(events_.size());
    for (size_t k = 0; k < order.size(); ++k) {
      SequenceEvent event = events_[order[k]];
      if (event.pairedWith >= 0 && (size_t) event.pairedWith < movedTo.size())
        event.pairedWith = movedTo[(size_t) event.pairedWith];
      sorted.push_back(event);
    }
    events_.swap(sorted);
    sorted_ = true;
  }

  /** The last event's time in seconds, sorting first; 0 when empty. */
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
   *
   * Safe on the audio thread: it never sorts and never allocates. It used to
   * sort first, and stable_sort allocates its scratch whenever the timeline
   * was edited since the last sort -- in process(), where the standalone
   * plays a MIDI file through this. A sorted timeline is searched; an
   * unsorted one is scanned whole, which gives the same block, because
   * MidiBuffer::addEvent orders by offset and keeps ties in the order they
   * arrive -- the order the stable sort would have kept.
   */
  void fillBuffer(MidiBuffer& out, double blockStart, uint32_t numSamples,
                  double sampleRate) const {
    if (sampleRate <= 0.0 || numSamples == 0) return;
    fillBufferFrames(out, (int64_t) std::llround(blockStart * sampleRate), numSamples, sampleRate);
  }

  /**
   * The same, with the block given in whole FRAMES: [startFrame, startFrame
   * + numSamples), which is what a caller counting samples should pass.
   *
   * Every event belongs to frame floor(time * sampleRate), worked out the
   * same way for every block. The window used to be in seconds -- this
   * block's end was start + n/rate, and the caller's next start was
   * (F + n)/rate -- and the two differ in the last bit. An event in that
   * sliver was delivered twice or never: at 120 BPM, 480 ticks a quarter,
   * 48 kHz and 128-frame blocks, 209 of 7200 events came twice, and at 100
   * BPM some never came at all. A doubled note-on or a lost note-off is a
   * stuck note. In frames the blocks tile exactly, whatever their sizes.
   */
  void fillBufferFrames(MidiBuffer& out, int64_t startFrame, uint32_t numSamples,
                        double sampleRate) const {
    if (sampleRate <= 0.0 || numSamples == 0) return;
    const double first = (double) startFrame;
    const double end = (double) (startFrame + (int64_t) numSamples);
    auto frameOf = [sampleRate](const SequenceEvent& e) { return e.time * sampleRate; };
    auto emit = [&](const SequenceEvent& e) {
      out.addEvent(e.message, (int) ((int64_t) std::floor(frameOf(e)) - startFrame));
    };
    if (!sorted_) {
      for (const SequenceEvent& e : events_) {
        const double at = frameOf(e);
        if (at >= first && at < end) emit(e);
      }
      return;
    }
    // Times are sorted and the rate is positive, so frameOf is too.
    size_t lo = 0, hi = events_.size();
    while (lo < hi) {
      const size_t mid = (lo + hi) / 2;
      if (frameOf(events_[mid]) < first)
        lo = mid + 1;
      else
        hi = mid;
    }
    for (size_t i = lo; i < events_.size() && frameOf(events_[i]) < end; ++i) emit(events_[i]);
  }

  /** Move every note by `semitones`, leaving everything else alone.
   *
   *  Notes that would fall outside 0..127 are DROPPED rather than clamped:
   *  clamping piles a transposed run onto one pitch at the top of the range,
   *  which is a chord nobody wrote.
   *
   *  Poly pressure moves with them: it names its note by number, so left
   *  behind it pressed a key nobody was holding -- or, in a chord, the note
   *  that had moved onto the old pitch from below. */
  void transpose(int semitones) {
    std::vector<SequenceEvent> kept;
    kept.reserve(events_.size());
    for (SequenceEvent& event : events_) {
      const MidiMessage& m = event.message;
      if (!isNoteOn(m) && !isNoteOff(m) && !m.isPolyPressure()) {
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
    // The tempo map is walked ONCE, into the seconds at each change, and each
    // event looks its change up. tickToSeconds() walks the whole map per call,
    // and per event that is events times changes: a 1 MB file of 100,000 of
    // each took 13 seconds to open, and one of 4 MB several minutes, with
    // nothing to show for it. The walk is tickToSeconds()'s own -- the same
    // skips, the same sums in the same order -- so every time is the one it
    // gives, to the bit.
    struct Change {
      uint32_t tick;
      double seconds, bpm;
    };
    std::vector<Change> map;
    map.reserve(file.tempoChanges.size());
    const double ticks = file.ticksPerQuarter > 0 ? (double) file.ticksPerQuarter : 480.0;
    {
      double seconds = 0.0, bpm = 120.0;
      uint32_t last = 0;
      for (const MidiTempoEvent& t : file.tempoChanges) {
        if (t.tick < last) continue; // out of order: tickToSeconds skips it too
        seconds += ((double) (t.tick - last) / ticks) * (60.0 / bpm);
        bpm = t.bpm > 0.0 ? t.bpm : bpm;
        last = t.tick;
        map.push_back({last, seconds, bpm});
      }
    }
    auto secondsAt = [&](uint32_t tick) {
      // The last change strictly BEFORE the tick: one on it starts there, so
      // the time of the tick itself is still the old tempo's.
      size_t lo = 0, hi = map.size();
      while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (map[mid].tick < tick)
          lo = mid + 1;
        else
          hi = mid;
      }
      if (lo == 0) return ((double) tick / ticks) * (60.0 / 120.0);
      const Change& c = map[lo - 1];
      return c.seconds + ((double) (tick - c.tick) / ticks) * (60.0 / c.bpm);
    };

    MidiSequence sequence;
    for (const MidiTrack& track : file.tracks)
      for (const MidiFileEvent& event : track.events)
        sequence.addEvent(event.message, secondsAt(event.tick));
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
