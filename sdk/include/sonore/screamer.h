// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the Tube Screamer's clipping stage, as its circuit.
//
// The op-amp device earns its keep here: the TS-808's drive stage is a
// non-inverting op-amp whose GROUND LEG is 4.7 kOhm + 47 nF (so the gain
// falls to unity below ~720 Hz: the famous "bass stays clean" voicing,
// the same 723 Hz figure targets.ts grades against) and whose FEEDBACK arm
// is 51 kOhm + a 500 kOhm drive pot with 51 pF and the 1N914 pair across
// it. Because the diodes clip the FEEDBACK, the output is structurally
// input + clipped(gain excess): the "clean signal rides on top" behaviour
// every Tube Screamer analysis names (Yeh's thesis treats the op-amp as
// ideal, which is what circuit.h's nullor is; the chip never rails in this
// stage). The netlist below is that public schematic, solved whole at 4x.
//
// The TONE stage is deliberately STRUCTURAL, and says so: a single-knob
// crossfade hinged at the same 723 Hz voicing. The tone control's real
// active network is not modelled component-for-component here. Building
// it from the schematic on circuit.h is a follow-up, not an assumption.
//
// A full-scale sample is one volt at the (buffered) input, as fuzz.h.
//
// Included by dsp.h.
#pragma once
#ifndef SONORE_DSP_H_INCLUDED
#error "screamer.h is part of dsp.h and needs what it includes first: include <sonore/dsp.h> instead."
#endif
#include <cmath>
#include "audio.h"

namespace sonore {

/**
 * The TS-808's clipping stage as its circuit -- the non-inverting op-amp
 * with the 1N914 pair in its feedback, solved at 4x on circuit.h's engine --
 * followed by a structural one-knob tone control (see the file comment for
 * what is modelled and what is not).
 *
 * One channel, per sample, a full-scale sample being one volt at the input.
 * Call setSampleRate() in prepare() and report latencySamples() to the
 * host. Drive, tone and level are 0..1. The setters and process() run on the
 * audio thread. The level glides; the drive moves a resistor. The same drive
 * again costs nothing; a new one re-factorises the circuit's matrix on the
 * next sample, so set it once a block, not per sample.
 */
class TubeScreamer {
public:
  /** Builds the netlist; drive, tone and level start at 0.5. Call
   *  setSampleRate() before audio. */
  TubeScreamer() { build(); }

  /** The host rate in Hz (1 or less is taken as 48000); the circuit runs at
   *  factor() times it. Ends with reset(), so the circuit starts settled.
   *  Nothing is allocated. */
  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    os_.shaper().ckt.setSampleRate((double) sr_ * (double) factor());
    tone_.setSampleRate(sr_);
    tone_.setCutoff(723.0f);
    levelGlide_.setSampleRate(sr_);
    reset();
  }
  /** The drive pot, 0..1: 51k .. 551k of feedback resistance. */
  void setDrive(float amount) {
    // The same value is no change, as FuzzFace's knobs are. Every call moved
    // the resistor, so a drive set every sample to the value it already had
    // -- a smoothed parameter that has arrived -- re-factorised the matrix
    // every sample: 1.3-1.5x the time of leaving it alone.
    const float d = clampf(amount, 0.0f, 1.0f);
    if (d == drive_) return;
    drive_ = d;
    applyDrive();
  }
  /** One knob, dark to bright, hinged at the stage's own 723 Hz. */
  void setTone(float t) { toneAmt_ = clampf(t, 0.0f, 1.0f); }
  /** The volume pot, 0..1. Glides once audio runs (see Glide): in one
   *  sample 0.1 -> 1 stepped the output 17 times its largest natural step. */
  void setLevel(float level) {
    level_ = clampf(level, 0.0f, 1.0f);
    levelGlide_.set(level_);
  }
  /** Silence: clears the oversampler and the tone filter, solves the
   *  circuit's resting state against a silent input, and lands the level on
   *  its setting. */
  void reset() {
    os_.reset();
    tone_.reset();
    levelGlide_.snap();
  }
  /** The oversampling the circuit runs at: 4x. */
  static constexpr int factor() { return 4; }
  /** The oversampler's delay at the host rate, in whole samples: the latency
   *  to report. */
  static constexpr int latencySamples() { return Oversampled<Core, 2>::latencySamples(); }

  /** One sample (1.0 is one volt at the input) through the clipping stage,
   *  the tone control and the level. No allocation; the audio thread. */
  inline float process(float x) {
    const float y = os_.process(x);
    const float lp = tone_.lp(y);
    return levelGlide_.next() * (lp + toneAmt_ * (y - lp));
  }

private:
  /** The circuit as the oversampler runs it: the netlist, the source the
   *  input drives, and the op-amp's output node. */
  struct Core {
    NodalCircuit<6, 1, 2, 2, 1> ckt; ///< the netlist, solved at the oversampled rate
    /** The input voltage source's id in `ckt`. */
    int src = -1, nOut = 0; ///< the op-amp's output node, read as the stage's output
    /** One oversampled sample: the input drives the source, the circuit takes
     *  a step, the output node is the result. */
    inline float process(float x) {
      ckt.setSource(src, (double) x);
      ckt.step();
      return (float) ckt.voltage(nOut);
    }
    /** The operating point solved against a silent input. The source still
     *  held the last oversampled sample that went through, and the operating
     *  point was solved against it: silence after a reset() came out of the
     *  fuzz at +17 dBFS. */
    void reset() {
      ckt.setSource(src, 0.0);
      ckt.reset();
    }
  };

  void build() {
    auto& c = os_.shaper().ckt;
    c.clear();
    const int nIn = c.addNode();
    const int nP = c.addNode();  // op-amp +
    const int nM = c.addNode();  // op-amp -
    const int nX = c.addNode();  // between the 4.7k and the 47n
    const int nO = c.addNode();  // op-amp out
    const int nF = c.addNode();  // between the 51k and the drive pot
    os_.shaper().src = c.addVoltageSource(nIn, 0, 0.0);
    c.addResistor(nIn, nP, 10e3);      // into the + input (the buffered source)
    c.addOpamp(nP, nM, nO);
    c.addResistor(nM, nX, 4.7e3);      // the ground leg: 4.7k + 47n = the 720 Hz corner
    c.addCapacitor(nX, 0, 47e-9);
    c.addResistor(nO, nF, 51e3);       // feedback: 51k + drive pot,
    rDrive_ = c.addResistor(nF, nM, 1.0);
    c.addCapacitor(nO, nM, 51e-12);    // ...51 pF across it,
    c.addDiode(nO, nM);                // ...and the 1N914 pair.
    c.addDiode(nM, nO);
    os_.shaper().nOut = nO;
    applyDrive(); // not setDrive(drive_): the same value, so it would skip
  }

  /** The drive pot's resistance into the netlist. */
  void applyDrive() { os_.shaper().ckt.setResistor(rDrive_, 1.0 + (double) drive_ * 500e3); }

  Oversampled<Core, 2> os_;
  OnePole tone_;
  int rDrive_ = -1;
  float sr_ = 48000.0f, drive_ = 0.5f, toneAmt_ = 0.5f, level_ = 0.5f;
  Glide levelGlide_{0.5f, 0.5f};
};

} // namespace sonore
