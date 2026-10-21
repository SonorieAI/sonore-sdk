// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: a feedback delay network reverb.
//
// effects.h's Reverb is Schroeder/Moorer: four combs into two allpasses, the
// 1962 topology, and it sounds like it -- a comb bank has a modal density set
// by four delay lengths and the tail carries their signature. Jot's feedback
// delay network (1991) is the modern answer: N delays feeding back through a
// UNITARY mixing matrix, so every line hears every other and the modal density
// grows with N² instead of N. Eight lines of it are the substrate of every
// plate, hall and shimmer written since.
//
// What is decided here, and why:
//
//   The matrix is a Hadamard, applied as a fast Walsh-Hadamard transform:
//   N log N adds, no multiplies, and unitary by construction (scaled by
//   1/sqrt(N)), which is the property that makes the network lossless before
//   the per-line gains are applied -- the decay then comes ONLY from those
//   gains, which is what lets the decay time be set exactly.
//
//   Per-line gain from the delay length and the decay time, Jot's formula:
//   g = 10^(-3 L / (T60 sr)). Every path through the network loses 60 dB in
//   T60 seconds whichever lines it passes through, so the declared decay is
//   the measured one. The unit test checks this against the Schroeder
//   backward integral of the impulse response.
//
//   Delay lengths are distinct PRIMES, picked from a logarithmic spread and
//   rounded to the nearest prime at prepare(): distinct primes are coprime by
//   definition, and coprime lengths are what stop the tail from ringing on
//   the pitch of a common factor. Scaled by the sample rate, so the room
//   keeps its size -- up to MaxDelay: the longest line is at most
//   MaxDelay / sr seconds (42 ms at 192 kHz for the default 8192), and past
//   that the whole spread shrinks together rather than the long lines piling
//   up at the ceiling. A plugin that wants a full-size hall at 192 kHz asks
//   for FdnReverb<8, 32768>.
//
//   Damping is a one-pole lowpass inside each line's loop, so high
//   frequencies decay faster than low ones the way air makes them.
//
//   Modulation moves each line's read point by a fraction of a sample on its
//   own slow LFO. With static lengths a network of this size still has a
//   faint metallic signature at the lengths' beat frequencies; a few samples
//   of drift smears it out. Read with readCubic(), because a delay that
//   sweeps must -- see DelayLine.
//
// Included by dsp.h.
#pragma once
#ifndef SONORE_DSP_H_INCLUDED
#error "fdn.h is part of dsp.h and needs what it includes first: include <sonore/dsp.h> instead."
#endif
#include <cmath>
#include "audio.h"

namespace sonore {

/**
 * SIZE WARNING, the same one the Convolver carries: eight 32768-sample lines
 * plus a pre-delay is ~1.1 MB of state, which CANNOT live on the stack in
 * wasm (64 KB). A real plugin is fine -- the ABI puts SonoreDsp in static
 * storage -- but a local FdnReverb in a test or a helper crashes there.
 *
 * 32768 holds the longest line of the largest room (160 ms) up to 204 kHz.
 * The default was 8192, which held it to 51 kHz: past that every line was
 * shortened by the same factor, which is more echoes a second at the same
 * decay and more passes through the damping. The default network came out
 * 4.7 dB louder in the low mids at 192 kHz and its top decayed 15% sooner,
 * where one with room for its lines sounded as it does at 48. Past 204 kHz
 * the lines shrink again; FdnReverb<8, 8192> is the smaller one, for a
 * caller who never runs above 51 kHz.
 */
template <int Lines = 8, int MaxDelay = 32768>
class FdnReverb {
  static_assert(Lines >= 2 && (Lines & (Lines - 1)) == 0,
                "a Hadamard matrix needs a power-of-two number of lines");
  static_assert(MaxDelay >= 512, "the lines need room for a room");

public:
  /** The network's settings, set together with setParameters(); the same
   *  values again cost nothing, so a plugin can pass them every block. Size
   *  0..1 moves the mean line length from 20 to 100 ms (changed while audio
   *  runs, it fades from the old lengths to the new over 50 ms); decay is
   *  the T60 in seconds, at least 0.05; damping 0..1 closes the loop's
   *  lowpass from 20 kHz down to 1.5 kHz; pre-delay is 0..200 ms, see
   *  maxPreDelayMs(); modulation, width and mix are 0..1. */
  struct Parameters {
    float size = 0.5f;       ///< 0..1, scales the delay lengths (20 ms .. 100 ms mean)
    float decay = 2.0f;      ///< seconds to -60 dB, 0.05 at the least; a change glides
    float damping = 0.3f;    ///< 0..1, high-frequency loss in the loop (its lowpass 20 kHz .. 1.5 kHz)
    float modulation = 0.3f; ///< 0..1, read-point drift, up to 0.083 ms (4 samples at 48 kHz)
    float preDelayMs = 10.0f; ///< before the network, 0..200 ms, held to maxPreDelayMs(); a change jumps
    /** 0..1, how the network's two outputs are blended, gliding when changed:
     *  1 keeps them apart, 0 is mono, 0.5 halfway. */
    float width = 1.0f;
    float mix = 0.3f;        ///< 0..1 dry/wet, equal-power; a change glides
  };

