// SPDX-License-Identifier: Apache-2.0
//
// MIDI clock: tempo and transport from the wire.
//
// ── Why this exists ─────────────────────────────────────────────────────────
//
// A plugin inside a DAW is told the tempo: the host fills in a transport
// structure and every wrapper here passes it through. A plugin driven from a
// MIDI cable is told nothing. It receives a single byte, 0xF8, twenty-four
// times per quarter note, and whatever it wants to know about tempo it has to
// work out from how fast they arrive.
//
// That is the case this covers: a hardware sequencer, a drum machine, another
// computer, or the standalone build being clocked by any of them.
//
// ── Estimating a tempo from arrival times ───────────────────────────────────
//
// The naive reading (one interval, converted directly) is unusable. MIDI
// clock is jittery by construction: it travels over a serial link at 31250
// baud, it queues behind whatever else is being sent, and USB adds its own
// millisecond of scheduling. A tempo read from single intervals jumps by
// several BPM continuously, and a delay time computed from it warbles.
//
// So the estimate is smoothed, and the amount of smoothing is the entire
// design decision here:
//
//   - Too little, and the tempo audibly wanders while the source is steady.
//   - Too much, and a real tempo change takes seconds to follow, which is
//     worse: the sequencer has already moved and the plugin has not.
//
// A one-pole over the last couple of dozen clocks lands close to a quarter
// note of averaging: enough that jitter disappears, short enough that a
// deliberate change arrives within a beat.
//
// What a one-pole cannot do is tell a glitch from a change. One dropped byte
// doubles an interval and one duplicate splits it, and at 120 BPM those read
// 60 and 240 -- well inside any sane tempo range, and each pulled the
// estimate by several BPM. So a reading more than a quarter away from the
// current tempo is held back: a glitch happens once, a real change repeats,
// and four consistent readings in a row are believed outright.
//
// ── Not a replacement for host transport ────────────────────────────────────
//
// When a host provides a transport, believe the host. It knows the tempo
// exactly, it knows the bar position, and it is not guessing from arrival
// times. This is for when there is nothing better.
#pragma once

#include <cmath>
#include <cstdint>

#include "audio.h"

namespace sonore {

/**
 * Tempo and transport, derived from the realtime bytes in a MIDI stream and
 * the Song Position Pointer that goes with them.
 *
 * Call process() once per block with that block's MIDI and its length. Nothing
 * allocates and nothing blocks; this is safe on the audio thread.
 */
class MidiClock {
public:
  /** Twenty-four clocks to the quarter note. Fixed by the MIDI specification,
   *  not a preference. */
  static constexpr int kClocksPerQuarter = 24;

  /** [audio-thread] Back to stopped, no tempo heard (bpm() is 120 until
   *  one is) and position 0: for a transport that jumped or a source that
   *  was swapped. */
  void reset() {
    running_ = false;
    haveTempo_ = false;
    haveLastClock_ = false;
    bpm_ = 120.0;
    framesSinceClock_ = 0.0;
    ppq_ = 0.0;
    clocks_ = 0;
    songClocks_ = 0;
    outliers_ = 0;
    outlierSum_ = 0.0;
    sawStart_ = false;
  }

  /** The sample rate, and only that: what the clock has heard is kept.
   *
   *  The transport is the SEQUENCER's, and a host preparing the plugin again
   *  -- a new rate or block size, a restart for a latency change -- has not
   *  stopped it. Forgetting it here left a playing sequencer reading as
   *  stopped, its position held at 0 while the clocks kept coming, until
   *  somebody stopped and started the sequencer itself. The time since the
   *  last clock is carried into the new rate's frames, so the next interval
   *  still reads as a tempo. reset() is the call that forgets. */
  void prepare(double sampleRate) {
    const double rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    framesSinceClock_ *= rate / sampleRate_;
    sampleRate_ = rate;
  }

  /**
   * One block.
   *
   * Events are read at their sample offsets, so a clock arriving late in a
   * block is timed from there rather than from the block boundary. At 24 PPQ
   * and 120 BPM the clocks are 20 ms apart and a 128-frame block is under
   * 3 ms, so ignoring the offset would be a tenth of an interval of error --
   * small, but it is free to be right.
   */
  void process(const MidiBuffer& midi, uint32_t numFrames) {
    uint32_t consumed = 0;
    for (const auto& entry : midi) {
      const int at = entry.samplePosition < 0
                         ? 0
                         : (entry.samplePosition > (int) numFrames ? (int) numFrames
                                                                  : entry.samplePosition);
      advance((double) at - (double) consumed);
      consumed = (uint32_t) at;
      handle(entry.getMessage());
    }
    advance((double) numFrames - (double) consumed);
  }

  /** Beats per minute. 120 until enough clocks have arrived to know better --
   *  a musically useless default is worse than a conventional one. */
  double bpm() const { return bpm_; }

  /** Whether a tempo has actually been MEASURED, as opposed to assumed. A DSP
   *  that would rather stay silent than sync to a guess can ask. */
  bool hasTempo() const { return haveTempo_; }

  /** Whether the sequencer is playing: set by Start and Continue, cleared by
   *  Stop. */
  bool isRunning() const { return running_; }

