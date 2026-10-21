// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the Fuzz Face, as its circuit.
//
// The first pedal simple enough to model whole and famous enough to be worth
// it: two germanium PNP transistors, four capacitors, a feedback resistor,
// and every part value printed in every analysis of it since 1966 (the
// netlist below is the Dallas Arbiter circuit as documented in the public
// ElectroSmash analysis: 2.2u in, 33k and 8.2k collector loads, 100k
// feedback from Q2's emitter to Q1's base, 1k fuzz pot with a 20u bypass,
// 470R + 10n into a 500k volume pot, 9 V supply -- negative ground, because
// the transistors are PNP).
//
// It runs on circuit.h's nodal DK engine rather than a hand-derived ODE,
// because the fuzz's character IS the circuit's interactions: the bias
// point Q2's emitter feeds back to Q1's base, the input impedance loading
// the source (which is why a guitar's volume knob cleans it up -- model
// that with setSourceImpedance), and the asymmetric clipping that falls out
// of the transistors' operating point instead of being painted on.
//
// The transistors are Ebers-Moll with germanium-CLASS parameters (Is 1e-7,
// so the junctions sit near 0.2 V; betas 70 and 120, the mismatched pair
// the classic units selected for). They are typical values for an AC128,
// not a specific unit's datasheet -- germanium spread is the reason two
// vintage Fuzz Faces never sounded alike.
//
// The circuit runs at 4x: BJT clipping is tanh-shaped, and measured, the
// folded products of a hot bright tone stay under the harness's gate there.
//
// Included by dsp.h.
#pragma once
#ifndef SONORE_DSP_H_INCLUDED
#error "fuzz.h is part of dsp.h and needs what it includes first: include <sonore/dsp.h> instead."
#endif
#include <cmath>
#include "audio.h"

namespace sonore {

/**
 * The Fuzz Face pedal as its circuit: two germanium PNP transistors with the
 * Dallas Arbiter part values, solved whole by circuit.h's nodal engine at 4x
 * (see the file comment for the netlist and the transistor parameters).
 *
 * One channel, per sample, in volts: a full-scale sample swings the input
 * +/-1 V, and the output is the volume pot's wiper. Call setSampleRate() in
 * prepare() and report latencySamples() to the host. Fuzz and volume are
 * 0..1 of the pots' travel, the source impedance is in ohms. The setters and
 * process() run on the audio thread. The same value again costs nothing; a
 * new one moves a resistor, which re-factorises the circuit's matrix on the
 * next sample, so set the pots once a block rather than per sample. Before
 * audio runs a knob re-solves the operating point, so a pedal loaded with
 * its settings starts quiet; once it runs the circuit moves with the knob.
 */
class FuzzFace {
public:
  /** The circuit with both pots full up and a 1 kohm source. Call
   *  setSampleRate() before process(). */
  FuzzFace() { build(); }

  /** The host's rate in Hz -- the circuit runs at factor() times it; 1 or
   *  below is taken as 48 kHz. Resets the state (see reset()). From
   *  prepare(), not per block. */
  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    os_.shaper().ckt.setSampleRate((double) sr_ * (double) factor());
    reset();
  }
  // The knobs. Turned before audio runs -- the plugin flow: prepare, then
  // the parameters -- the operating point is solved again for them. It was
  // solved in setSampleRate() for whatever the pots held then, so every
  // freshly loaded pedal with the fuzz below full thumped at +7 dBFS when
  // playback started. The same value again is no change at all.

  /** The fuzz pot, 0..1: how much of Q2's emitter resistance is bypassed. */
  void setFuzz(float amount) {
    const float f = clampf(amount, 0.0f, 1.0f);
    if (f == fuzz_) return;
    fuzz_ = f;
    applyFuzz();
    settle();
  }
  /** The volume pot, 0..1. */
  void setVolume(float amount) {
    const float v = clampf(amount, 0.0f, 1.0f);
    if (v == vol_) return;
    vol_ = v;
    applyVolume();
    settle();
  }
  /** What is driving it: a guitar pickup is 5k .. 40k, a line out is tens
   *  of ohms. The fuzz cleans up into a high impedance -- that is the
   *  volume-knob trick, and it is real physics here. */
  void setSourceImpedance(float ohms) {
    const float r = clampf(ohms, 1.0f, 1e6f);
    if (r == source_) return;
    source_ = r;
    os_.shaper().ckt.setResistor(rs_, (double) source_);
    settle();
  }
  /** Clears the oversampler and solves the circuit's operating point for a
   *  silent input, so the next sample starts from rest. Until process()
   *  runs again, a knob change re-solves it too. */
  void reset() {
    os_.reset();
    running_ = false;
  }
  /** The oversampling factor the circuit runs at: 4. */
  static constexpr int factor() { return 4; }
  /** The oversampler's delay in samples at the host rate: what to report
   *  to the host. */
  static constexpr int latencySamples() { return Oversampled<Core, 2>::latencySamples(); }

