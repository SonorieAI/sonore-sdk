// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: Windows audio capture (WASAPI, shared mode, event driven).
//
// The mirror of audio_wasapi.h, and deliberately a separate object rather
// than a second mode of that one: capture has its own device, its own client,
// its own event and its own thread, and the only thing the two share is the
// ring between them. Folding them together would mean one object with two of
// everything and a flag deciding which half is real.
//
// Shared mode again, for the same reason: it is the path that works on every
// Windows box without exclusive mode or format negotiation. Where capture
// differs is that the DEVICE's rate is not ours to accept: the output picked
// the rate first and the DSP is prepared at it, so this asks Windows to
// convert, and only refuses if Windows will not.
#pragma once

#if defined(_WIN32)

// WIN32_LEAN_AND_MEAN, or windows.h drags in the ORIGINAL winsock.h and
// redefines every type winsock2.h already declared. This header shares a
// build with osc.h, which needs winsock2, and nothing here wants winsock at
// all -- the cost of leaving it out was a wall of 'sockaddr: struct type
// redefinition' from a file that has never heard of sockets.
// NOMINMAX for the same reason and in the same breath: windows.h defines min
// and max as MACROS, and any translation unit that has already said
// numeric_limits<T>::max() finds the '(' on the right of '::' illegal.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <audioclient.h>
#include <mmdeviceapi.h>
// PKEY_Device_FriendlyName lives here, and this header needs the property-key
// machinery declared FIRST or every key expands to an undeclared identifier.
#include <propsys.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <windows.h>

