// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the hardware compressors: opto, FET, VCA.
//
// dsp.h's Compressor is the textbook feed-forward design: a detector, a gain
// computer with a soft knee, a multiplier. Every classic compressor is that
// diagram with one box replaced by a piece of hardware whose physics is the
// sound, and "make it sound like an LA-2A" was being answered by turning the
// textbook's release knob. These are the three gain elements that define the
// three families, each modelled from what the element does:
//
//   OptoCompressor   the LA-2A's T4 cell: an electroluminescent panel driven
//                    by the sidechain, lighting a cadmium-sulphide photocell
//                    whose resistance sets the gain. The panel is fast; the
//                    photocell is not, and its recovery depends on how long
//                    and how brightly it was lit. The manufacturer's own
//                    figures, which the model reproduces: 10 ms attack, 50%
//                    of the release in about 60 ms and the rest in 0.5 to
//                    5 seconds, longer after longer compression. Two states
//                    -- a fast one and a slow "memory" that fills while the
//                    cell is lit -- and the release follows whichever is
//                    higher, which is exactly that shape. No ratio control:
//                    the T4's own curve is a soft 3:1 in Compress and steeper
//                    in Limit, and Peak Reduction sets how hard it is lit.
//
//   FetCompressor    the 1176: a FET as the shunt leg of an attenuator, its
//                    channel resistance set by the sidechain. Two things the
//                    circuit does that a gain computer does not: it is a
//                    FEEDBACK compressor (the sidechain reads the OUTPUT), so
//                    the ratio buttons set a loop gain and the effective
//                    knee and timing are program-dependent; and a FET in its
//                    triode region has a channel conductance that depends on
//                    the drain voltage -- the signal -- which is where the
//                    1176's even-harmonic grain under gain reduction comes
//                    from. Attack 20 to 800 microseconds, release 50 to
//                    1100 ms, the four ratios and "all buttons in".
//                    The loop is SOLVED each sample, not delayed. A digital
//                    feedback compressor that feeds back last sample's output
//                    has a unit delay in a loop whose gain is 3 to 19, and at
//                    a 20 microsecond attack -- one sample -- it limit-cycles;
//                    that is why software 1176s quietly lengthen the attack.
//                    The output level is the fixed point of "what the
//                    detector sees is what the attenuator made", a monotone
//                    equation in |y| that bisection finds exactly, the same
//                    move zero-delay feedback makes for a filter. Then the
//                    FET's signal-dependent conductance makes the sample the
//                    root of a quadratic, which has a closed form.
//
//   VcaCompressor    the dbx 160 and the SSL bus compressor: a VCA whose gain
//                    is exactly exponential in its control voltage, so the
//                    ratio is exactly the ratio and the element itself adds
//                    nothing -- which is the point of a VCA and the reason
//                    this one measures under -60 dB of distortion at 10 dB
//                    of gain reduction where the FET measures -40. The dbx
//                    detects true RMS through a single time constant (its
//                    "program-dependent" feel is the RMS integrator); the
//                    SSL detects peaks and offers the two-capacitor auto
//                    release, fast then slow. OverEasy is a wide soft knee.
//
// The detector and knee mathematics are Giannoulis, Massberg & Reiss,
// "Digital Dynamic Range Compressor Design -- A Tutorial and Analysis" (JAES
// 2012); the time constants and curves are the manufacturers' published
// specifications; the gain elements are the physics stated above. Every
// figure quoted is measured in the unit test.
//
// Included by dsp.h.
#pragma once
#include <cmath>
#include "audio.h"