  /** One volt per unit: a full-scale sample swings the input +/-1 V. */
  inline float process(float x) {
    running_ = true;
    return os_.process(x);
  }

  /** Q2's collector, the classic bias check (folklore says -4.5 V). */
  float q2CollectorVolts() { return (float) os_.shaper().ckt.voltage(os_.shaper().nC2); }
  /** Whether the circuit's solver converged on the last oversampled step;
   *  false when it ran out of iterations or its answer was not finite. */
  bool converged() const { return os_.shaper().ckt.converged(); }

private:
  /** The circuit as the oversampler runs it: the netlist, the source the
   *  input drives, Q2's collector for the bias check, and the output node. */
  struct Core {
    NodalCircuit<12, 2, 4, 4> ckt; ///< the netlist, built by build()
    ///@{
    /** The input source's id, and the node ids of Q2's collector and of
     *  the output (the volume wiper). */
    int src = -1, nC2 = 0, nOut = 0;
    ///@}
    /** One oversampled sample: the input in volts, the output in volts. */
    inline float process(float x) {
      ckt.setSource(src, (double) x);
      ckt.step();
      return (float) ckt.voltage(nOut);
    }
    // Against a silent input. The source still held the last oversampled
    // sample that went through, and the operating point was solved against
    // it: silence after a reset() came out of the fuzz at +17 dBFS.
    /** Zeroes the input and solves the operating point for it. */
    void reset() {
      ckt.setSource(src, 0.0);
      ckt.reset();
    }
  };

  void build() {
    auto& c = os_.shaper().ckt;
    c.clear();
    const int nIn = c.addNode();      // the source's live end
    const int nRs = c.addNode();      // after the source impedance
    const int nB1 = c.addNode();      // Q1 base
    const int nC1 = c.addNode();      // Q1 collector = Q2 base
    const int nC2 = c.addNode();      // Q2 collector
    const int nE2 = c.addNode();      // Q2 emitter
    const int nW = c.addNode();       // fuzz pot wiper
    const int nR5 = c.addNode();      // after the 470R
    const int nVt = c.addNode();      // top of the volume pot
    const int nVw = c.addNode();      // volume wiper: the output
    const int nV = c.addNode();       // the -9 V rail
    os_.shaper().src = c.addVoltageSource(nIn, 0, 0.0);
    c.addVoltageSource(nV, 0, -9.0);
    rs_ = c.addResistor(nIn, nRs, 1000.0);
    c.addCapacitor(nRs, nB1, 2.2e-6);
    c.addBjt(nC1, nB1, 0, 1e-7, 70.0, 4.0, true);   // Q1, germanium PNP
    c.addResistor(nC1, nV, 33e3);
    c.addBjt(nC2, nC1, nE2, 1e-7, 120.0, 4.0, true); // Q2
    c.addResistor(nC2, nV, 8.2e3);
    ra_ = c.addResistor(nE2, nW, 1.0);       // fuzz pot, set by setFuzz
    rb_ = c.addResistor(nW, 0, 1001.0);
    c.addCapacitor(nW, 0, 20e-6);
    c.addResistor(nE2, nB1, 100e3);          // the feedback that biases Q1
    c.addResistor(nC2, nR5, 470.0);
    c.addCapacitor(nR5, nVt, 10e-9);
    rt_ = c.addResistor(nVt, nVw, 1.0);      // volume pot, set by setVolume
    rv_ = c.addResistor(nVw, 0, 500e3);
    os_.shaper().nC2 = nC2;
    os_.shaper().nOut = nVw;
    applyFuzz();
    applyVolume();
  }

  void applyFuzz() {
    auto& c = os_.shaper().ckt;
    c.setResistor(ra_, (1.0 - (double) fuzz_) * 1000.0 + 1.0);
    c.setResistor(rb_, (double) fuzz_ * 1000.0 + 1.0);
  }
  void applyVolume() {
    auto& c = os_.shaper().ckt;
    c.setResistor(rt_, (1.0 - (double) vol_) * 500e3 + 1.0);
    c.setResistor(rv_, (double) vol_ * 500e3 + 1.0);
  }
  /** Before audio runs, a knob is a setting: solve the operating point for
   *  it. Once running it is a movement, and the circuit moves with it. */
  void settle() {
    if (!running_) reset();
  }

  Oversampled<Core, 2> os_;
  int rs_ = -1, ra_ = -1, rb_ = -1, rt_ = -1, rv_ = -1;
  float sr_ = 48000.0f, fuzz_ = 1.0f, vol_ = 1.0f, source_ = 1000.0f;
  bool running_ = false; // process() has run since the last reset()
};

} // namespace sonore
