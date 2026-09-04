// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: Linux audio output (ALSA, dlopened).
//
// Same policy as the GTK webview: libasound is resolved at RUNTIME, so the SDK
// builds with no ALSA dev package and a machine without sound (CI, WSL) fails
// gracefully with a named reason instead of failing to build or crashing.
//
// The stream is plain blocking writei on "default" at the device's preference:
// PulseAudio/PipeWire sit behind that PCM on every desktop distro, so this is
// the path that actually reaches speakers.
#pragma once

#if defined(__linux__)

#include <dlfcn.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace sonore {
namespace standalone {

class AlsaOutput {
public:
  using RenderFn = std::function<void(float* interleaved, uint32_t frames, uint32_t channels)>;

  ~AlsaOutput() { stop(); }

  double sampleRate() const { return sampleRate_; }
  uint32_t channels() const { return channels_; }
  const std::string& error() const { return error_; }

  /** Negotiate the device; no audio flows until run(). */
  /** ALSA's PCM names, as a user would type them. Enumerating the full card
   *  list needs the config API; these are the aliases that actually exist on
   *  every install and cover what a standalone needs. */
  static std::vector<std::string> listDevices() {
    return {"default", "sysdefault", "hw:0,0", "hw:1,0", "pulse", "pipewire"};
  }

  void setDeviceIndex(int index) { deviceIndex_ = index; }
  const std::string& deviceName() const { return deviceName_; }

  bool open(RenderFn render) {
    render_ = std::move(render);

    // Versioned soname only: the unversioned .so lives in dev packages.
    lib_ = dlopen("libasound.so.2", RTLD_LAZY);
    if (!lib_) {
      error_ = "libasound.so.2 is not available";
      return false;
    }

    open_ = (OpenFn) dlsym(lib_, "snd_pcm_open");
    setParams_ = (SetParamsFn) dlsym(lib_, "snd_pcm_set_params");
    writei_ = (WriteiFn) dlsym(lib_, "snd_pcm_writei");
    recover_ = (RecoverFn) dlsym(lib_, "snd_pcm_recover");
    close_ = (CloseFn) dlsym(lib_, "snd_pcm_close");
    if (!open_ || !setParams_ || !writei_ || !recover_ || !close_) {
      error_ = "libasound is missing expected symbols";
      return false;
    }

    const std::vector<std::string> devices = listDevices();
    deviceName_ = (deviceIndex_ > 0 && (size_t) deviceIndex_ < devices.size())
                      ? devices[(size_t) deviceIndex_]
                      : devices[0];
    if (open_(&pcm_, deviceName_.c_str(), 0 /* playback */, 0) < 0 || !pcm_) {
      // A named device that is not there falls back to "default" rather than
      // leaving the app silent.
      deviceName_ = devices[0];
      if (open_(&pcm_, deviceName_.c_str(), 0, 0) < 0 || !pcm_) {
        error_ = "no ALSA playback device (is a sound server running?)";
        return false;
      }
    }

    // FLOAT_LE interleaved stereo at 48 kHz with ~40 ms of buffer. 48 kHz is a
    // request, not an assumption: set_params negotiates, and the DSP is
    // prepared at whatever the device actually granted.
    constexpr int kFormatFloatLe = 14; // SND_PCM_FORMAT_FLOAT_LE
    constexpr int kAccessRwInterleaved = 3; // SND_PCM_ACCESS_RW_INTERLEAVED
    if (setParams_(pcm_, kFormatFloatLe, kAccessRwInterleaved, channels_, (unsigned) sampleRate_,
                   1 /* allow resample */, 40000 /* usec */) < 0) {
      error_ = "the device refused float stereo playback";
      close_(pcm_);
      pcm_ = nullptr;
      return false;
    }

    return true;
  }

  bool run() {
    if (!pcm_) return false;
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { renderLoop(); });
    return true;
  }

  void stop() {
    if (running_.exchange(false)) {
      if (thread_.joinable()) thread_.join();
    }
    if (pcm_ && close_) {
      close_(pcm_);
      pcm_ = nullptr;
    }
    // The library handle is deliberately kept: dlclosing audio libraries that
    // register atexit handlers is a classic shutdown crash.
  }

private:
  void renderLoop() {
    constexpr uint32_t kBlock = 256;
    std::vector<float> buffer(kBlock * channels_);
    while (running_.load(std::memory_order_acquire)) {
      render_(buffer.data(), kBlock, channels_);
      // A short write is legal (the device took what fitted); the rest is
      // written in a second call rather than dropped, which was a click per
      // period on a device whose period is not a multiple of the block.
      uint32_t done = 0;
      while (done < kBlock && running_.load(std::memory_order_acquire)) {
        const long wrote =
            writei_(pcm_, buffer.data() + (size_t) done * channels_, kBlock - done);
        if (wrote >= 0) {
          done += (uint32_t) wrote;
          continue;
        }
        // An underrun or a suspend is recovered (snd_pcm_prepare behind the
        // call) and the block retried. A device that cannot be recovered --
        // unplugged, server gone -- used to make this loop spin at full CPU,
        // rendering blocks for a device that would never take another frame;
        // it now waits, and keeps waiting quietly until stop().
        if (recover_(pcm_, (int) wrote, 1) < 0) {
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
          break;
        }
      }
    }
  }

  using OpenFn = int (*)(void**, const char*, int, int);
  using SetParamsFn = int (*)(void*, int, int, unsigned, unsigned, int, unsigned);
  using WriteiFn = long (*)(void*, const void*, unsigned long);
  using RecoverFn = int (*)(void*, int, int);
  using CloseFn = int (*)(void*);

  RenderFn render_;
  int deviceIndex_ = 0;
  std::string deviceName_;
  void* lib_ = nullptr;
  void* pcm_ = nullptr;
  OpenFn open_ = nullptr;
  SetParamsFn setParams_ = nullptr;
  WriteiFn writei_ = nullptr;
  RecoverFn recover_ = nullptr;
  CloseFn close_ = nullptr;
  std::thread thread_;
  std::atomic<bool> running_{false};
  double sampleRate_ = 48000.0;
  uint32_t channels_ = 2;
  std::string error_;
};

} // namespace standalone
} // namespace sonore

#endif // __linux__
