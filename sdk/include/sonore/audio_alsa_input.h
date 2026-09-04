// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: Linux audio capture (ALSA, dlopened).
//
// The mirror of audio_alsa.h, with the same policy for the same reason:
// libasound is resolved at RUNTIME, so the SDK builds with no ALSA dev package
// and a machine with no sound card (CI, WSL) fails gracefully with a named
// reason rather than failing to build.
//
// snd_pcm_set_params is asked to allow resampling, which is what lets a 44.1k
// interface feed a 48k session -- the same job AUTOCONVERTPCM does on Windows,
// and the same reason: the output picked the rate before capture was opened
// and the DSP is already prepared at it.
#pragma once

#if defined(__linux__)

#include <dlfcn.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace sonore {
namespace standalone {

class AlsaInput {
public:
  ~AlsaInput() { stop(); }

  /** The same names the output backend offers. Enumerating the real card list
   *  needs the config API; these are the aliases that exist on every install. */
  static std::vector<std::string> listDevices() {
    void* lib = dlopen("libasound.so.2", RTLD_LAZY);
    if (!lib) return {};
    // Not dlclosed: audio libraries register atexit handlers and unloading
    // them is a classic shutdown crash.
    return {"default", "sysdefault", "hw:0,0", "hw:1,0", "pulse", "pipewire"};
  }

  double sampleRate() const { return sampleRate_; }
  const std::string& deviceName() const { return deviceName_; }
  const std::string& error() const { return error_; }
  uint64_t droppedFrames() const { return ring_.droppedFrames(); }
  uint64_t starvedFrames() const { return ring_.starvedFrames(); }

  bool open(int deviceIndex, double wantedRate) {
    lib_ = dlopen("libasound.so.2", RTLD_LAZY);
    if (!lib_) {
      error_ = "libasound.so.2 is not available";
      return false;
    }
    open_ = (OpenFn) dlsym(lib_, "snd_pcm_open");
    setParams_ = (SetParamsFn) dlsym(lib_, "snd_pcm_set_params");
    readi_ = (ReadiFn) dlsym(lib_, "snd_pcm_readi");
    recover_ = (RecoverFn) dlsym(lib_, "snd_pcm_recover");
    close_ = (CloseFn) dlsym(lib_, "snd_pcm_close");
    start_ = (StartFn) dlsym(lib_, "snd_pcm_start");
    if (!open_ || !setParams_ || !readi_ || !recover_ || !close_) {
      error_ = "libasound is missing expected symbols";
      return false;
    }

    const std::vector<std::string> devices = {"default", "sysdefault", "hw:0,0",
                                              "hw:1,0",  "pulse",      "pipewire"};
    deviceName_ = (deviceIndex > 0 && (size_t) deviceIndex < devices.size())
                      ? devices[(size_t) deviceIndex]
                      : devices[0];
    constexpr int kStreamCapture = 1; // SND_PCM_STREAM_CAPTURE
    if (open_(&pcm_, deviceName_.c_str(), kStreamCapture, 0) < 0 || !pcm_) {
      deviceName_ = devices[0];
      if (open_(&pcm_, deviceName_.c_str(), kStreamCapture, 0) < 0 || !pcm_) {
        error_ = "no ALSA capture device (is a sound server running?)";
        return false;
      }
    }

    sampleRate_ = wantedRate > 0.0 ? wantedRate : 48000.0;
    constexpr int kFormatFloatLe = 14;      // SND_PCM_FORMAT_FLOAT_LE
    constexpr int kAccessRwInterleaved = 3; // SND_PCM_ACCESS_RW_INTERLEAVED
    if (setParams_(pcm_, kFormatFloatLe, kAccessRwInterleaved, 2, (unsigned) sampleRate_,
                   1 /* allow resample */, 40000 /* usec */) < 0) {
      error_ = "the device refused float stereo capture";
      close_(pcm_);
      pcm_ = nullptr;
      return false;
    }

    ring_.reset(kBlock * 8);
    return true;
  }

  bool run() {
    if (!pcm_) return false;
    running_.store(true, std::memory_order_release);
    if (start_) start_(pcm_);
    thread_ = std::thread([this] { captureLoop(); });
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
  }

  void read(float* left, float* right, uint32_t frames) { ring_.read(left, right, frames); }

private:
  static constexpr uint32_t kBlock = 256;

  using OpenFn = int (*)(void**, const char*, int, int);
  using SetParamsFn = int (*)(void*, int, int, unsigned, unsigned, int, unsigned);
  using ReadiFn = long (*)(void*, void*, unsigned long);
  using RecoverFn = int (*)(void*, int, int);
  using CloseFn = int (*)(void*);
  using StartFn = int (*)(void*);

  void captureLoop() {
    std::vector<float> buffer(kBlock * 2);
    while (running_.load(std::memory_order_acquire)) {
      const long got = readi_(pcm_, buffer.data(), kBlock);
      if (got < 0) {
        // An overrun is normal under load and recoverable; anything the
        // recovery does not fix ends the loop rather than spinning on an
        // error forever.
        if (!recover_ || recover_(pcm_, (int) got, 1) < 0) break;
        continue;
      }
      if (got > 0) ring_.write(buffer.data(), (size_t) got);
    }
  }

  void* lib_ = nullptr;
  void* pcm_ = nullptr;
  OpenFn open_ = nullptr;
  SetParamsFn setParams_ = nullptr;
  ReadiFn readi_ = nullptr;
  RecoverFn recover_ = nullptr;
  CloseFn close_ = nullptr;
  StartFn start_ = nullptr;

  std::thread thread_;
  std::atomic<bool> running_{false};
  AudioRing ring_;
  double sampleRate_ = 0.0;
  std::string deviceName_, error_;
};

using PlatformAudioInput = AlsaInput;

} // namespace standalone
} // namespace sonore

#endif // __linux__
