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
//   keeps its size at 96 kHz.
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
#include <cmath>
#include "audio.h"

namespace sonore {

/**
 * SIZE WARNING, the same one the Convolver carries: eight 8192-sample lines
 * plus a pre-delay is ~320 KB of state, which CANNOT live on the stack in
 * wasm (64 KB). A real plugin is fine -- the ABI puts SonoreDsp in static
 * storage -- but a local FdnReverb in a test or a helper crashes there.
 */
template <int Lines = 8, int MaxDelay = 8192>
class FdnReverb {
  static_assert(Lines >= 2 && (Lines & (Lines - 1)) == 0,
                "a Hadamard matrix needs a power-of-two number of lines");
  static_assert(MaxDelay >= 512, "the lines need room for a room");

public:
  struct Parameters {
    float size = 0.5f;       // 0..1, scales the delay lengths (20 ms .. 100 ms mean)
    float decay = 2.0f;      // seconds to -60 dB
    float damping = 0.3f;    // 0..1, high-frequency loss in the loop
    float modulation = 0.3f; // 0..1, read-point drift, up to ~4 samples
    float preDelayMs = 10.0f;
    float width = 1.0f;      // 0..1 stereo spread
    float mix = 0.3f;        // 0..1 dry/wet
  };

  void prepare(const ProcessSpec& spec) {
    sr_ = (float) spec.sampleRate;
    for (int i = 0; i < Lines; ++i) {
      damp_[i].setSampleRate(sr_);
      // Distinct rates, all well under 1 Hz, so no two lines drift together.
      lfoInc_[i] = (0.11f + 0.083f * (float) i) / sr_;
      lfoPhase_[i] = (float) i / (float) Lines;
    }
    setParameters(params_);
    reset();
  }

  void reset() {
    for (int i = 0; i < Lines; ++i) {
      line_[i].reset();
      damp_[i].reset();
    }
    preDelay_.reset();
  }

  void setParameters(const Parameters& p) {
    params_ = p;
    // Mean length from 20 ms (a booth) to 100 ms (a hall), spread over a
    // factor of 2.5 so the shortest and longest lines differ enough to
    // decorrelate. Then each is moved to the nearest prime.
    const float meanMs = 20.0f + clampf(p.size, 0.0f, 1.0f) * 80.0f;
    const float lo = meanMs / 1.6f, hi = meanMs * 1.6f;
    for (int i = 0; i < Lines; ++i) {
      const float t = Lines > 1 ? (float) i / (float) (Lines - 1) : 0.0f;
      const float ms = lo * std::pow(hi / lo, t);
      int samples = (int) (ms * 0.001f * sr_);
      const int ceiling = MaxDelay - 16; // room for the cubic read + drift
      if (samples > ceiling) samples = ceiling;
      if (samples < 32) samples = 32;
      length_[i] = (float) nearestPrime(samples, ceiling);
      // Jot: -60 dB after decay seconds along every path.
      const float t60 = p.decay > 0.05f ? p.decay : 0.05f;
      gain_[i] = std::pow(10.0f, -3.0f * length_[i] / (t60 * sr_));
    }
    // Damping: 0 leaves the loop open to 20 kHz, 1 closes it at 1.5 kHz.
    const float d = clampf(p.damping, 0.0f, 1.0f);
    const float cutoff = 20000.0f * std::pow(1500.0f / 20000.0f, d);
    for (int i = 0; i < Lines; ++i) damp_[i].setCutoff(cutoff);
    modDepth_ = clampf(p.modulation, 0.0f, 1.0f) * 4.0f;
    preDelaySamples_ = clampf(p.preDelayMs, 0.0f, 200.0f) * 0.001f * sr_;
    if (preDelaySamples_ > (float) (kPreDelayMax - 2)) preDelaySamples_ = (float) (kPreDelayMax - 2);
    mixer_.setMix(clampf(p.mix, 0.0f, 1.0f));
  }

  /** How long the tail lasts, for the host's tail report. -60 dB by
   *  definition of `decay`; the default floor asks for a further 20 dB. */
  int tailSamples(float floorDb = -80.0f) const {
    const float t60 = params_.decay > 0.05f ? params_.decay : 0.05f;
    return (int) (t60 * (-floorDb / 60.0f) * sr_) + MaxDelay;
  }

  /** One stereo frame, in place. */
  inline void process(float& left, float& right) {
    const float dryL = left, dryR = right;
    const float in = preDelay_.tap(preDelaySamples_, (dryL + dryR) * 0.5f);

    // Read every line (with drift), damp, and scale by its decay gain.
    float y[Lines];
    for (int i = 0; i < Lines; ++i) {
      lfoPhase_[i] += lfoInc_[i];
      if (lfoPhase_[i] >= 1.0f) lfoPhase_[i] -= 1.0f;
      const float drift = modDepth_ * fastmath::sinTurns(lfoPhase_[i]);
      const float d = length_[i] + drift;
      const float v = modDepth_ > 0.0f ? line_[i].readCubic(d) : line_[i].read(d);
      y[i] = damp_[i].lp(v) * gain_[i];
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
    float wetL = 0.0f, wetR = 0.0f;
    for (int i = 0; i < Lines; ++i) {
      const float s = (i & 1) ? -1.0f : 1.0f; // the injection pattern, undone
      wetL += s * y[i];
      wetR += s * line_[i].read(length_[i] * kGolden);
    }
    const float norm = 1.0f / std::sqrt((float) Lines);
    wetL *= norm;
    wetR *= norm;

    const float w = clampf(params_.width, 0.0f, 1.0f);
    const float outL = wetL * w + wetR * (1.0f - w);
    const float outR = wetR * w + wetL * (1.0f - w);
    left = mixer_.process(dryL, outL);
    right = mixer_.process(dryR, outR);
  }

private:
  static constexpr int kPreDelayMax = 16384; // 200 ms at 80 kHz; clamped above
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

  DelayLine<MaxDelay> line_[Lines];
  DelayLine<kPreDelayMax> preDelay_;
  OnePole damp_[Lines];
  float length_[Lines]{}, gain_[Lines]{};
  float lfoPhase_[Lines]{}, lfoInc_[Lines]{};
  float sr_ = 48000.0f, modDepth_ = 0.0f, preDelaySamples_ = 480.0f;
  Parameters params_{};
  DryWetMixer mixer_;
};

} // namespace sonore
