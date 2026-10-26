// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: audio in and out on Android, through AAudio.
//
// AAudio is the NDK's own low-latency audio API (Android 8, API 26; this SDK's
// Android floor is 9). It is what Google's Oboe library calls first, and on a
// device with an MMAP path it hands the app the hardware's buffer directly.
// The two classes here are the standalone's PlatformAudio and
// PlatformAudioInput on Android, the same surface WASAPI, ALSA and Core Audio
// offer on the desktop (scripts/backend-surface.mjs checks it).
//
// What Android does differently:
//
//  - There is no device list to choose from. AAudio opens the route the system
//    picked -- the speaker, headphones, a USB interface when one is plugged in
//    -- and follows the user's choice in the system's own UI. listDevices()
//    names that one route.
//
//  - The rate is the device's. A stream opened without a rate runs at the
//    hardware's own (48 kHz on nearly every phone), with no resampler in the
//    path, which is the low-latency path; the DSP is prepared at whatever it
//    is.
//
//  - Low latency is asked for, and EXCLUSIVE sharing: the MMAP path where the
//    device has one. A device without it gives a shared stream instead, which
//    still works. The buffer is two bursts deep, AAudio's own advice for a
//    stream with no glitches to absorb.
//
//  - A stream can be DISCONNECTED -- headphones pulled, a USB interface
//    unplugged, the route changed under it. The error callback says so; the
//    stream may not be closed from inside it, so it is recorded and
//    takeDeviceLost() reports it to the thread that reopens.
//
//  - Capture needs the RECORD_AUDIO permission, granted by the user at run
//    time. Without it the input stream does not open, and error() says why.
#pragma once

#if defined(__ANDROID__)

#include <aaudio/AAudio.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "audio_ring.h"

namespace sonore {
namespace standalone {

namespace aaudiodetail {

/** A builder with what every stream here asks for: float samples, low
 *  latency, the hardware to itself if it can have it. */
inline AAudioStreamBuilder* builder(aaudio_direction_t direction, int32_t channels, int32_t rate) {
  AAudioStreamBuilder* b = nullptr;
  if (AAudio_createStreamBuilder(&b) != AAUDIO_OK || !b) return nullptr;
  AAudioStreamBuilder_setDirection(b, direction);
  AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
  AAudioStreamBuilder_setChannelCount(b, channels);
  if (rate > 0) AAudioStreamBuilder_setSampleRate(b, rate);
  AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
  AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_EXCLUSIVE);
  if (direction == AAUDIO_DIRECTION_OUTPUT) {
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_MEDIA);
    AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_MUSIC);
  } else {
    // What a microphone hears, without the voice-call processing (noise
    // suppression, gain control) a VOICE preset would put on it.
    AAudioStreamBuilder_setInputPreset(b, AAUDIO_INPUT_PRESET_UNPROCESSED);
  }
  return b;
}

inline std::string describe(const char* what, aaudio_result_t r) {
  const char* text = AAudio_convertResultToText(r);
  return std::string(what) + " (" + (text ? text : "AAudio error") + ")";
}

} // namespace aaudiodetail

/**
 * The system output, through an AAudio stream whose data callback runs the
 * standalone's render function on AAudio's own high-priority thread.
 */
class AAudioOutput {
public:
  using RenderFn = std::function<void(float* interleaved, uint32_t frames, uint32_t channels)>;

  ~AAudioOutput() { stop(); }

  /** The one route Android offers: whatever the system is playing to. */
  static std::vector<std::string> listDevices() { return {"System output"}; }
  /** Nothing to choose between (see the file comment). */
  void setDeviceIndex(int) {}