namespace sonore {

namespace compdetail {
/** One-pole time constant, seconds to 63%. */
inline float tau(float seconds, float sr) {
  const float s = seconds > 1e-6f ? seconds : 1e-6f;
  return std::exp(-1.0f / (s * sr));
}
/** The quadratic soft knee of Giannoulis et al.: gain reduction in dB for a
 *  level `over` dB above threshold, `slope` = 1/R - 1 (negative), knee width
 *  `knee`. */
inline float kneeGainDb(float over, float slope, float knee) {
  const float half = 0.5f * knee;
  if (knee > 0.0f && over > -half && over < half) {
    const float t = over + half;
    return slope * t * t / (2.0f * knee);
  }
  return over > 0.0f ? slope * over : 0.0f;
}
} // namespace compdetail

// ── Opto ─────────────────────────────────────────────────────────────────────

class OptoCompressor {
public:
  enum class Mode { Compress, Limit };

  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    // The electroluminescent panel: the light follows the sidechain in ~10 ms.
    panel_ = compdetail::tau(0.010f, sr_);
    // The photocell: fast recovery to half in 60 ms (tau 87 ms), a slow
    // memory that fills over half a second and empties over three.
    fastRelease_ = compdetail::tau(0.0866f, sr_);
    slowAttack_ = compdetail::tau(0.5f, sr_);
    slowRelease_ = compdetail::tau(3.0f, sr_);
    emphasis_.setSampleRate(sr_);
    setEmphasis(emphasisAmount_);
  }
  /** 0..1: how hard the panel is driven. The LA-2A's one big knob. */
  void setPeakReduction(float amount) { peakReduction_ = clampf(amount, 0.0f, 1.0f); }
  void setMode(Mode m) { mode_ = m; }
  void setMakeup(float db) { makeup_ = dbToGain(db); }
  /** The sidechain's high-frequency emphasis (the R37 trim): 0 = flat,
   *  1 = +10 dB above 3 kHz, which makes it listen to sibilance. */
  void setEmphasis(float amount) {
    emphasisAmount_ = clampf(amount, 0.0f, 1.0f);
    emphasis_.highShelf(3000.0f, 0.7071f, 10.0f * emphasisAmount_);
  }
  void reset() {
    light_ = 0.0f;
    fast_ = slow_ = 0.0f;
    emphasis_.reset();
  }

  /** The gain for this sample from a detector input (the signal, or a key). */
  inline float computeGain(float detector) {
    // The panel's light follows the rectified, emphasised sidechain.
    const float sc = std::fabs(emphasis_.process(detector));
    light_ = flushDenormal(sc + panel_ * (light_ - sc));
    // What the T4's curve asks for at this brightness: a soft knee, 3:1 in
    // Compress and steep in Limit. Peak Reduction is the sidechain gain: at
    // full it puts a -6 dBFS tone 20 dB into the curve, at half it just
    // reaches it, at zero the panel never lights.
    const float levelDb = gainToDbFloor(light_) + peakReduction_ * 40.0f - 10.0f;
    const float slope = mode_ == Mode::Compress ? (1.0f / 3.0f - 1.0f) : (1.0f / 15.0f - 1.0f);
    const float knee = mode_ == Mode::Compress ? 10.0f : 4.0f;
    const float want = -compdetail::kneeGainDb(levelDb, slope, knee); // dB of reduction, >= 0
    // The photocell: the fast state follows the light up at once and down
    // over 60-ish ms; the slow state holds HALF of the reduction, charging
    // while the cell is lit and draining over seconds. The cell's conductance
    // is whichever is higher -- so the release falls fast to the level the
    // memory holds, then crawls, and how much it holds is how long it was lit.
    fast_ = want > fast_ ? want : flushDenormal(want + fastRelease_ * (fast_ - want));
    const float memory = 0.5f * want;
    const float slowCoef = memory > slow_ ? slowAttack_ : slowRelease_;
    slow_ = flushDenormal(memory + slowCoef * (slow_ - memory));
    grDb_ = fast_ > slow_ ? fast_ : slow_;
    return dbToGain(-grDb_) * makeup_;
  }
  inline float process(float x) { return x * computeGain(x); }
  /** Gain reduction in dB, positive. */
  float gainReduction() const { return grDb_; }

private:
  Mode mode_ = Mode::Compress;
  Biquad emphasis_;
  float sr_ = 48000.0f, peakReduction_ = 0.5f, makeup_ = 1.0f, emphasisAmount_ = 0.0f;
  float panel_ = 0.998f, fastRelease_ = 0.9998f, slowAttack_ = 0.99996f, slowRelease_ = 0.999993f;
  float light_ = 0.0f, fast_ = 0.0f, slow_ = 0.0f, grDb_ = 0.0f;
};

// ── FET ──────────────────────────────────────────────────────────────────────

