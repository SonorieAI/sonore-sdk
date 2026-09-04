// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: mixing between channel layouts.
//
// A plugin that accepts 5.1 and is asked for stereo has to fold six channels
// into two, and the coefficients are not a matter of taste: ITU-R BS.775 says
// centre and surrounds come in at -3 dB, and a downmix that ignores it is
// either centre-heavy or centre-shy on every film stem it touches.
//
// The usual shape for this is a generic matrix that leaves the coefficients
// to the caller. We can do better, because ProcessContext already carries what each
// channel MEANS: given the roles on both sides, the matrix derives itself, and
// the one thing nobody remembers, that LFE must not be folded into full-range
// channels: is handled rather than left as a footnote.

#pragma once

#include <cmath>
#include <cstddef>

#include "audio.h"

namespace sonore {

/**
 * An N-in, M-out mixing matrix.
 *
 * Small and fixed-size: a channel count is bounded by kMaxAudioChannels, so
 * this never allocates and can be built in prepare() or rebuilt whenever a
 * layout changes.
 */
class ChannelMatrix {
public:
  static constexpr size_t kMaxChannels = 8;

  void clear() {
    for (size_t o = 0; o < kMaxChannels; ++o)
      for (size_t i = 0; i < kMaxChannels; ++i) gain_[o][i] = 0.0f;
    inputs_ = outputs_ = 0;
  }

  void setSize(size_t numInputs, size_t numOutputs) {
    inputs_ = numInputs < kMaxChannels ? numInputs : kMaxChannels;
    outputs_ = numOutputs < kMaxChannels ? numOutputs : kMaxChannels;
  }

  void setGain(size_t output, size_t input, float gain) {
    if (output < kMaxChannels && input < kMaxChannels) gain_[output][input] = gain;
  }

  float getGain(size_t output, size_t input) const {
    if (output >= kMaxChannels || input >= kMaxChannels) return 0.0f;
    return gain_[output][input];
  }

  size_t numInputs() const { return inputs_; }
  size_t numOutputs() const { return outputs_; }

  /**
   * Build the matrix from what the channels MEAN.
   *
   * Every output takes its own role at unity when the input has it. What is
   * left over is folded by the standard rules: centre and surrounds split
   * across front left and right at -3 dB, sides likewise. LFE is deliberately
   * NOT folded into full-range outputs: a subwoofer channel summed into the
   * mains is 10 dB of mud and the single most common downmix mistake.
   */
  void buildFromRoles(const uint8_t* inputRoles, size_t numInputs, const uint8_t* outputRoles,
                      size_t numOutputs) {
    clear();
    setSize(numInputs, numOutputs);
    if (!inputRoles || !outputRoles) return;

    auto findOutput = [&](uint8_t role) -> long {
      for (size_t o = 0; o < outputs_; ++o)
        if (outputRoles[o] == role) return (long) o;
      return -1;
    };

    const float minus3dB = 0.70794578f; // 10^(-3/20)

    for (size_t i = 0; i < inputs_; ++i) {
      const uint8_t role = inputRoles[i];
      const long direct = findOutput(role);
      if (direct >= 0) {
        gain_[(size_t) direct][i] = 1.0f;
        continue;
      }

      // No matching output: fold it where the standard says it goes.
      const long left = findOutput(kChannelFL);
      const long right = findOutput(kChannelFR);
      const long mono = findOutput(kChannelFC);

      if (role == kChannelLFE) continue; // never folded into full-range outputs

      if (left < 0 && right < 0) {
        // Folding everything to a single channel: a mono output.
        if (mono >= 0) gain_[(size_t) mono][i] = minus3dB;
        continue;
      }

      switch (role) {
        case kChannelFC:
        case kChannelBC:
        case kChannelTC:
          // Centred content splits equally, at -3 dB so the sum keeps its
          // level rather than gaining 3 dB in the middle.
          if (left >= 0) gain_[(size_t) left][i] = minus3dB;
          if (right >= 0) gain_[(size_t) right][i] = minus3dB;
          break;
        case kChannelBL:
        case kChannelSL:
        case kChannelTFL:
        case kChannelTBL:
        case kChannelFLC:
          if (left >= 0) gain_[(size_t) left][i] = minus3dB;
          break;
        case kChannelBR:
        case kChannelSR:
        case kChannelTFR:
        case kChannelTBR:
        case kChannelFRC:
          if (right >= 0) gain_[(size_t) right][i] = minus3dB;
          break;
        default:
          // An unmodelled role is dropped rather than smeared across the
          // fronts, where it would arrive at the wrong level and the wrong
          // place.
          break;
      }
    }

    // A mono INPUT feeding a wider output has the opposite problem: nothing
    // to fold, everything to spread.
    if (inputs_ == 1 && outputs_ > 1 && inputRoles[0] == kChannelFC) {
      const long left = findOutput(kChannelFL);
      const long right = findOutput(kChannelFR);
      if (left >= 0 && right >= 0) {
        gain_[(size_t) left][0] = minus3dB;
        gain_[(size_t) right][0] = minus3dB;
      }
    }
  }

  /** Mid/side encode: M = (L+R)/sqrt2, S = (L-R)/sqrt2. The root-two keeps the
   *  round trip unity, which a plain halving does not. */
  void setMidSideEncode() {
    clear();
    setSize(2, 2);
    const float k = 0.70710678f;
    gain_[0][0] = k;
    gain_[0][1] = k;
    gain_[1][0] = k;
    gain_[1][1] = -k;
  }

  /** Its exact inverse, so encode followed by decode returns the input. */
  void setMidSideDecode() { setMidSideEncode(); } // the matrix is its own inverse

  /** Apply in place. `scratch` must hold at least numInputs samples and exists
   *  because an in-place matrix multiply would otherwise read outputs it had
   *  already overwritten: the bug that turns a downmix into feedback. */
  void process(AudioBlock<float>& block, float* scratch) const {
    if (!scratch || outputs_ == 0) return;
    const size_t channels = block.getNumChannels();
    const size_t samples = block.getNumSamples();
    const size_t ins = inputs_ < channels ? inputs_ : channels;
    const size_t outs = outputs_ < channels ? outputs_ : channels;

    for (size_t s = 0; s < samples; ++s) {
      for (size_t i = 0; i < ins; ++i) scratch[i] = block.getChannelPointer(i)[s];
      for (size_t o = 0; o < outs; ++o) {
        float sum = 0.0f;
        for (size_t i = 0; i < ins; ++i) sum += scratch[i] * gain_[o][i];
        block.getChannelPointer(o)[s] = sum;
      }
    }
  }

private:
  float gain_[kMaxChannels][kMaxChannels] = {};
  size_t inputs_ = 0, outputs_ = 0;
};

} // namespace sonore