  /** Position in quarter notes from the top of the song -- from the last
   *  Start, or from where a Song Position Pointer put it -- advancing between
   *  clocks rather than stepping at each one: a DSP asking mid-block wants
   *  where the music is now, not where the last clock byte was.
   *
   *  Counted in CLOCKS, which are the position: the first clock after Start
   *  is 0 and each one after it is a 24th of a quarter on. The tempo only
   *  fills in between two of them, and never past the next one. Integrated
   *  from the tempo alone, the position was off by whatever the estimate got
   *  wrong and kept it: a change from 120 to 80 BPM, held back for four
   *  clocks as a possible glitch, left it two clocks ahead for good, and
   *  every change after added its own. */
  double ppqPosition() const { return ppq_; }

  /** How many clock bytes have been seen since the last Start. */
  uint64_t clockCount() const { return clocks_; }

private:
  void handle(const MidiMessage& m) {
    switch (m.getRawStatus()) {
      case 0xF8: { // Clock
        // An interval is between two CLOCKS. The time since prepare() or
        // Start is not one: 15 ms into a 120 BPM stream it read 167 BPM, and
        // the first reading is taken whole.
        if (haveLastClock_ && framesSinceClock_ > 1.0) {
          const double perQuarter = framesSinceClock_ * kClocksPerQuarter;
          const double measured = 60.0 * sampleRate_ / perQuarter;
          // Outside 20..400 BPM the reading is far more likely to be a glitch
          // than a tempo anyone has chosen.
          if (measured > 20.0 && measured < 400.0) take(measured);
        }
        haveLastClock_ = true;
        framesSinceClock_ = 0.0;
        ++clocks_;
        // A stopped sequencer keeps sending clock, so a tempo is there at
        // once when it starts; only a running one moves the position.
        if (running_) {
          ppq_ = (double) songClocks_ / kClocksPerQuarter;
          ++songClocks_;
        }
        break;
      }
      case 0xFA: // Start: from the top, at the next clock.
        running_ = true;
        sawStart_ = true;
        ppq_ = 0.0;
        clocks_ = 0;
        songClocks_ = 0;
        framesSinceClock_ = 0.0;
        haveLastClock_ = false;
        break;
      case 0xFB: // Continue: from where Stop left it, or the pointer moved it.
        running_ = true;
        break;
      case 0xF2: // Song Position Pointer
        // Where the next clock plays, in sixteenths (six clocks each), 14
        // bits low byte first. A sequencer started anywhere but the top
        // sends this and then Continue -- every play from bar 9 -- and the
        // position went on from wherever the last Stop had left it.
        songClocks_ = (uint64_t) ((m.getRawData2() << 7) | m.getRawData1()) * 6;
        ppq_ = (double) songClocks_ / kClocksPerQuarter;
        break;
      case 0xFC: // Stop.
        running_ = false;
        break;
      default:
        break;
    }
  }

  void take(double measured) {
    if (!haveTempo_) {
      // The FIRST usable interval is taken whole. Smoothing towards it from
      // an assumed 120 would spend a second climbing to a tempo that was
      // already known. If it was itself a glitch, the readings after it are
      // all "outliers" that agree with each other, and replace it.
      bpm_ = measured;
      haveTempo_ = true;
      outliers_ = 0;
      return;
    }
    const double ratio = measured / bpm_;
    if (ratio > 0.8 && ratio < 1.25) {
      bpm_ += kSmoothing * (measured - bpm_);
      outliers_ = 0;
      return;
    }
    // Far from the tempo: a dropped or doubled byte, or a real jump. Count
    // it towards a new tempo only while the far readings agree -- within
    // 15%, room for jitter at a fast tempo -- and believe them at four.
    const double mean = outliers_ > 0 ? outlierSum_ / outliers_ : 0.0;
    if (outliers_ > 0 && std::fabs(measured / mean - 1.0) < 0.15) {
      outlierSum_ += measured;
      ++outliers_;
    } else {
      outlierSum_ = measured;
      outliers_ = 1;
    }
    if (outliers_ >= kConfirm) {
      bpm_ = outlierSum_ / outliers_;
      outliers_ = 0;
    }
  }

  void advance(double frames) {
    if (frames <= 0.0) return;
    framesSinceClock_ += frames;
    // The position only moves while the transport is running, and only once a
    // tempo is known: advancing at an assumed 120 would put a stopped
    // sequencer's plugin somewhere it never was. Nor past the clock that has
    // not come yet: a slower tempo shows as a late clock, and the position
    // waits for it rather than running ahead on the old estimate.
    if (running_ && haveTempo_) {
      const double next = (double) songClocks_ / kClocksPerQuarter;
      ppq_ += frames * bpm_ / (60.0 * sampleRate_);
      if (ppq_ > next) ppq_ = next;
    }
  }

  // One pole per clock byte. At 24 clocks to the quarter this averages over
  // roughly the last beat -- see the note at the top about why neither
  // extreme works.
  static constexpr double kSmoothing = 0.08;
  // Consecutive, mutually consistent readings far from the tempo that make
  // a new one. A sixth of a beat.
  static constexpr int kConfirm = 4;

  double sampleRate_ = 48000.0;
  double bpm_ = 120.0;
  double framesSinceClock_ = 0.0;
  double ppq_ = 0.0;
  uint64_t clocks_ = 0;
  // The song position, in clocks, that the NEXT clock plays: Start makes it
  // 0, each clock while running moves it on.
  uint64_t songClocks_ = 0;
  double outlierSum_ = 0.0;
  int outliers_ = 0;
  bool running_ = false, haveTempo_ = false, sawStart_ = false, haveLastClock_ = false;
};

} // namespace sonore
