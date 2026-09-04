// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: Linux audio output through JACK, dlopened.
//
// ── Why ALSA is not enough ──────────────────────────────────────────────────
//
// The ALSA backend writes to "default", which on a desktop distribution reaches
// PulseAudio or PipeWire and comes out of the speakers. That is right for
// somebody trying a plugin out.
//
// It is not what a Linux studio runs. Serious Linux audio is JACK: it is how
// applications are connected to each other, how latency is kept in the low
// milliseconds, and how a standalone becomes something you can route into a
// DAW rather than a thing that only talks to the sound card. PipeWire ships a
// libjack that speaks the same API, so this reaches both.
//
// ── The shape is genuinely different from ALSA ──────────────────────────────
//
// ALSA is a stream you WRITE to from a thread you own, in a loop. JACK is a
// graph that CALLS you: the server runs a real-time thread, hands every client
// a buffer at the same instant, and expects an answer before the deadline. So
// there is no render loop here and no thread of ours -- run() only activates
// the client and connects its ports, and the audio arrives on a thread JACK
// created.
//
// That has one consequence worth stating: the process callback is as
// real-time-critical as a plugin's own, more so, because being late does not
// glitch one application -- it xruns the whole graph, every client in it.
//
// ── dlopened, like everything else ──────────────────────────────────────────
//
// Same policy as ALSA and the GTK webview. libjack is not installed on a
// machine that has never run it, and a standalone that failed to LOAD there
// would be worse than one that says "JACK is not on this machine" and falls
// back.
#pragma once

#if defined(__linux__)

#include <dlfcn.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace sonore {
namespace standalone {

class JackOutput {
public:
  using RenderFn = std::function<void(float* interleaved, uint32_t frames, uint32_t channels)>;

  ~JackOutput() { stop(); }

  double sampleRate() const { return sampleRate_; }
  uint32_t channels() const { return channels_; }
  const std::string& error() const { return error_; }

  /**
   * JACK has no device list in the sense the other backends do -- it IS the
   * device, and which hardware it is attached to was decided when the server
   * started. Reporting one entry is the honest answer; reporting the ALSA card
   * list would offer choices this backend cannot honour.
   */
  static std::vector<std::string> listDevices() { return {"JACK"}; }

  void setDeviceIndex(int) {
    // Nothing to select. Kept because the shared standalone code calls it on
    // every backend, and a backend that omitted it would not compile there --
    // which is what backend-surface.mjs checks.
  }

  const std::string& deviceName() const { return deviceName_; }

  /** Whether the library is even present. Not an error: a machine with no JACK
   *  is the ordinary case, and the standalone falls back to ALSA. */
  static bool isAvailable() { return api().ok; }

  /**
   * Register a client and its ports. No audio flows until run().
   *
   * The buffer size and sample rate are the SERVER's, not ours -- JACK decides
   * them for the whole graph and a client that wanted otherwise would be asking
   * every other client to change.
   */
  bool open(RenderFn render) {
    render_ = std::move(render);
    const Api& api_ = api();
    if (!api_.ok) {
      error_ = "libjack is not on this machine";
      return false;
    }

    int status = 0;
    // JackNoStartServer (0x01): do NOT start a server. A standalone that
    // silently launched one would change the machine's audio configuration
    // because somebody opened a plugin.
    client_ = api_.client_open("Sonore", 0x01, &status);
    if (!client_) {
      error_ = "no JACK server is running";
      return false;
    }

    sampleRate_ = (double) api_.get_sample_rate(client_);
    bufferFrames_ = api_.get_buffer_size(client_);

    // JackPortIsOutput (0x2), and the default audio type.
    for (uint32_t i = 0; i < channels_; ++i) {
      char name[16];
      std::snprintf(name, sizeof(name), "out_%u", i + 1);
      ports_[i] = api_.port_register(client_, name, "32 bit float mono audio", 0x2, 0);
      if (!ports_[i]) {
        error_ = "JACK refused a port";
        close();
        return false;
      }
    }

    if (api_.set_process_callback(client_, &processTrampoline, this) != 0) {
      error_ = "JACK refused the process callback";
      close();
      return false;
    }

    deviceName_ = "JACK";
    // Sized once, here, so the callback never allocates. JACK may change the
    // buffer size at runtime; the callback clamps to what it was given rather
    // than reallocating on the audio thread.
    scratch_.assign((size_t) bufferFrames_ * (size_t) channels_ * 4u, 0.0f);
    return true;
  }

  bool run() {
    const Api& api_ = api();
    if (!client_ || !api_.ok) return false;
    if (api_.activate(client_) != 0) {
      error_ = "JACK refused to activate the client";
      return false;
    }
    running_.store(true, std::memory_order_release);
    connectToSystem();
    return true;
  }

  void stop() {
    const Api& api_ = api();
    running_.store(false, std::memory_order_release);
    if (client_ && api_.ok) {
      // Deactivated BEFORE closing: deactivate waits for the current callback
      // to finish, and closing while one is running frees the ports underneath
      // it.
      api_.deactivate(client_);
    }
    close();
  }

private:
  struct Api {
    bool ok = false;
    void* (*client_open)(const char*, int, int*) = nullptr;
    int (*client_close)(void*) = nullptr;
    int (*activate)(void*) = nullptr;
    int (*deactivate)(void*) = nullptr;
    void* (*port_register)(void*, const char*, const char*, unsigned long, unsigned long) = nullptr;
    void* (*port_get_buffer)(void*, uint32_t) = nullptr;
    const char* (*port_name)(void*) = nullptr;
    int (*set_process_callback)(void*, int (*)(uint32_t, void*), void*) = nullptr;
    uint32_t (*get_sample_rate)(void*) = nullptr;
    uint32_t (*get_buffer_size)(void*) = nullptr;
    const char** (*get_ports)(void*, const char*, const char*, unsigned long) = nullptr;
    int (*connect)(void*, const char*, const char*) = nullptr;
    void (*free_)(void*) = nullptr;
  };

