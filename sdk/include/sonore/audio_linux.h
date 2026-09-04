// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the Linux output the standalone actually uses.
//
// ── Why there are two backends and one type ─────────────────────────────────
//
// PlatformAudio is a single alias per platform, which was right while Linux had
// one backend. It has two now, and they are not alternatives in the sense of
// "pick one at build time": whether JACK is worth using is a fact about the
// machine the plugin is RUNNING on, not the machine it was built on.
//
// So this owns both and presents the one surface the shared standalone code
// calls -- the surface backend-surface.mjs checks every backend for -- and the
// choice becomes a device in the list.
//
// ── Which one is the default ────────────────────────────────────────────────
//
// JACK when a server is already running, ALSA otherwise. Never STARTING a
// server: a standalone that silently launched one would reconfigure the
// machine's audio because somebody opened a plugin, and on a system where JACK
// and PulseAudio are both live that can take the desktop's sound away.
//
// A user who wants the other one picks it from the device list, which is what
// the list is for.
#pragma once

#if defined(__linux__)

#include <string>
#include <vector>

#include "audio_alsa.h"
#include "audio_jack.h"

namespace sonore {
namespace standalone {

class LinuxOutput {
public:
  using RenderFn = std::function<void(float* interleaved, uint32_t frames, uint32_t channels)>;

  /**
   * JACK first when a server is running, then ALSA's PCM names.
   *
   * Ordered by what a machine can actually do rather than alphabetically: on a
   * studio box JACK is entry zero and is what the standalone opens by default;
   * on a laptop it is not in the list at all and nothing pretends otherwise.
   */
  static std::vector<std::string> listDevices() {
    std::vector<std::string> out;
    if (JackOutput::isAvailable()) out.push_back("JACK");
    for (const std::string& name : AlsaOutput::listDevices()) out.push_back("ALSA: " + name);
    return out;
  }

  void setDeviceIndex(int index) {
    index_ = index < 0 ? 0 : index;
    // Which backend that index means depends on whether JACK is in the list,
    // which is why the two lists are built by the same function.
    useJack_ = JackOutput::isAvailable() && index_ == 0;
    if (!useJack_) alsa_.setDeviceIndex(JackOutput::isAvailable() ? index_ - 1 : index_);
  }

  bool open(RenderFn render) {
    if (useJack_) {
      if (jack_.open(render)) return true;
      // A JACK server that went away between listing and opening is a real
      // sequence -- somebody stopped it. Falling back is better than refusing
      // to make a sound, and the reason survives in error().
      error_ = "JACK: " + jack_.error() + " -- falling back to ALSA";
      useJack_ = false;
      alsa_.setDeviceIndex(0);
    }
    if (alsa_.open(render)) return true;
    error_ = alsa_.error();
    return false;
  }

  bool run() { return useJack_ ? jack_.run() : alsa_.run(); }

  void stop() {
    jack_.stop();
    alsa_.stop();
  }

  double sampleRate() const { return useJack_ ? jack_.sampleRate() : alsa_.sampleRate(); }
  uint32_t channels() const { return useJack_ ? jack_.channels() : alsa_.channels(); }

  const std::string& deviceName() const {
    return useJack_ ? jack_.deviceName() : alsa_.deviceName();
  }

  /** The fallback's reason survives here even when the fallback SUCCEEDED --
   *  "it works but not the way you asked" is worth being able to read. */
  const std::string& error() const {
    if (!error_.empty()) return error_;
    return useJack_ ? jack_.error() : alsa_.error();
  }

private:
  JackOutput jack_;
  AlsaOutput alsa_;
  std::string error_;
  int index_ = 0;
  bool useJack_ = JackOutput::isAvailable();
};

} // namespace standalone
} // namespace sonore

#endif // __linux__
