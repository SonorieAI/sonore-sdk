// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the delay as a finished machine.
//
// DelayLine in dsp.h is the primitive: a ring you write and read. Every
// "delay plugin" a user asks for is the same machine on top of it -- two
// lines, feedback with a filter in the loop, ping-pong routing, a little
// modulation, tempo sync -- and every one of them has been rebuilt from the
// primitive by hand, which is where the time knob comes out with a zipper and
// the feedback runs away. This is that machine, once.
//
// The one decision that separates a good delay from a bad one is what happens
// when the TIME changes while audio is running:
//
//   CROSSFADE   the old delay keeps reading while a second read head opens at
//               the new time, and the two are faded over ~50 ms with
//               equal-power gains. No pitch shift, no click. What a digital
//               delay does, and the default. A change that arrives while a
//               fade is still running is QUEUED and starts when the fade
//               lands: restarting a fade from a blend of two delay times would
//               jump the old head, which is the click this mode exists to
//               avoid, and a dragged knob arrives as dozens of changes.
//   GLIDE       the read point slides from the old time to the new, so the
//               audio in flight is pitched down or up while it slides -- the
//               tape-echo "spin", which some people turn the knob for. The
//               slide is a one-pole with a 100 ms time constant AT ANY RATE
//               (the coefficient is derived in prepare, not a constant), and
//               its speed is CAPPED at half a sample per sample: a one-pole
//               alone moves a 100 ms change at one sample per sample for its
//               first moments, which is not a spin, it is the audio stopping
//               dead. Capped, a large change pitches down by an octave (or up
//               by a fifth) and eases into the new time.
//
// Both are here because both are real, and a delay that only does one is
// missing half the users. Every read is the cubic tap: a static delay at a
// fractional time through the linear tap is a lowpass, and switching taps at
// the end of a fade is a step the ear can hear.
//
// Included by dsp.h.
#pragma once
#include <cmath>
#include "audio.h"

namespace sonore {

/**
 * SIZE: MaxSamples floats per channel -- the default holds four seconds at
 * 48 kHz in 1.5 MB for stereo. A member of the plugin, never a local.
 */
template <int MaxSamples = 192000>
class StereoDelay {
  static_assert(MaxSamples >= 1024, "a delay needs at least some room");

public:
  enum class TimeChange { Crossfade, Glide };

  struct Parameters {
    float timeMs = 375.0f;      // left channel, or both
    float timeRightMs = 0.0f;   // 0 = same as left
    float feedback = 0.4f;      // 0..1 of the delayed signal fed back
    float dampingHz = 6000.0f;  // lowpass in the loop: each repeat darker
    float lowCutHz = 80.0f;     // highpass in the loop: no mud build-up
    float modDepthMs = 0.0f;    // read-point modulation, tape wobble
    float modRateHz = 0.5f;
    float mix = 0.35f;
    bool pingPong = false;
  };

  void prepare(const ProcessSpec& spec) {
    sr_ = (float) spec.sampleRate;
    for (int c = 0; c < 2; ++c) {
      damp_[c].setSampleRate(sr_);
      cut_[c].setSampleRate(sr_);
    }
    fadeSamples_ = (int) (0.05f * sr_);
    // 100 ms time constant, whatever the rate.
    glide_ = 1.0 - std::exp(-1.0 / (0.1 * (double) sr_));
    lfoInc_ = 0.0f;
    setParameters(params_);
    reset();
  }

  void reset() {
    for (int c = 0; c < 2; ++c) {
      line_[c].reset();
      damp_[c].reset();
      cut_[c].reset();
      current_[c] = target_[c];
      fadeFrom_[c] = target_[c];
      fadePos_[c] = 1.0f;
      hasPending_[c] = false;
    }
    lfoPhase_ = 0.0f;
  }

  void setTimeChange(TimeChange mode) { mode_ = mode; }

  void setParameters(const Parameters& p) {
    params_ = p;
    const float maxMs = 1000.0f * (float) (MaxSamples - 8) / sr_;
    const float l = clampf(p.timeMs, 1.0f, maxMs);
    const float r = p.timeRightMs > 0.0f ? clampf(p.timeRightMs, 1.0f, maxMs) : l;
    setTime(0, l * 0.001f * sr_);
    setTime(1, r * 0.001f * sr_);
    // Above 0.98 the loop never stops; 1.0 would be an oscillator.
    feedback_ = clampf(p.feedback, 0.0f, 0.98f);
    for (int c = 0; c < 2; ++c) {
      damp_[c].setCutoff(clampf(p.dampingHz, 200.0f, sr_ * 0.45f));
      cut_[c].setCutoff(clampf(p.lowCutHz, 10.0f, 2000.0f));
    }
    modDepth_ = clampf(p.modDepthMs, 0.0f, 20.0f) * 0.001f * sr_;
    lfoInc_ = clampf(p.modRateHz, 0.01f, 20.0f) / sr_;
    mixer_.setMix(clampf(p.mix, 0.0f, 1.0f));
  }

  /** Set the time from the host's tempo: a dotted eighth at whatever the
   *  song is running at. Reads the TransportInfo the wrapper hands the DSP. */
  void syncTo(const TransportInfo& t, int denominator, NoteFlavour flavour = NoteFlavour::Straight,
              bool rightToo = true) {
    const double samples = noteLengthInSamples(t, (double) sr_, denominator, flavour);
    Parameters p = params_;
    p.timeMs = (float) (samples * 1000.0 / sr_);
    if (rightToo) p.timeRightMs = 0.0f;
    setParameters(p);
  }