  /** Sets the rate and works the line lengths, loop gains and drift out
   *  again for it, then clears the network as reset() does. Nothing is
   *  allocated: the lines are fixed arrays. The channel count is not read:
   *  the network is always stereo. */
  void prepare(const ProcessSpec& spec) {
    sr_ = (float) spec.sampleRate;
    for (int i = 0; i < Lines; ++i) {
      damp_[i].setSampleRate(sr_);
      gainGlide_[i].setSampleRate(sr_, 20.0f);
      // Distinct rates, all well under 1 Hz, so no two lines drift together.
      lfoInc_[i] = (0.11f + 0.083f * (float) i) / sr_;
      lfoPhase_[i] = (float) i / (float) Lines;
    }
    widthGlide_.setSampleRate(sr_, 20.0f);
    for (DryWetMixer& m : mixer_) m.setSampleRate(sr_);
    applied_ = false; // a new rate: everything below is worked out again
    setParameters(params_);
    reset();
  }

  /** Silence: clears every line, the pre-delay and the loop filters and puts
   *  the drift back where prepare() starts it. A size change in progress
   *  lands at once (on the queued size, if any), and decay, width and mix
   *  land on their settings. */
  void reset() {
    for (DryWetMixer& m : mixer_) m.snap();
    running_ = false;
    // A size queued behind a running fade is where the lines were going:
    // land there. prepare() works the lengths out again and then resets.
    if (hasPending_)
      for (int i = 0; i < Lines; ++i) length_[i] = pending_[i];
    hasPending_ = false;
    fade_ = 1.0f;
    widthGlide_.snap();
    for (int i = 0; i < Lines; ++i) gainGlide_[i].snap();
    for (int i = 0; i < Lines; ++i) {
      line_[i].reset();
      damp_[i].reset();
      // The drift starts where prepare() starts it: two renders after a
      // reset() differed by up to 0.157, so two identical bounces did not
      // null.
      lfoPhase_[i] = (float) i / (float) Lines;
    }
    preDelay_.reset();
  }