  static const Api& api() {
    static Api resolved = [] {
      Api a;
      // .so.0 first, which is what every distribution ships. PipeWire's
      // replacement installs under the same soname, which is the whole point
      // of it -- so this reaches PipeWire without knowing about PipeWire.
      void* lib = dlopen("libjack.so.0", RTLD_LAZY);
      if (!lib) lib = dlopen("libjack.so", RTLD_LAZY);
      if (!lib) return a;

      bool all = true;
      auto bind = [&](const char* name, void* slot) {
        void* sym = dlsym(lib, name);
        if (!sym) all = false;
        std::memcpy(slot, &sym, sizeof(sym));
      };
      bind("jack_client_open", &a.client_open);
      bind("jack_client_close", &a.client_close);
      bind("jack_activate", &a.activate);
      bind("jack_deactivate", &a.deactivate);
      bind("jack_port_register", &a.port_register);
      bind("jack_port_get_buffer", &a.port_get_buffer);
      bind("jack_port_name", &a.port_name);
      bind("jack_set_process_callback", &a.set_process_callback);
      bind("jack_get_sample_rate", &a.get_sample_rate);
      bind("jack_get_buffer_size", &a.get_buffer_size);
      bind("jack_get_ports", &a.get_ports);
      bind("jack_connect", &a.connect);
      bind("jack_free", &a.free_);
      a.ok = all;
      return a;
    }();
    return resolved;
  }

  static int processTrampoline(uint32_t frames, void* userData) {
    return ((JackOutput*) userData)->process(frames);
  }

  /**
   * [JACK's real-time thread] Fill every port.
   *
   * Allocates nothing and takes no lock. Being late here does not glitch one
   * application -- it xruns the whole graph, every client in it -- so this is
   * more real-time-critical than a plugin's own process(), not less.
   */
  int process(uint32_t frames) {
    const Api& api_ = api();
    if (!running_.load(std::memory_order_acquire) || !render_) {
      // Silence rather than whatever was in the buffer. JACK does not clear
      // port buffers between cycles, so a client that returns without writing
      // outputs the previous cycle again -- which is a loud buzz, not silence.
      for (uint32_t c = 0; c < channels_; ++c)
        if (ports_[c])
          if (float* out = (float*) api_.port_get_buffer(ports_[c], frames))
            std::memset(out, 0, (size_t) frames * sizeof(float));
      return 0;
    }

    // Clamped rather than reallocated. JACK can change the buffer size at
    // runtime, and growing a vector on the real-time thread is the one thing
    // this callback must never do -- a short block is a glitch, an allocation
    // is an xrun for everybody.
    const uint32_t usable =
        frames * channels_ <= (uint32_t) scratch_.size() ? frames : (uint32_t) (scratch_.size() / channels_);

    render_(scratch_.data(), usable, channels_);

    // Interleaved from the renderer, one buffer per port for JACK. The
    // de-interleave is the only per-sample work here.
    for (uint32_t c = 0; c < channels_; ++c) {
      float* out = ports_[c] ? (float*) api_.port_get_buffer(ports_[c], frames) : nullptr;
      if (!out) continue;
      for (uint32_t i = 0; i < usable; ++i) out[i] = scratch_[(size_t) i * channels_ + c];
      // Anything past what was rendered is silence, not stale audio.
      if (usable < frames) std::memset(out + usable, 0, (size_t) (frames - usable) * sizeof(float));
    }
    return 0;
  }

  /**
   * Connect to whatever the system's playback ports are.
   *
   * A client that registered ports and connected nothing is silent, and the
   * user has no way to know it is working -- they would have to open a patchbay
   * to find out. Failing to connect is NOT an error: a considered JACK setup
   * routes by hand, and forcing a connection would fight it.
   */
  void connectToSystem() {
    const Api& api_ = api();
    if (!api_.get_ports || !api_.connect || !api_.port_name) return;
    // JackPortIsPhysical (0x4) | JackPortIsInput (0x1): the hardware's inputs,
    // which are what a player connects its outputs to.
    const char** targets = api_.get_ports(client_, nullptr, nullptr, 0x4 | 0x1);
    if (!targets) return;
    for (uint32_t c = 0; c < channels_ && targets[c]; ++c)
      if (ports_[c]) api_.connect(client_, api_.port_name(ports_[c]), targets[c]);
    if (api_.free_) api_.free_((void*) targets);
  }

  void close() {
    const Api& api_ = api();
    if (client_ && api_.ok && api_.client_close) api_.client_close(client_);
    client_ = nullptr;
    for (uint32_t i = 0; i < channels_; ++i) ports_[i] = nullptr;
  }

  static constexpr uint32_t kMaxChannels = 2;

  RenderFn render_;
  void* client_ = nullptr;
  void* ports_[kMaxChannels] = {nullptr, nullptr};
  std::vector<float> scratch_;
  std::string deviceName_;
  std::string error_;
  std::atomic<bool> running_{false};
  double sampleRate_ = 48000.0;
  uint32_t channels_ = 2;
  uint32_t bufferFrames_ = 512;
};

} // namespace standalone
} // namespace sonore

#endif // __linux__