  /** The delay each channel is heading for, in samples: a change queued
   *  behind a running fade counts, because it is where the head will land. */
  float delaySamples(int channel) const {
    const int c = channel & 1;
    return hasPending_[c] ? pending_[c] : target_[c];
  }

  /** How long the repeats last, for the host's tail report: the time it takes
   *  the feedback to fall to `floorDb` (a further 20 below -60 by default). */
  int tailSamples(float floorDb = -80.0f) const {
    const float longest = target_[0] > target_[1] ? target_[0] : target_[1];
    if (feedback_ <= 0.0f) return (int) longest + 8;
    const float perRepeatDb = 20.0f * std::log10(feedback_); // negative
    const float repeats = floorDb / perRepeatDb;
    return (int) (repeats * longest) + 8;
  }

  /** One stereo frame, in place. */
  inline void process(float& left, float& right) {
    const float dryL = left, dryR = right;
    lfoPhase_ += lfoInc_;
    if (lfoPhase_ >= 1.0f) lfoPhase_ -= 1.0f;
    const float wobble = modDepth_ > 0.0f ? modDepth_ * fastmath::sinTurns(lfoPhase_) : 0.0f;

    const float outL = readHead(0, wobble);
    const float outR = readHead(1, wobble);

    // Loop filters: darker and cleaner on every pass.
    const float fbL = cut_[0].hp(damp_[0].lp(outL)) * feedback_;
    const float fbR = cut_[1].hp(damp_[1].lp(outR)) * feedback_;

    if (params_.pingPong) {
      // The input goes in on the left, and each repeat crosses over: what
      // came out of the left line goes into the right, and back again.
      line_[0].write(flushDenormal((dryL + dryR) * 0.5f + fbR));
      line_[1].write(flushDenormal(fbL));
    } else {
      line_[0].write(flushDenormal(dryL + fbL));
      line_[1].write(flushDenormal(dryR + fbR));
    }

    left = mixer_.process(dryL, outL);
    right = mixer_.process(dryR, outR);
  }

private:
  void setTime(int c, float samples) {
    if (mode_ == TimeChange::Glide) {
      target_[c] = samples;
      hasPending_[c] = false;
      return;
    }
    if (fadePos_[c] < 1.0f) {
      // Mid-fade: remember where to go next, and go there when this fade
      // lands. The most recent request wins.
      pending_[c] = samples;
      hasPending_[c] = true;
      return;
    }
    hasPending_[c] = false;
    startFade(c, samples);
  }
  void startFade(int c, float samples) {
    if (samples == target_[c]) return;
    fadeFrom_[c] = target_[c];
    fadePos_[c] = 0.0f;
    target_[c] = samples;
  }

  inline float readHead(int c, float wobble) {
    if (mode_ == TimeChange::Glide) {
      // Slide the delay itself, and the pitch bends while it moves. The
      // rate is a one-pole, capped at half a sample per sample, so a big
      // move spins at a fixed pitch and then eases in. The position is a
      // DOUBLE and snaps at the end: in float the one-pole's last steps fall
      // below half an ulp of a 9600-sample delay and it parks two samples
      // short of where it was sent -- the test that lands a glide on an
      // integer delay and asks for the input back exactly is what found it.
      double step = (target_[c] - current_[c]) * glide_;
      if (step > kMaxSlew) step = kMaxSlew;
      if (step < -kMaxSlew) step = -kMaxSlew;
      current_[c] += step;
      if (std::fabs(target_[c] - current_[c]) < 1e-6) current_[c] = target_[c];
      return line_[c].readCubic((float) current_[c] + wobble);
    }
    if (fadePos_[c] >= 1.0f) {
      if (hasPending_[c]) {
        hasPending_[c] = false;
        startFade(c, pending_[c]);
      }
      if (fadePos_[c] >= 1.0f) {
        current_[c] = target_[c];
        return line_[c].readCubic(target_[c] + wobble);
      }
    }
    // Two heads, equal-power. The old one keeps playing exactly what it was
    // playing; the new one opens at the new time. Nothing is pitched.
    fadePos_[c] += 1.0f / (float) (fadeSamples_ > 0 ? fadeSamples_ : 1);
    if (fadePos_[c] > 1.0f) fadePos_[c] = 1.0f;
    const float t = fadePos_[c];
    const float gOld = std::cos(0.5f * kPi * t), gNew = std::sin(0.5f * kPi * t);
    const float a = line_[c].readCubic(fadeFrom_[c] + wobble);
    const float b = line_[c].readCubic(target_[c] + wobble);
    current_[c] = target_[c];
    return a * gOld + b * gNew;
  }

  static constexpr float kMaxSlew = 0.5f; // samples of delay per sample: an octave down

  DelayLine<MaxSamples> line_[2];
  OnePole damp_[2], cut_[2];
  DryWetMixer mixer_;
  Parameters params_{};
  TimeChange mode_ = TimeChange::Crossfade;
  float sr_ = 48000.0f, feedback_ = 0.4f, modDepth_ = 0.0f, lfoInc_ = 0.0f, lfoPhase_ = 0.0f;
  double glide_ = 1.0 / 4800.0;
  float target_[2] = {18000.0f, 18000.0f};
  double current_[2] = {18000.0, 18000.0};
  float fadeFrom_[2] = {18000.0f, 18000.0f}, fadePos_[2] = {1.0f, 1.0f};
  float pending_[2] = {18000.0f, 18000.0f};
  bool hasPending_[2] = {false, false};
  int fadeSamples_ = 2400;
};

} // namespace sonore
