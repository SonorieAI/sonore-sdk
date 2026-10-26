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

namespace detail {

/** Where a device chosen by name is in a device list NOW: at the place it
 *  had, if that place still holds it, else the first place that does, else
 *  -1. Every device list here is numbered by position, and a device that
 *  goes away takes its place with it -- each one after it moves up, and a
 *  number kept from before names its neighbour. */
inline int findListedDevice(const std::vector<std::string>& list, int was,
                            const std::string& name) {
  if (name.empty()) return -1;
  if (was >= 0 && (size_t) was < list.size() && list[(size_t) was] == name) return was;
  for (size_t i = 0; i < list.size(); ++i)
    if (list[i] == name) return (int) i;
  return -1;
}

/** The name at a place in a device list, or nothing. */
inline std::string listedDevice(const std::vector<std::string>& list, int index) {
  return index >= 0 && (size_t) index < list.size() ? list[(size_t) index] : std::string();
}

} // namespace detail

} // namespace standalone
} // namespace sonore

#if defined(_WIN32)
#include "audio_wasapi_input.h"
#elif defined(__ANDROID__)
#include "audio_aaudio.h"
namespace sonore {
namespace standalone {
using PlatformAudioInput = AAudioInput;
} // namespace standalone
} // namespace sonore
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
  bool takeDeviceLost() { return false; } // nothing open, nothing to lose
  void read(float* left, float* right, uint32_t frames) {
    for (uint32_t i = 0; i < frames; ++i) left[i] = right[i] = 0.0f;
  }
  double sampleRate() const { return 0.0; }
  uint64_t droppedFrames() const { return 0; }
  uint64_t starvedFrames() const { return 0; }
  size_t availableFrames() const { return 0; } // nothing is ever captured
  const std::string& deviceName() const { return name_; }
  const std::string& error() const { return error_; }

private:
  std::string error_, name_;
};

using PlatformAudioInput = NullAudioInput;

} // namespace standalone
} // namespace sonore

#endif