  /** Open a stereo float stream at the device's own rate. The stream is not
   *  started: run() does that, once the DSP is prepared at sampleRate(). */
  bool open(RenderFn render) {
    stop();
    error_.clear();
    render_ = std::move(render);
    lost_.store(false, std::memory_order_relaxed);
    AAudioStreamBuilder* b = aaudiodetail::builder(AAUDIO_DIRECTION_OUTPUT, 2, 0);
    if (!b) {
      error_ = "AAudio is not available";
      return false;
    }
    AAudioStreamBuilder_setDataCallback(b, &AAudioOutput::dataCallback, this);
    AAudioStreamBuilder_setErrorCallback(b, &AAudioOutput::errorCallback, this);
    const aaudio_result_t r = AAudioStreamBuilder_openStream(b, &stream_);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK || !stream_) {
      stream_ = nullptr;
      error_ = aaudiodetail::describe("the output stream did not open", r);
      return false;
    }
    sampleRate_ = (double) AAudioStream_getSampleRate(stream_);
    channels_ = (uint32_t) AAudioStream_getChannelCount(stream_);
    const int32_t burst = AAudioStream_getFramesPerBurst(stream_);
    if (burst > 0) AAudioStream_setBufferSizeInFrames(stream_, burst * 2);
    burst_ = burst > 0 ? (uint32_t) burst : 0;
    exclusive_ = AAudioStream_getSharingMode(stream_) == AAUDIO_SHARING_MODE_EXCLUSIVE;
    name_ = exclusive_ ? "System output (low latency, exclusive)" : "System output (low latency)";
    return true;
  }

  /** Start the stream opened by open(). */
  bool run() {
    if (!stream_) return false;
    const aaudio_result_t r = AAudioStream_requestStart(stream_);
    if (r != AAUDIO_OK) {
      error_ = aaudiodetail::describe("the output stream did not start", r);
      return false;
    }
    return true;
  }

  /** True once since the route went away under a running stream. */
  bool takeDeviceLost() { return lost_.exchange(false, std::memory_order_acq_rel); }

  void stop() {
    if (!stream_) return;
    AAudioStream_requestStop(stream_);
    AAudioStream_close(stream_);
    stream_ = nullptr;
  }

  double sampleRate() const { return sampleRate_; }
  uint32_t channels() const { return channels_; }
  /** The frames AAudio moves at once: what one callback usually asks for. */
  uint32_t bufferFrames() const { return burst_; }
  /** Underruns the stream has counted since it opened. */
  int32_t xruns() const { return stream_ ? AAudioStream_getXRunCount(stream_) : 0; }
  const std::string& deviceName() const { return name_; }
  const std::string& error() const { return error_; }

private:
  static aaudio_data_callback_result_t dataCallback(AAudioStream*, void* user, void* audioData,
                                                    int32_t numFrames) {
    auto* self = static_cast<AAudioOutput*>(user);
    auto* out = static_cast<float*>(audioData);
    if (self->render_ && numFrames > 0) self->render_(out, (uint32_t) numFrames, self->channels_);
    else if (numFrames > 0) std::memset(out, 0, sizeof(float) * (size_t) numFrames * self->channels_);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
  }

  /** On AAudio's own thread, where the stream may not be closed: recorded. */
  static void errorCallback(AAudioStream*, void* user, aaudio_result_t error) {
    if (error == AAUDIO_ERROR_DISCONNECTED)
      static_cast<AAudioOutput*>(user)->lost_.store(true, std::memory_order_release);
  }

  AAudioStream* stream_ = nullptr;
  RenderFn render_;
  double sampleRate_ = 0.0;
  uint32_t channels_ = 2;
  uint32_t burst_ = 0;
  bool exclusive_ = false;
  std::atomic<bool> lost_{false};
  std::string name_ = "System output";
  std::string error_;
};

/**
 * The system input -- the microphone, or a plugged-in interface -- through an
 * AAudio capture stream whose callback fills a ring the render reads from.
 */
class AAudioInput {
public:
  ~AAudioInput() { stop(); }

  static std::vector<std::string> listDevices() { return {"System input"}; }

  double sampleRate() const { return sampleRate_; }
  const std::string& deviceName() const { return name_; }
  const std::string& error() const { return error_; }
  uint64_t droppedFrames() const { return ring_.droppedFrames(); }
  uint64_t starvedFrames() const { return ring_.starvedFrames(); }
  /** Frames read() would deliver now. [reader] */
  size_t availableFrames() const { return ring_.readable(); }
  bool takeDeviceLost() { return lost_.exchange(false, std::memory_order_acq_rel); }