  /** Stores the settings; the same values again return at once. Once
   *  process() has run, a new size fades the lines to their new lengths over
   *  50 ms (one arriving mid-fade waits for it to land), and decay, width and
   *  mix glide; damping, modulation and pre-delay land at once. Before then
   *  everything lands. No allocation. */
  void setParameters(const Parameters& p) {
    // The same parameters again are not a change. A plugin passes them once a
    // block, and every call ran the prime search and the pow()s again: 2.5 to
    // 5.2 us a call, 15 to 28% on top of the reverb itself at 64-sample blocks.
    if (applied_ && sameParameters(p, params_)) return;
    applied_ = true;
    params_ = p;
    // Mean length from 20 ms (a booth) to 100 ms (a hall), spread over a
    // factor of 2.5 so the shortest and longest lines differ enough to
    // decorrelate. Then each is moved to the nearest prime.
    const float meanMs = 20.0f + clampf(p.size, 0.0f, 1.0f) * 80.0f;
    const float lo = meanMs / 1.6f, hi = meanMs * 1.6f;
    const int ceiling = MaxDelay - 16; // room for the cubic read + drift
    // When the LONGEST line will not fit, every line shrinks by the same
    // factor, so the spread that decorrelates them survives. They used to be
    // clamped one by one at the ceiling, before the prime step -- so at
    // 192 kHz seven of the eight landed on the same prime, 8171, and the
    // network was a comb fluttering every 42.6 ms instead of a reverb.
    const float longest = hi * 0.001f * sr_;
    const float fit = longest > (float) ceiling ? (float) ceiling / longest : 1.0f;
    // A size changed while audio runs FADES between the old lengths and the
    // new over 50 ms. Every size moves the lines to new primes, so every
    // change was a jump of every read point: 0.5 -> 0.52 stepped the output
    // 65 times its largest natural step, and no smoothing of the knob could
    // help, since each prime is itself a jump.
    //
    // A size that arrives while a fade is still running is QUEUED, as the
    // delay queues a time, and its fade starts when this one lands. It used
    // to restart the fade from where that one was heading, which drops the
    // old head however loud it still is: a size knob turned or automated --
    // a new value every block -- clicked on every block, 11 to 13 times the
    // natural step. The most recent size wins.
    float next[Lines];
    int previous = 0;
    for (int i = 0; i < Lines; ++i) {
      const float t = Lines > 1 ? (float) i / (float) (Lines - 1) : 0.0f;
      const float ms = lo * std::pow(hi / lo, t);
      int samples = (int) (ms * 0.001f * sr_ * fit);
      if (samples > ceiling) samples = ceiling;
      if (samples < 32) samples = 32;
      int prime = nearestPrime(samples, ceiling);
      // DISTINCT, which coprime needs: the spread makes neighbours 14% apart,
      // but a short room at a low rate rounds them closer than one prime gap.
      if (prime <= previous) prime = nextPrimeAbove(previous, ceiling);
      previous = prime;
      next[i] = (float) prime;
      // Jot: -60 dB after decay seconds along every path.
      const float t60 = p.decay > 0.05f ? p.decay : 0.05f;
      gain_[i] = std::pow(10.0f, -3.0f * next[i] / (t60 * sr_));
      // The loop gains and the width glide once audio runs (see Glide): a
      // decay changed in one sample stepped the tail 3.6 times its largest
      // natural step, a width 47 times.
      gainGlide_[i].set(gain_[i]);
    }
    if (running_ && fade_ < 1.0f) {
      bool queued = false;
      for (int i = 0; i < Lines; ++i) queued = queued || next[i] != length_[i];
      for (int i = 0; i < Lines; ++i) pending_[i] = next[i];
      hasPending_ = queued;
    } else {
      bool moved = false;
      for (int i = 0; i < Lines; ++i) moved = moved || next[i] != length_[i];
      if (moved && running_) startFade(next);
      else
        for (int i = 0; i < Lines; ++i) length_[i] = next[i];
      hasPending_ = false;
    }
    fadeStep_ = 1.0f / (0.05f * sr_);
    // Damping: 0 leaves the loop open to 20 kHz, 1 closes it at 1.5 kHz.
    const float d = clampf(p.damping, 0.0f, 1.0f);
    const float cutoff = 20000.0f * std::pow(1500.0f / 20000.0f, d);
    for (int i = 0; i < Lines; ++i) damp_[i].setCutoff(cutoff);
    // In TIME: 4 samples at 48 kHz, 16 at 192. It was 4 samples at every
    // rate, so the drift that keeps the tail from ringing metallic was a
    // quarter as deep at 192 kHz as at 48.
    modDepth_ = clampf(p.modulation, 0.0f, 1.0f) * kMaxDriftSeconds * sr_;
    preDelaySamples_ = clampf(p.preDelayMs, 0.0f, 200.0f) * 0.001f * sr_;
    if (preDelaySamples_ > (float) (kPreDelayMax - 2)) preDelaySamples_ = (float) (kPreDelayMax - 2);
    for (DryWetMixer& m : mixer_) m.setMix(clampf(p.mix, 0.0f, 1.0f));
  }

  /** The longest pre-delay the line holds at the running rate, in ms: the
   *  full 200 up to 80 kHz, 85 at 192. A longer one is clamped to this. */
  float maxPreDelayMs() const {
    const float ms = 1000.0f * (float) (kPreDelayMax - 2) / sr_;
    return ms < 200.0f ? ms : 200.0f;
  }

  /** The read-point drift's depth, in ms, at the running rate. */
  float modulationDepthMs() const { return 1000.0f * modDepth_ / sr_; }

