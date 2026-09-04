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
// The naive reading, one interval, converted directly, is unusable. MIDI
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
 * Tempo and transport, derived from the realtime bytes in a MIDI stream.
 *
 * Call process() once per block with that block's MIDI and its length. Nothing
 * allocates and nothing blocks; this is safe on the audio thread.
 */
class MidiClock {
public:
  /** Twenty-four clocks to the quarter note. Fixed by the MIDI specification,
   *  not a preference. */
  static constexpr int kClocksPerQuarter = 24;

  void reset() {
    running_ = false;
    haveTempo_ = false;
    bpm_ = 120.0;
    framesSinceClock_ = 0.0;
    ppq_ = 0.0;
    clocks_ = 0;
    sawStart_ = false;
  }

  void prepare(double sampleRate) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    reset();
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
      handle(entry.getMessage().getRawStatus());
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

  /** Position in quarter notes since the last Start, advancing between clocks
   *  rather than stepping at each one -- a DSP asking mid-block wants where
   *  the music is now, not where the last clock byte was. */
  double ppqPosition() const { return ppq_; }

  /** How many clock bytes have been seen since the last Start. */
  uint64_t clockCount() const { return clocks_; }

private:
  void handle(int status) {
    switch (status) {
      case 0xF8: { // Clock
        if (framesSinceClock_ > 1.0) {
          const double perQuarter = framesSinceClock_ * kClocksPerQuarter;
          const double measured = 60.0 * sampleRate_ / perQuarter;
          // A sanity window, because one dropped byte doubles the interval and
          // one duplicated byte halves it. Outside 20..400 BPM the reading is
          // far more likely to be a glitch than a tempo anyone has chosen.
          if (measured > 20.0 && measured < 400.0) {
            if (!haveTempo_) {
              // The FIRST usable interval is taken whole. Smoothing towards it
              // from an assumed 120 would spend a second climbing to a tempo
              // that was already known.
              bpm_ = measured;
              haveTempo_ = true;
            } else {
              bpm_ += kSmoothing * (measured - bpm_);
            }
          }
        }
        framesSinceClock_ = 0.0;
        ++clocks_;
        break;
      }
      case 0xFA: // Start: from the top.
        running_ = true;
        sawStart_ = true;
        ppq_ = 0.0;
        clocks_ = 0;
        framesSinceClock_ = 0.0;
        break;
      case 0xFB: // Continue: from where Stop left it.
        running_ = true;
        break;
      case 0xFC: // Stop.
        running_ = false;
        break;
      default:
        break;
    }
  }

  void advance(double frames) {
    if (frames <= 0.0) return;
    framesSinceClock_ += frames;
    // The position only moves while the transport is running, and only once a
    // tempo is known: advancing at an assumed 120 would put a stopped
    // sequencer's plugin somewhere it never was.
    if (running_ && haveTempo_) ppq_ += frames * bpm_ / (60.0 * sampleRate_);
  }

  // One pole per clock byte. At 24 clocks to the quarter this averages over
  // roughly the last beat -- see the note at the top about why neither
  // extreme works.
  static constexpr double kSmoothing = 0.08;

  double sampleRate_ = 48000.0;
  double bpm_ = 120.0;
  double framesSinceClock_ = 0.0;
  double ppq_ = 0.0;
  uint64_t clocks_ = 0;
  bool running_ = false, haveTempo_ = false, sawStart_ = false;
};

} // namespace sonore