  /** Open stereo capture at `wantedRate`, the rate the output settled on. */
  bool open(int /*deviceIndex*/, double wantedRate) {
    stop();
    error_.clear();
    lost_.store(false, std::memory_order_relaxed);
    AAudioStreamBuilder* b =
        aaudiodetail::builder(AAUDIO_DIRECTION_INPUT, 2, wantedRate > 0.0 ? (int32_t) wantedRate : 0);
    if (!b) {
      error_ = "AAudio is not available";
      return false;
    }
    AAudioStreamBuilder_setDataCallback(b, &AAudioInput::dataCallback, this);
    AAudioStreamBuilder_setErrorCallback(b, &AAudioInput::errorCallback, this);
    const aaudio_result_t r = AAudioStreamBuilder_openStream(b, &stream_);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK || !stream_) {
      stream_ = nullptr;
      // The commonest reason by far, said in words a person can act on.
      error_ = aaudiodetail::describe("the input stream did not open -- has the app been allowed to "
                                      "record audio?",
                                      r);
      return false;
    }
    sampleRate_ = (double) AAudioStream_getSampleRate(stream_);
    channels_ = (uint32_t) AAudioStream_getChannelCount(stream_);
    const int32_t burst = AAudioStream_getFramesPerBurst(stream_);
    const size_t block = burst > 0 ? (size_t) burst : 256;
    ring_.reset(block * 8);
    ring_.setPrime(block * 2);
    stereo_.assign(block * 2 * 4, 0.0f);
    return true;
  }

  bool run() {
    if (!stream_) return false;
    const aaudio_result_t r = AAudioStream_requestStart(stream_);
    if (r != AAUDIO_OK) {
      error_ = aaudiodetail::describe("the input stream did not start", r);
      return false;
    }
    return true;
  }

  void stop() {
    if (!stream_) return;
    AAudioStream_requestStop(stream_);
    AAudioStream_close(stream_);
    stream_ = nullptr;
  }

  void read(float* left, float* right, uint32_t frames) { ring_.read(left, right, frames); }

private:
  static aaudio_data_callback_result_t dataCallback(AAudioStream*, void* user, void* audioData,
                                                    int32_t numFrames) {
    auto* self = static_cast<AAudioInput*>(user);
    const auto* in = static_cast<const float*>(audioData);
    if (numFrames <= 0) return AAUDIO_CALLBACK_RESULT_CONTINUE;
    if (self->channels_ == 2) {
      self->ring_.write(in, (size_t) numFrames);
    } else {
      // A mono microphone, written as stereo a block at a time, through the
      // buffer open() sized: no allocation on AAudio's thread.
      const size_t room = self->stereo_.size() / 2;
      size_t done = 0;
      while (done < (size_t) numFrames) {
        const size_t n = (size_t) numFrames - done < room ? (size_t) numFrames - done : room;
        for (size_t i = 0; i < n; ++i) {
          const float v = in[(done + i) * self->channels_];
          self->stereo_[2 * i] = v;
          self->stereo_[2 * i + 1] = v;
        }
        self->ring_.write(self->stereo_.data(), n);
        done += n;
      }
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
  }

  static void errorCallback(AAudioStream*, void* user, aaudio_result_t error) {
    if (error == AAUDIO_ERROR_DISCONNECTED)
      static_cast<AAudioInput*>(user)->lost_.store(true, std::memory_order_release);
  }

  AAudioStream* stream_ = nullptr;
  AudioRing ring_;
  std::vector<float> stereo_;
  double sampleRate_ = 0.0;
  uint32_t channels_ = 2;
  std::atomic<bool> lost_{false};
  std::string name_ = "System input";
  std::string error_;
};

} // namespace standalone
} // namespace sonore

#endif // __ANDROID__