  /** Line `i`'s delay, in samples, at the current rate and size. Whether the
   *  network is a reverb or a comb is decided here -- distinct primes or not
   *  -- so it is worth being able to ask. A size queued behind a running
   *  fade counts: it is where the line will land. */
  float lineLength(int i) const {
    if (i < 0 || i >= Lines) return 0.0f;
    return hasPending_ ? pending_[i] : length_[i];
  }

  /** How long the tail lasts, for the host's tail report. -60 dB by
   *  definition of `decay`; the default floor asks for a further 20 dB. */
  int tailSamples(float floorDb = -80.0f) const {
    // In double and capped: a frozen decay (1e5 s, or infinity) ran the
    // float product past INT_MAX, which is undefined as an int and came out
    // as -2147475456 -- a host told the tail was negative.
    const double t60 = params_.decay > 0.05f ? (double) params_.decay : 0.05;
    const double samples = t60 * (-floorDb / 60.0) * sr_ + MaxDelay;
    return samples < 2147483647.0 ? (int) samples : 2147483647;
  }

  /** One stereo frame, in place. */
  inline void process(float& left, float& right) {
    const float dryL = left, dryR = right;
    const float in = preDelay_.tap(preDelaySamples_, (dryL + dryR) * 0.5f);

    // Read every line (with drift), damp, and scale by its decay gain.
    running_ = true;
    if (hasPending_ && fade_ >= 1.0f) {
      hasPending_ = false;
      startFade(pending_);
    }
    const bool fading = fade_ < 1.0f;
    float gOld = 0.0f, gNew = 1.0f;
    if (fading) {
      fade_ += fadeStep_;
      if (fade_ > 1.0f) fade_ = 1.0f;
      gOld = std::cos(0.5f * kPi * fade_);
      gNew = std::sin(0.5f * kPi * fade_);
    }
    float y[Lines];
    for (int i = 0; i < Lines; ++i) {
      lfoPhase_[i] += lfoInc_[i];
      if (lfoPhase_[i] >= 1.0f) lfoPhase_[i] -= 1.0f;
      const float drift = modDepth_ * fastmath::sinTurns(lfoPhase_[i]);
      const float d = length_[i] + drift;
      float v = modDepth_ > 0.0f ? line_[i].readCubic(d) : line_[i].read(d);
      if (fading) {
        const float dOld = fadeFrom_[i] + drift;
        const float o = modDepth_ > 0.0f ? line_[i].readCubic(dOld) : line_[i].read(dOld);
        v = o * gOld + v * gNew;
      }
      y[i] = damp_[i].lp(v) * gainGlide_[i].next();
    }

    // Mix through the Hadamard: every output is a +/- sum of every input,
    // orthogonal rows, scaled to unit energy.
    float m[Lines];
    for (int i = 0; i < Lines; ++i) m[i] = y[i];
    hadamard(m);

    // Feed back, injecting the input with alternating sign so the lines do
    // not all start in phase.
    for (int i = 0; i < Lines; ++i) {
      const float inject = (i & 1) ? -in : in;
      line_[i].write(flushDenormal(m[i] + inject));
    }

    // Two outputs that both hear EVERY line, at different points along it.
    // The first draft took left and right from two orthogonal sign patterns
    // over the lines, which decorrelated them nicely and left them 2 dB
    // apart in level: the lines are correlated through the matrix, so the
    // cross terms of two different sign patterns do not cancel the same way.
    // Reading every line twice -- the end for one side, the golden-ratio
    // point for the other -- gives two signals with the same statistics
    // (equal level by symmetry) that share no samples (decorrelated by the
    // 10-40 ms between the taps). Static reads, so linear is exact.
    //
    // The right tap moves with the size too, so it fades with the lines: it
    // read only the new length, and a size change jumped its read point in
    // one sample -- 0.5 -> 0.8 stepped the right channel 40 times its
    // largest natural step while the left, faded, stayed under one.
    float wetL = 0.0f, wetR = 0.0f;
    for (int i = 0; i < Lines; ++i) {
      const float s = (i & 1) ? -1.0f : 1.0f; // the injection pattern, undone
      wetL += s * y[i];
      float r = line_[i].read(length_[i] * kGolden);
      if (fading) r = line_[i].read(fadeFrom_[i] * kGolden) * gOld + r * gNew;
      wetR += s * r;
    }
    const float norm = 1.0f / std::sqrt((float) Lines);
    wetL *= norm;
    wetR *= norm;

    // Width: each side keeps (1 + w) / 2 of its own output and takes
    // (1 - w) / 2 of the other's, so 0 is mono. It took w and 1 - w, mono at
    // 0.5, and at 0 the two outputs swapped: full width, sides crossed.
    widthGlide_.set(clampf(params_.width, 0.0f, 1.0f));
    const float w = widthGlide_.next();
    const float own = 0.5f * (1.0f + w), other = 0.5f * (1.0f - w);
    const float outL = wetL * own + wetR * other;
    const float outR = wetR * own + wetL * other;
    left = mixer_[0].process(dryL, outL);
    right = mixer_[1].process(dryR, outR);
  }

private:
  static constexpr int kPreDelayMax = 16384; // 200 ms at 80 kHz; clamped above
  static constexpr float kMaxDriftSeconds = 4.0f / 48000.0f;
  static constexpr float kGolden = 0.6180339887f; // where the right tap sits