// rpcndr.h, which the COM headers above pull in, contains `#define small char`
// -- a MIDL artefact from the 1990s that has been breaking other people's code
// ever since. It is not a type, a keyword or anything a C++ program can defend
// against; a variable named `small` three thousand lines away in a file that
// has never heard of COM simply stops compiling. Taken back here, because the
// alternative is every consumer of this header renaming their variables.
#ifdef small
#undef small
#endif

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace sonore {
namespace standalone {

class WasapiInput {
public:
  ~WasapiInput() { stop(); }

  /** The capture endpoints this machine has. Index 0 is always the system
   *  DEFAULT, matching the output backend, so the two indices mean the same
   *  kind of thing and a user can learn one rule. */
  static std::vector<std::string> listDevices() {
    std::vector<std::string> names;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IMMDeviceEnumerator* enumerator = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), (void**) &enumerator)) &&
        enumerator) {
      IMMDeviceCollection* collection = nullptr;
      if (SUCCEEDED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection)) &&
          collection) {
        UINT count = 0;
        collection->GetCount(&count);
        // Only claim a default when there is something to be the default OF.
        // Listing "System default" on a machine with no microphone is an
        // index that names nothing.
        if (count > 0) names.push_back("System default");
        for (UINT i = 0; i < count; ++i) {
          IMMDevice* dev = nullptr;
          if (FAILED(collection->Item(i, &dev)) || !dev) continue;
          IPropertyStore* props = nullptr;
          if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && props) {
            PROPVARIANT value;
            PropVariantInit(&value);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &value)) &&
                value.vt == VT_LPWSTR && value.pwszVal) {
              char utf8[256] = {0};
              WideCharToMultiByte(CP_UTF8, 0, value.pwszVal, -1, utf8, sizeof(utf8) - 1, nullptr,
                                  nullptr);
              names.push_back(utf8);
            }
            PropVariantClear(&value);
            props->Release();
          }
          dev->Release();
        }
        collection->Release();
      }
      enumerator->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return names;
  }

  double sampleRate() const { return sampleRate_; }
  const std::string& deviceName() const { return deviceName_; }
  const std::string& error() const { return error_; }
  uint64_t droppedFrames() const { return ring_.droppedFrames(); }
  uint64_t starvedFrames() const { return ring_.starvedFrames(); }

  /** Open a capture endpoint and ask it for `wantedRate`.
   *
   *  The rate is not negotiable from this side: the OUTPUT device already
   *  picked it and the DSP is already prepared at it. A capture device
   *  running at a different rate is the normal case, not an error -- a 44.1k
   *  interface feeding a 48k mixer is most people's setup -- so this asks
   *  Windows to convert, which it does properly and for free. */
  bool open(int deviceIndex, double wantedRate) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    comInitialised_ = SUCCEEDED(init);

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**) &enumerator);
    if (FAILED(hr) || !enumerator) return fail("no audio device enumerator", hr);

    IMMDevice* device = nullptr;
    if (deviceIndex > 0) {
      IMMDeviceCollection* collection = nullptr;
      if (SUCCEEDED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection)) &&
          collection) {
        UINT count = 0;
        collection->GetCount(&count);
        if ((UINT) (deviceIndex - 1) < count) collection->Item((UINT) (deviceIndex - 1), &device);
        collection->Release();
      }
    }
    if (!device) hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
    enumerator->Release();
    if (!device) return fail("no input device", hr);

    {
      IPropertyStore* props = nullptr;
      if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) && props) {
        PROPVARIANT value;
        PropVariantInit(&value);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &value)) &&
            value.vt == VT_LPWSTR && value.pwszVal) {
          char utf8[256] = {0};
          WideCharToMultiByte(CP_UTF8, 0, value.pwszVal, -1, utf8, sizeof(utf8) - 1, nullptr,
                              nullptr);
          deviceName_ = utf8;
        }
        PropVariantClear(&value);
        props->Release();
      }
    }

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**) &client_);
    device->Release();
    if (FAILED(hr) || !client_) return fail("the capture client would not activate", hr);

    WAVEFORMATEX* mix = nullptr;
    hr = client_->GetMixFormat(&mix);
    if (FAILED(hr) || !mix) return fail("no capture mix format", hr);
    captureChannels_ = mix->nChannels;

    // Ask for the rate the DSP is prepared at, in float, keeping the device's
    // own channel count. AUTOCONVERTPCM is what makes Windows do the
    // conversion; without it a 44.1k microphone simply refuses to open on a
    // 48k session and the user is told to go and change a control panel.
    WAVEFORMATEX wanted{};
    wanted.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    wanted.nChannels = mix->nChannels;
    wanted.nSamplesPerSec = (DWORD) (wantedRate > 0.0 ? wantedRate : mix->nSamplesPerSec);
    wanted.wBitsPerSample = 32;
    wanted.nBlockAlign = (WORD) (wanted.nChannels * wanted.wBitsPerSample / 8);
    wanted.nAvgBytesPerSec = wanted.nSamplesPerSec * wanted.nBlockAlign;
    wanted.cbSize = 0;

    const DWORD kConvert =
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | 0x80000000 /* AUTOCONVERTPCM */ |
        0x08000000 /* SRC_DEFAULT_QUALITY */;
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, kConvert, kBufferDuration, 0, &wanted,
                             nullptr);
    if (SUCCEEDED(hr)) {
      sampleRate_ = (double) wanted.nSamplesPerSec;
      captureChannels_ = wanted.nChannels;
    } else {
      // The conversion path is not available everywhere. Fall back to the
      // device's own mix format and report the mismatch rather than
      // resampling by hand, badly, on the audio thread.
      client_->Release();
      client_ = nullptr;
      IMMDeviceEnumerator* again = nullptr;
      if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**) &again)) ||
          !again) {
        CoTaskMemFree(mix);
        return fail("no audio device enumerator", hr);
      }
      IMMDevice* dev2 = nullptr;
      again->GetDefaultAudioEndpoint(eCapture, eConsole, &dev2);
      again->Release();
      if (!dev2) {
        CoTaskMemFree(mix);
        return fail("no input device", hr);
      }
      const HRESULT act = dev2->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                         (void**) &client_);
      dev2->Release();
      if (FAILED(act) || !client_) {
        CoTaskMemFree(mix);
        return fail("the capture client would not activate", act);
      }
      hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                               kBufferDuration, 0, mix, nullptr);
      if (FAILED(hr)) {
        CoTaskMemFree(mix);
        return fail("the capture client would not initialise", hr);
      }
      sampleRate_ = (double) mix->nSamplesPerSec;
      captureChannels_ = mix->nChannels;
      if (wantedRate > 0.0 && std::abs(sampleRate_ - wantedRate) > 1.0) {
        char buffer[200];
        std::snprintf(buffer, sizeof(buffer),
                      "the input runs at %.0f Hz and the output at %.0f Hz, and this build "
                      "could not get Windows to convert",
                      sampleRate_, wantedRate);
        error_ = buffer;
        CoTaskMemFree(mix);
        return false;
      }
    }
    CoTaskMemFree(mix);

    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_ || FAILED(client_->SetEventHandle(event_))) return fail("no event handle", E_FAIL);

    UINT32 bufferFrames = 0;
    hr = client_->GetBufferSize(&bufferFrames);
    if (FAILED(hr)) return fail("no capture buffer size", hr);

    hr = client_->GetService(__uuidof(IAudioCaptureClient), (void**) &captureClient_);
    if (FAILED(hr) || !captureClient_) return fail("no capture client", hr);

    // Four device buffers of slack. One is not enough for two free-running
    // clocks; a second of it would just turn drift into latency nobody asked
    // for.
    ring_.reset((size_t) bufferFrames * 4);
    scratch_.assign((size_t) bufferFrames * 2, 0.0f);
    return true;
  }

  bool run() {
    if (!client_ || !captureClient_) return false;
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { captureLoop(); });
    const HRESULT hr = client_->Start();
    if (FAILED(hr)) {
      stop();
      return fail("the capture stream would not start", hr);
    }
    return true;
  }

  void stop() {
    if (running_.exchange(false)) {
      if (event_) SetEvent(event_);
      if (thread_.joinable()) thread_.join();
    }
    if (client_) client_->Stop();
    if (captureClient_) {
      captureClient_->Release();
      captureClient_ = nullptr;
    }
    if (client_) {
      client_->Release();
      client_ = nullptr;
    }
    if (event_) {
      CloseHandle(event_);
      event_ = nullptr;
    }
    if (comInitialised_) {
      CoUninitialize();
      comInitialised_ = false;
    }
  }

  /** Called from the RENDER thread. Never blocks and never allocates: a short
   *  read is silence, and the count of how much says so afterwards. */
  void read(float* left, float* right, uint32_t frames) { ring_.read(left, right, frames); }

