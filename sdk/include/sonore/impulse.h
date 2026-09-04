// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: impulse responses, prepared for convolution.
//
// Convolver in fft.h takes a mono array of samples and convolves fast. What a
// convolution REVERB needs on top of that is everything between a file on
// disk and that array, and it is more than it looks:
//
//   * the file is at ITS rate, not the session's, and an IR played at the
//     wrong rate is transposed: a hall becomes a bathroom;
//   * a stereo IR is two responses, not one interleaved stream;
//   * loading two IRs at the same peak does NOT make them equally loud, so
//     normalising by peak leaves the user riding the mix control every time
//     they change preset;
//   * a recorded IR usually starts with silence, which is pre-delay the user
//     did not ask for.
//
// All of it is offline work: this reads files, allocates and resamples, so it
// belongs in prepare() or a message-thread handler, never in process().

#pragma once

#include <cmath>
#include <cstddef>
#include <vector>

#include "audiofile.h"
#include "resample.h"

namespace sonore {

/** How to make two impulse responses equally loud. */
enum class ImpulseNormalise {
  None,
  Peak,   // loudest sample becomes 1.0: predictable headroom, uneven loudness
  Energy, // total energy normalised: even LOUDNESS, which is what a preset
          // switch should preserve
};

/**
 * A loaded, rate-matched impulse response, one array per channel.
 *
 * Channels are kept SEPARATE rather than interleaved because that is what a
 * convolver wants: one partitioned FFT per channel, each fed its own response.
 */
class ImpulseResponse {
public:
  /** Load from any format audiofile.h reads, resampled to `targetSampleRate`. */
  bool loadFromFile(const char* path, double targetSampleRate,
                    ImpulseNormalise normalise = ImpulseNormalise::Energy) {
    WavData file;
    if (!readAudioFile(path, &file) || file.numChannels == 0 || file.samples.empty())
      return false;
    return loadFromSamples(file.samples.data(), file.numFrames(), file.numChannels,
                           (double) file.sampleRate, targetSampleRate, normalise);
  }

  /** Load from memory. `sourceSampleRate` is what the samples ARE, not what
   *  they should become. */
  bool loadFromSamples(const float* interleaved, size_t numFrames, uint16_t numChannels,
                       double sourceSampleRate, double targetSampleRate,
                       ImpulseNormalise normalise = ImpulseNormalise::Energy) {
    if (!interleaved || numFrames == 0 || numChannels == 0) return false;
    const double source = sourceSampleRate > 0.0 ? sourceSampleRate : 48000.0;
    const double target = targetSampleRate > 0.0 ? targetSampleRate : source;

    channels_.assign(numChannels, {});
    for (uint16_t c = 0; c < numChannels; ++c) {
      std::vector<float>& out = channels_[c];
      out.resize(numFrames);
      for (size_t i = 0; i < numFrames; ++i)
        out[i] = interleaved[i * (size_t) numChannels + (size_t) c];
    }

    sampleRate_ = source;
    if (std::fabs(target - source) > 1e-6) resampleTo(target);
    applyNormalisation(normalise);
    return !channels_.empty() && !channels_[0].empty();
  }

  /**
   * Drop leading and trailing near-silence.
   *
   * The leading part is PRE-DELAY: trimming it moves the reverb earlier, which
   * is usually what a user wants from a raw recording and never what they want
   * from an IR someone already designed. Hence opt-in, with the amount removed
   * reported so a plugin can give it back as a control.
   */
  size_t trimSilence(float thresholdDb = -60.0f) {
    if (channels_.empty()) return 0;
    const float threshold = std::pow(10.0f, thresholdDb * 0.05f);
    const size_t length = channels_[0].size();

    size_t first = length, last = 0;
    for (const std::vector<float>& channel : channels_)
      for (size_t i = 0; i < channel.size(); ++i)
        if (std::fabs(channel[i]) >= threshold) {
          if (i < first) first = i;
          if (i > last) last = i;
        }
    if (first > last) return 0; // the whole thing is below threshold: leave it

    for (std::vector<float>& channel : channels_) {
      std::vector<float> trimmed(channel.begin() + (long) first, channel.begin() + (long) last + 1);
      channel.swap(trimmed);
    }
    return first;
  }

  size_t numChannels() const { return channels_.size(); }
  size_t length() const { return channels_.empty() ? 0 : channels_[0].size(); }
  double sampleRate() const { return sampleRate_; }

  /** The response for one channel. A mono IR answers for every channel, which
   *  is what makes a mono file usable on a stereo bus without a special case
   *  at every call site. */
  const float* channel(size_t index) const {
    if (channels_.empty()) return nullptr;
    return channels_[index < channels_.size() ? index : 0].data();
  }

  /** Total energy, the quantity Energy normalisation equalises. */
  double energy() const {
    double sum = 0.0;
    for (const std::vector<float>& channel : channels_)
      for (float v : channel) sum += (double) v * (double) v;
    return sum;
  }

  float magnitude() const {
    float peak = 0.0f;
    for (const std::vector<float>& channel : channels_)
      for (float v : channel) {
        const float a = v < 0.0f ? -v : v;
        if (a > peak) peak = a;
      }
    return peak;
  }

private:
  void resampleTo(double target) {
    const double ratio = target / sampleRate_;
    for (std::vector<float>& channel : channels_) {
      const size_t outLength = Resampler::outputLength(channel.size(), ratio);
      if (outLength == 0) continue;
      std::vector<float> out(outLength);
      Resampler::resample(channel.data(), channel.size(), out.data(), out.size(), ratio);
      channel.swap(out);
    }
    sampleRate_ = target;
  }

  void applyNormalisation(ImpulseNormalise mode) {
    if (mode == ImpulseNormalise::None || channels_.empty()) return;

    float gain = 1.0f;
    if (mode == ImpulseNormalise::Peak) {
      const float peak = magnitude();
      gain = peak > 1e-9f ? 1.0f / peak : 1.0f;
    } else {
      // Energy, not peak: two IRs at the same peak are NOT equally loud, and
      // a user switching presets should not have to ride the mix control.
      // Normalising to unit energy means convolving with either leaves the
      // signal at the same level.
      const double total = energy();
      gain = total > 1e-12 ? (float) (1.0 / std::sqrt(total)) : 1.0f;
    }
    for (std::vector<float>& channel : channels_)
      for (float& v : channel) v *= gain;
  }

  std::vector<std::vector<float>> channels_;
  double sampleRate_ = 48000.0;
};

} // namespace sonore
