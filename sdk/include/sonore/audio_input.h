// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: live audio input for the standalone build.
//
// The output backend's own header carried the admission: "live microphone
// input is a later feature, not a missing piece of this one." It was a
// missing piece. An effect standalone that cannot be fed a live signal is a
// guitar pedal with no jack: you can render files through it and you can
// audition its test tone, and you cannot play through it, which is the one
// thing anybody wants to do with a distortion.
//
// The shape is the same on every platform and it is the shape the problem
// has, not a preference:
//
//   Capture runs on its OWN clock, on its own thread, woken by its own
//   device. Render runs on another. They are never the same clock even when
//   they claim the same rate, so the only correct thing between them is a
//   ring the capture side fills and the render side drains.
//
// What this does NOT do is hide the consequence. Two free-running clocks
// drift, and a ring absorbs drift until it doesn't; when it doesn't, either
// the reader finds nothing or the writer finds no room. Both are counted and
// both are reported. A backend that silently papered over them would be a
// backend whose glitches have no name.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "audio_ring.h"

namespace sonore {
namespace standalone {

/** The SPSC ring, which now lives in audio_ring.h so a recorder can use it
 *  without dragging a capture backend in with it. The name here is what every
 *  caller in this file has always written. */
using AudioRing = sonore::AudioRing;

} // namespace standalone
} // namespace sonore

#if defined(_WIN32)
#include "audio_wasapi_input.h"
#elif defined(__linux__)
#include "audio_alsa_input.h"
#elif defined(__APPLE__)
#include "audio_coreaudio_input.h"
#else

namespace sonore {
namespace standalone {

class NullAudioInput {
public:
  static std::vector<std::string> listDevices() { return {}; }
  bool open(int, double) {
    error_ = "no audio input backend for this platform";
    return false;
  }
  bool run() { return false; }
  void stop() {}
  void read(float* left, float* right, uint32_t frames) {
    for (uint32_t i = 0; i < frames; ++i) left[i] = right[i] = 0.0f;
  }
  double sampleRate() const { return 0.0; }
  uint64_t droppedFrames() const { return 0; }
  uint64_t starvedFrames() const { return 0; }
  const std::string& deviceName() const { return name_; }
  const std::string& error() const { return error_; }

private:
  std::string error_, name_;
};

using PlatformAudioInput = NullAudioInput;

} // namespace standalone
} // namespace sonore

#endif
