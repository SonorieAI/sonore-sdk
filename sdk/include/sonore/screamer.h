// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the Tube Screamer's clipping stage, as its circuit.
//
// The op-amp device earns its keep here: the TS-808's drive stage is a
// non-inverting op-amp whose GROUND LEG is 4.7 kOhm + 47 nF (so the gain
// falls to unity below ~720 Hz: the famous "bass stays clean" voicing:
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
// active network is not modelled component-for-component here: building
// it from the schematic on circuit.h is a follow-up, not an assumption.
//
// A full-scale sample is one volt at the (buffered) input, as fuzz.h.
//
// Included by dsp.h.
#pragma once
#include <cmath>
#include "audio.h"

namespace sonore {

class TubeScreamer {
public:
  TubeScreamer() { build(); }

  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    os_.shaper().ckt.setSampleRate((double) sr_ * (double) factor());
    tone_.setSampleRate(sr_);
    tone_.setCutoff(723.0f);
    reset();
  }
  /** The drive pot, 0..1: 51k .. 551k of feedback resistance. */
  void setDrive(float amount) {
    drive_ = clampf(amount, 0.0f, 1.0f);
    os_.shaper().ckt.setResistor(rDrive_, 1.0 + (double) drive_ * 500e3);
  }
  /** One knob, dark to bright, hinged at the stage's own 723 Hz. */
  void setTone(float t) { toneAmt_ = clampf(t, 0.0f, 1.0f); }
  /** The volume pot, 0..1. */
  void setLevel(float level) { level_ = clampf(level, 0.0f, 1.0f); }
  void reset() {
    os_.reset();
    tone_.reset();
  }
  static constexpr int factor() { return 4; }
  static constexpr int latencySamples() { return Oversampled<Core, 2>::latencySamples(); }

  inline float process(float x) {
    const float y = os_.process(x);
    const float lp = tone_.lp(y);
    return level_ * (lp + toneAmt_ * (y - lp));
  }

private:
  struct Core {
    NodalCircuit<6, 1, 2, 2, 1> ckt;
    int src = -1, nOut = 0;
    inline float process(float x) {
      ckt.setSource(src, (double) x);
      ckt.step();
      return (float) ckt.voltage(nOut);
    }
    void reset() { ckt.reset(); }
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
    setDrive(drive_);
  }

  Oversampled<Core, 2> os_;
  OnePole tone_;
  int rDrive_ = -1;
  float sr_ = 48000.0f, drive_ = 0.5f, toneAmt_ = 0.5f, level_ = 0.5f;
};

} // namespace sonore
