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
  None,   ///< the level the file has, untouched
  Peak,   ///< loudest sample becomes 1.0: predictable headroom, uneven loudness
  /** Unit energy per channel as measured at 48 kHz: even LOUDNESS, which is
   *  what a preset switch should preserve, at whatever rate it plays. The
   *  default. */
  Energy,
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

  /** Load from a FILE'S BYTES in memory -- a WAV, AIFF, FLAC, MP3 or Ogg
   *  compiled into the plugin, so the factory impulses ship inside the
   *  binary and cannot go missing -- decoded as loadFromFile() decodes a
   *  path, and resampled to `targetSampleRate`. */
  bool loadFromFileData(const void* fileData, size_t size, double targetSampleRate,
                        ImpulseNormalise normalise = ImpulseNormalise::Energy) {
    WavData file;
    if (!readAudioFile(fileData, size, &file) || file.numChannels == 0 || file.samples.empty())
      return false;
    return loadFromSamples(file.samples.data(), file.numFrames(), file.numChannels,
                           (double) file.sampleRate, targetSampleRate, normalise);
  }

  /** Load from samples already decoded, interleaved. `sourceSampleRate` is
   *  what the samples ARE, not what they should become. */
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
    // Normalised as the file is, THEN converted -- and the conversion keeps
    // the sound (see resampleTo). The other way round, the level depended on
    // the rate the session ran at: unit energy measured at 96 kHz is 3 dB
    // louder than at 48, and a peak of 1 twice as loud. The same reverb was
    // 3 to 6 dB louder at every doubling.
    applyNormalisation(normalise);
    if (std::fabs(target - source) > 1e-6) resampleTo(target);
    return !channels_.empty() && !channels_[0].empty();
  }

  /**
   * Drop leading and trailing near-silence.
   *
   * The leading part is PRE-DELAY: trimming it moves the reverb earlier, which
   * is usually what a user wants from a raw recording and never what they want
   * from an IR someone already designed. Hence opt-in, with the amount removed
   * reported so a plugin can give it back as a control.
   *
   * The threshold is in dB below the response's own loudest sample. It was in
   * dBFS, but by then the response is normalised -- Energy leaves a reverb
   * peaking 24 to 33 dB under full scale -- so -60 dBFS cut a 3-second
   * reverb 27 dB down its decay, at 1.3 s, dead. What is cut from the end is
   * faded into over the last 10 ms (an eighth of what is left, at most), so
   * the response does not end on a step either.
   */
  size_t trimSilence(float thresholdDb = -60.0f) {
    if (channels_.empty()) return 0;
    const float peak = magnitude();
    const float threshold = peak * std::pow(10.0f, thresholdDb * 0.05f);
    const size_t length = channels_[0].size();

    size_t first = length, last = 0;
    for (const std::vector<float>& channel : channels_)
      for (size_t i = 0; i < channel.size(); ++i)
        if (std::fabs(channel[i]) >= threshold) {
          if (i < first) first = i;
          if (i > last) last = i;
        }
    if (peak <= 0.0f || first > last) return 0; // silent: leave it

    const size_t kept = last + 1 - first;
    size_t fade = 0;
    if (last + 1 < length) {
      fade = (size_t) (0.01 * sampleRate_);
      if (fade > kept / 8) fade = kept / 8;
    }
    for (std::vector<float>& channel : channels_) {
      std::vector<float> trimmed(channel.begin() + (long) first, channel.begin() + (long) last + 1);
      // Raised cosine over the last `fade` samples, down to where the cut was.
      for (size_t i = 0; i < fade; ++i)
        trimmed[kept - 1 - i] *= 0.5f - 0.5f * std::cos(3.14159265358979f * (float) (i + 1) / (float) (fade + 1));
      channel.swap(trimmed);
    }
    return first;
  }

  /** Channels loaded: 1 for a mono file, 2 for stereo; 0 before the first
   *  successful load. */
  size_t numChannels() const { return channels_.size(); }
  /** Frames per channel at sampleRate(); every channel has the same length.
   *  0 before the first load. */
  size_t length() const { return channels_.empty() ? 0 : channels_[0].size(); }
  /** The rate the response is held at, in Hz: the target rate of the last
   *  load, or the file's own when no target was given. 48000 before any. */
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

  /** The loudest absolute sample over every channel: what Peak makes 1.0
   *  and what trimSilence() measures its threshold from. 0 when empty. */
  float magnitude() const {
    float peak = 0.0f;
    for (const std::vector<float>& channel : channels_)
      for (float v : channel) {
        const float a = v < 0.0f ? -v : v;
        if (a > peak) peak = a;
      }
    return peak;
  }

  /** The same response at another rate, `target` Hz. Resampling keeps the
   *  VALUES, and a response at twice the rate has twice the samples:
   *  convolved, it was twice as loud (+6 dB at every doubling, +12 at
   *  192 kHz). Each sample stands for 1/rate of a second, so the values are
   *  scaled by the ratio of the rates and the response sounds the same at
   *  both. Already at that rate (or a rate of 0 or less): nothing changes.
   *  Allocates. */
  void resampleTo(double target) {
    if (!(target > 0.0) || std::fabs(target - sampleRate_) <= 1e-6 || channels_.empty()) return;
    const double ratio = target / sampleRate_;
    const float keep = (float) (sampleRate_ / target);
    for (std::vector<float>& channel : channels_) {
      const size_t outLength = Resampler::outputLength(channel.size(), ratio);
      if (outLength == 0) continue;
      std::vector<float> out(outLength);
      Resampler::resample(channel.data(), channel.size(), out.data(), out.size(), ratio);
      for (float& v : out) v *= keep;
      channel.swap(out);
    }
    sampleRate_ = target;
  }

private:
  static constexpr double kReferenceRate = 48000.0;

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
      //
      // Unit energy PER CHANNEL, on average, with one gain for all of them so
      // the balance between the sides is kept. Each channel is heard on its
      // own side, so normalising the total put a stereo response's sides at
      // half the energy each -- 3 dB under the same response loaded as mono.
      //
      // As measured at 48 kHz. A response's energy grows with its rate (more
      // samples, each standing for less time), so unit energy in a 96 kHz
      // file is a quieter room than in a 48 kHz one; at the reference rate
      // the two come out equally loud.
      const double perChannel =
          energy() / (double) channels_.size() * (sampleRate_ / kReferenceRate);
      gain = perChannel > 1e-12 ? (float) (1.0 / std::sqrt(perChannel)) : 1.0f;
    }
    for (std::vector<float>& channel : channels_)
      for (float& v : channel) v *= gain;
  }

  std::vector<std::vector<float>> channels_;
  double sampleRate_ = 48000.0;
};

} // namespace sonore