  /** In-place fast Walsh-Hadamard, normalised to be unitary. */
  static inline void hadamard(float* v) {
    for (int len = 1; len < Lines; len <<= 1) {
      for (int i = 0; i < Lines; i += len << 1) {
        for (int j = i; j < i + len; ++j) {
          const float a = v[j], b = v[j + len];
          v[j] = a + b;
          v[j + len] = a - b;
        }
      }
    }
    const float norm = 1.0f / std::sqrt((float) Lines);
    for (int i = 0; i < Lines; ++i) v[i] *= norm;
  }

  /** Fade from the lengths the lines have to `to`. */
  void startFade(const float* to) {
    for (int i = 0; i < Lines; ++i) {
      fadeFrom_[i] = length_[i];
      length_[i] = to[i];
    }
    fade_ = 0.0f;
  }

  static bool isPrime(int n) {
    if (n < 2) return false;
    for (int d = 2; d * d <= n; ++d)
      if (n % d == 0) return false;
    return true;
  }
  /** The prime nearest to n, searching outwards; never above `ceiling`. */
  static int nearestPrime(int n, int ceiling) {
    for (int step = 0; step < n; ++step) {
      if (n + step <= ceiling && isPrime(n + step)) return n + step;
      if (isPrime(n - step)) return n - step;
    }
    return 2;
  }
  /** The first prime above n, or the largest under the ceiling if none fits. */
  static int nextPrimeAbove(int n, int ceiling) {
    for (int c = n + 1; c <= ceiling; ++c)
      if (isPrime(c)) return c;
    return nearestPrime(ceiling, ceiling);
  }

  static bool sameParameters(const Parameters& a, const Parameters& b) {
    return a.size == b.size && a.decay == b.decay && a.damping == b.damping &&
           a.modulation == b.modulation && a.preDelayMs == b.preDelayMs && a.width == b.width &&
           a.mix == b.mix;
  }
  static_assert(sizeof(Parameters) == 7 * sizeof(float),
                "a field added to Parameters must be compared in sameParameters() too");
  bool applied_ = false; // setParameters() has worked out params_ at this rate
  DelayLine<MaxDelay> line_[Lines];
  DelayLine<kPreDelayMax> preDelay_;
  OnePole damp_[Lines];
  float length_[Lines]{}, gain_[Lines]{};
  float lfoPhase_[Lines]{}, lfoInc_[Lines]{};
  float fadeFrom_[Lines]{}, fade_ = 1.0f, fadeStep_ = 1.0f / 2400.0f;
  float pending_[Lines]{}; // a size's lengths queued behind a running fade
  bool hasPending_ = false;
  bool running_ = false;
  Glide gainGlide_[Lines], widthGlide_{1.0f, 1.0f};
  float sr_ = 48000.0f, modDepth_ = 0.0f, preDelaySamples_ = 480.0f;
  Parameters params_{};
  /** One mix glide per side, set to the rate in prepare(). One shared by
   *  both stepped twice a frame, gliding in half its time, and was never
   *  given a rate. */
  DryWetMixer mixer_[2];
};

} // namespace sonore