class FetCompressor {
public:
  enum class Ratio { Four, Eight, Twelve, Twenty, AllButtons };

  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    setAttack(attackUs_);
    setRelease(releaseMs_);
  }
  /** The input knob: drive into the gain cell, dB. The 1176 has no
   *  threshold; this is how hard it is hit. */
  void setInput(float db) { input_ = dbToGain(db); }
  void setOutput(float db) { output_ = dbToGain(db); }
  /** 20 .. 800 microseconds. */
  void setAttack(float microseconds) {
    attackUs_ = clampf(microseconds, 20.0f, 800.0f);
    attack_ = compdetail::tau(attackUs_ * 1e-6f, sr_);
  }
  /** 50 .. 1100 milliseconds. */
  void setRelease(float ms) {
    releaseMs_ = clampf(ms, 50.0f, 1100.0f);
    release_ = compdetail::tau(releaseMs_ * 1e-3f, sr_);
  }
  void setRatio(Ratio r) { ratio_ = r; }
  /** The FET's drain-voltage dependence: 0 is an ideal attenuator, 0.2 is a
   *  measured 1176's grain (-40 dB of second harmonic at 10 dB of GR). */
  void setGrain(float kappa) { kappa_ = clampf(kappa, 0.0f, 0.5f); }
  void reset() {
    env_ = 0.0f;
    grDb_ = 0.0f;
  }

  inline float process(float x) {
    // The loop gain the ratio buttons set. In a feedback compressor the
    // closed-loop ratio R comes from a computer slope of 1/Rc - 1 with
    // 1/Rc = 2 - R, so the buttons are loop gains of -3, -7, -11 and -19.
    float loopRatio, threshold = kThresholdDb, kappa = kappa_;
    switch (ratio_) {
      case Ratio::Four: loopRatio = 4.0f; break;
      case Ratio::Eight: loopRatio = 8.0f; break;
      case Ratio::Twelve: loopRatio = 12.0f; break;
      case Ratio::Twenty: loopRatio = 20.0f; break;
      default: // all buttons in: the loop over-driven, a lower threshold, more grain
        loopRatio = 20.0f;
        threshold -= 8.0f;
        kappa = clampf(kappa_ * 2.0f, 0.0f, 0.5f);
        break;
    }
    const float slope = (2.0f - loopRatio) - 1.0f; // 1/Rc - 1
    const float in = x * input_;
    const float ax = std::fabs(in);

    // FEEDBACK, solved: the detector sees THIS sample's output. For a trial
    // output magnitude u the rectifier and the gate's RC give an envelope,
    // the envelope a gain reduction, the reduction an output |x|/(1 + G):
    // u is the fixed point of that, and the mismatch is monotone in u, so
    // bisection between 0 and |x| lands on it.
    auto conductanceFor = [&](float u, float& gr) {
      const float coef = u > env_ ? attack_ : release_;
      const float env = u + coef * (env_ - u);
      gr = -compdetail::kneeGainDb(gainToDbFloor(env) - threshold, slope, 2.0f);
      if (gr > kMaxReductionDb) gr = kMaxReductionDb;
      return 1.0f / dbToGain(-gr) - 1.0f;
    };
    float lo = 0.0f, hi = ax, gr = 0.0f;
    for (int i = 0; i < kSolveIterations; ++i) {
      const float mid = 0.5f * (lo + hi);
      const float g = conductanceFor(mid, gr);
      if (mid - ax / (1.0f + g) > 0.0f) hi = mid; else lo = mid;
    }
    const float u = 0.5f * (lo + hi);
    const float g0 = conductanceFor(u, gr);
    grDb_ = gr;
    env_ = flushDenormal(u + (u > env_ ? attack_ : release_) * (env_ - u));

    // The attenuator: series R (normalised to 1) into the FET's channel
    // conductance, G0 (1 + kappa y) because the channel sees the drain
    // signal. y = x / (1 + G0 (1 + kappa y)) is a quadratic in y.
    float y;
    const float a = g0 * kappa;
    if (a < 1e-9f) {
      y = in / (1.0f + g0);
    } else {
      const float b = 1.0f + g0;
      const float disc = b * b + 4.0f * a * in;
      y = disc > 0.0f ? (-b + std::sqrt(disc)) / (2.0f * a) : in / b;
    }
    return y * output_;
  }
  /** Gain reduction in dB, positive. */
  float gainReduction() const { return grDb_; }

private:
  static constexpr float kThresholdDb = -18.0f;    // where the FET starts to open, after the input knob
  static constexpr float kMaxReductionDb = 40.0f;  // the FET fully open
  static constexpr int kSolveIterations = 16;      // |x| / 65536: the loop closed to -96 dB

  Ratio ratio_ = Ratio::Four;
  float sr_ = 48000.0f, input_ = 1.0f, output_ = 1.0f, attackUs_ = 200.0f, releaseMs_ = 300.0f;
  float attack_ = 0.9f, release_ = 0.9999f, kappa_ = 0.2f;
  float env_ = 0.0f, grDb_ = 0.0f;
};

// ── VCA ──────────────────────────────────────────────────────────────────────

class VcaCompressor {
public:
  enum class Detector { Rms, Peak };