private:
  // 40 ms. Long enough that a scheduling hiccup on either side is absorbed,
  // short enough that a player does not hear their own hands late.
  static constexpr REFERENCE_TIME kBufferDuration = 400000; // 100-ns units

  bool fail(const char* what, HRESULT hr) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s (0x%08lx)", what, (unsigned long) hr);
    error_ = buf;
    return false;
  }

  void captureLoop() {
    // MTA here, unlike open(): this thread creates no COM objects and only
    // touches free-threaded WASAPI interfaces, and joining the main thread's
    // apartment would mean this thread's lifetime is tangled with the
    // webview's.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (running_.load(std::memory_order_acquire)) {
      if (WaitForSingleObject(event_, 200) != WAIT_OBJECT_0) continue;
      if (!running_.load(std::memory_order_acquire)) break;

      UINT32 packet = 0;
      while (SUCCEEDED(captureClient_->GetNextPacketSize(&packet)) && packet > 0) {
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        if (FAILED(captureClient_->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;

        if (frames > 0) {
          if (scratch_.size() < (size_t) frames * 2) scratch_.assign((size_t) frames * 2, 0.0f);
          // AUDCLNT_BUFFERFLAGS_SILENT means the pointer is not worth reading
          // and the frames are still real time that must not go missing --
          // dropping them would make the stream shorter than the clock.
          if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            std::memset(scratch_.data(), 0, (size_t) frames * 2 * sizeof(float));
          } else {
            const float* in = (const float*) data;
            const uint32_t ch = captureChannels_ ? captureChannels_ : 1;
            for (UINT32 i = 0; i < frames; ++i) {
              scratch_[(size_t) i * 2] = in[(size_t) i * ch];
              scratch_[(size_t) i * 2 + 1] = ch > 1 ? in[(size_t) i * ch + 1] : in[(size_t) i * ch];
            }
          }
          ring_.write(scratch_.data(), frames);
        }
        captureClient_->ReleaseBuffer(frames);
      }
    }
    CoUninitialize();
  }

  IAudioClient* client_ = nullptr;
  IAudioCaptureClient* captureClient_ = nullptr;
  HANDLE event_ = nullptr;
  std::thread thread_;
  std::atomic<bool> running_{false};
  bool comInitialised_ = false;

  AudioRing ring_;
  std::vector<float> scratch_;
  uint32_t captureChannels_ = 2;
  double sampleRate_ = 0.0;
  std::string deviceName_, error_;
};

using PlatformAudioInput = WasapiInput;

} // namespace standalone
} // namespace sonore

#endif // _WIN32