  void setSampleRate(float sr) {
    sr_ = sr > 1.0f ? sr : 48000.0f;
    setAttack(attackMs_);
    setRelease(releaseMs_);
    rms_ = compdetail::tau(rmsMs_ * 1e-3f, sr_);
    autoFast_ = compdetail::tau(0.15f, sr_);
    autoSlow_ = compdetail::tau(1.5f, sr_);
    autoCharge_ = compdetail::tau(0.5f, sr_);
  }
  void setThreshold(float db) { thresholdDb_ = db; }
  void setRatio(float ratio) { ratio_ = ratio < 1.0f ? 1.0f : ratio; }
  /** Knee width in dB. dbx's OverEasy is about 10; the SSL is close to hard. */
  void setKnee(float db) { kneeDb_ = db < 0.0f ? 0.0f : db; }
  void setAttack(float ms) {
    attackMs_ = ms;
    attack_ = compdetail::tau(ms * 1e-3f, sr_);
  }
  void setRelease(float ms) {
    releaseMs_ = ms;
    release_ = compdetail::tau(ms * 1e-3f, sr_);
  }
  void setMakeup(float db) { makeup_ = dbToGain(db); }
  void setDetector(Detector d) { detector_ = d; }
  /** The RMS integrator's time constant (dbx: ~15 ms). */
  void setRmsTime(float ms) {
    rmsMs_ = ms > 0.1f ? ms : 0.1f;
    rms_ = compdetail::tau(rmsMs_ * 1e-3f, sr_);
  }
  /** SSL's auto release: a fast capacitor and a slow one, the gain
   *  recovering along whichever holds more. */
  void setAutoRelease(bool on) { autoRelease_ = on; }
  void reset() {
    env_ = square_ = 0.0f;
    fast_ = slow_ = 0.0f;
    grDb_ = 0.0f;
  }

  inline float computeGain(float detector) {
    float level;
    if (detector_ == Detector::Rms) {
      // True RMS through one time constant: mean square, then the root.
      square_ = flushDenormal(detector * detector + rms_ * (square_ - detector * detector));
      level = std::sqrt(square_);
    } else {
      const float r = std::fabs(detector);
      // In auto the detector lets go at the fast rate; the shaping below
      // supplies the tail.
      env_ = flushDenormal(r + (r > env_ ? attack_ : (autoRelease_ ? autoFast_ : release_)) * (env_ - r));
      level = env_;
    }
    const float want = -compdetail::kneeGainDb(gainToDbFloor(level) - thresholdDb_, 1.0f / ratio_ - 1.0f, kneeDb_);
    if (autoRelease_) {
      // The SSL's two-capacitor release: the control voltage is the SUM of a
      // part that lets go in 150 ms and a part that lets go in 1.5 s, seven
      // tenths and three tenths of it. The slow capacitor CHARGES slowly too
      // (half a second), which is the program dependence: a short peak never
      // fills it and releases fast; a sustained passage does and drags a
      // long tail behind it.
      fast_ = want > fast_ ? want : flushDenormal(want + autoFast_ * (fast_ - want));
      slow_ = flushDenormal(want + (want > slow_ ? autoCharge_ : autoSlow_) * (slow_ - want));
      grDb_ = 0.7f * fast_ + 0.3f * slow_;
    } else if (detector_ == Detector::Rms) {
      // The RMS detector already set the timing; the attack/release smooth
      // the control voltage on top of it, in dB, the way the sidechain does.
      grDb_ = flushDenormal(want + (want > grDb_ ? attack_ : release_) * (grDb_ - want));
    } else {
      grDb_ = want;
    }
    // The VCA: exactly exponential in its control voltage.
    return dbToGain(-grDb_) * makeup_;
  }
  inline float process(float x) { return x * computeGain(x); }
  /** Gain reduction in dB, positive. */
  float gainReduction() const { return grDb_; }

private:

  Detector detector_ = Detector::Rms;
  float sr_ = 48000.0f, thresholdDb_ = -20.0f, ratio_ = 4.0f, kneeDb_ = 10.0f;
  float attackMs_ = 5.0f, releaseMs_ = 150.0f, rmsMs_ = 15.0f, makeup_ = 1.0f;
  float attack_ = 0.99f, release_ = 0.9999f, rms_ = 0.9986f, autoFast_ = 0.9999f, autoSlow_ = 0.99999f, autoCharge_ = 0.99996f;
  float env_ = 0.0f, square_ = 0.0f, fast_ = 0.0f, slow_ = 0.0f, grDb_ = 0.0f;
  bool autoRelease_ = false;
};

} // namespace sonore
