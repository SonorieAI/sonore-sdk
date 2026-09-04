// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: Windows audio output (WASAPI, shared mode, event driven).
//
// Output only, deliberately: the standalone exists to AUDITION a plugin, and
// shared-mode render is the path that works on every Windows box without
// touching exclusive mode, sample-rate negotiation, or capture privacy prompts.
// An effect gets its input from the standalone's internal source (or a WAV,
// once the user drops one); live microphone input is a later feature, not a
// missing piece of this one.
//
// Shared mode means the DEVICE picks the sample rate (the mix rate) and we
// prepare the DSP at that rate: resampling to fight the mixer would add
// latency and code for zero fidelity gain.
#pragma once

#if defined(_WIN32)

#include <audioclient.h>
#include <mmdeviceapi.h>
// PKEY_Device_FriendlyName lives here, and this header needs the property-key
// machinery declared FIRST or every key expands to an undeclared identifier.
#include <propsys.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
// windows.h defines min and max as MACROS, which turn any later std::min or
// std::max into a syntax error -- and only when this header happens to be
// included FIRST. That made it an include-order landmine rather than a bug:
// gfx/viewport.h compiled for months and then stopped the day plugin_editor.h
// gained one more include, because the new one reached windows.h before
// viewport.h was seen.
//
// WIN32_LEAN_AND_MEAN for the ordinary reason: winsock, RPC, OLE and the shell
// are a large amount of preprocessing nobody here asked for.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstring>
#include <atomic>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>

namespace sonore {
namespace standalone {

class WasapiOutput {
public:
  /** Fill `interleaved` with `frames` frames of `channels` channels. Called on
   *  the audio thread; everything real-time-unsafe is someone else's problem. */
  using RenderFn = std::function<void(float* interleaved, uint32_t frames, uint32_t channels)>;

  ~WasapiOutput() { stop(); }

  double sampleRate() const { return sampleRate_; }
  uint32_t channels() const { return channels_; }
  const std::string& error() const { return error_; }

  /** Negotiate the device and learn its rate. No audio flows yet: the caller
   *  prepares the DSP at sampleRate() BEFORE run(), or the first callback
   *  races an unprepared processor. */
  /** The output devices the machine has, in enumeration order. Index 0 is
   *  always the system DEFAULT, so a caller that never chooses still gets the
   *  device the user's OS settings point at. */
  static std::vector<std::string> listDevices() {
    std::vector<std::string> names;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IMMDeviceEnumerator* enumerator = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), (void**) &enumerator)) &&
        enumerator) {
      names.push_back("System default");
      IMMDeviceCollection* collection = nullptr;
      if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection)) &&
          collection) {
        UINT count = 0;
        collection->GetCount(&count);
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

  /** Which device to open next: 0 (or out of range) means the system default.
   *  Set before open(); changing it afterwards does nothing, which is why the
   *  standalone re-opens rather than pretending it can switch live. */
  void setDeviceIndex(int index) { deviceIndex_ = index; }
  const std::string& deviceName() const { return deviceName_; }

  bool open(RenderFn render) {
    render_ = std::move(render);

    // APARTMENTTHREADED, deliberately: open() runs on the MAIN thread, and the
    // first CoInitializeEx wins the apartment. An MTA here (a first version)
    // meant the webview created moments later got RPC_E_CHANGED_MODE and
    // WebView2, which requires an STA, silently failed, misdiagnosed as a
    // missing runtime. WASAPI's interfaces are free-threaded and do not care;
    // the render thread only touches them, never creates COM objects.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    comInitialised_ = SUCCEEDED(hr);

    IMMDeviceEnumerator* enumerator = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void**) &enumerator);
    if (FAILED(hr) || !enumerator) return fail("no audio device enumerator", hr);

    IMMDevice* device = nullptr;
    if (deviceIndex_ > 0) {
      // Index 1 is the first enumerated endpoint, because 0 means "default".
      IMMDeviceCollection* collection = nullptr;
      if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection)) &&
          collection) {
        UINT count = 0;
        collection->GetCount(&count);
        if ((UINT) (deviceIndex_ - 1) < count) collection->Item((UINT) (deviceIndex_ - 1), &device);
        collection->Release();
      }
    }
    // A chosen device that has since been unplugged falls back to the default
    // rather than refusing to make sound at all.
    if (!device) hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    enumerator->Release();
    if (!device) return fail("no output device", hr);

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
    if (FAILED(hr) || !client_) return fail("the audio client would not activate", hr);

    WAVEFORMATEX* mix = nullptr;
    hr = client_->GetMixFormat(&mix);
    if (FAILED(hr) || !mix) return fail("no mix format", hr);

    // Shared-mode mixes are 32-bit float in practice; anything else is a
    // configuration we refuse rather than misread: writing floats into a
    // 16-bit buffer is a full-scale noise blast through someone's monitors.
    bool isFloat = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
      // The KSDATAFORMAT GUIDs share one fixed suffix; Data1 carries the wave
      // format tag (3 = IEEE float). Comparing that avoids dragging in
      // ksmedia.h and its initguid ceremony for one constant.
      auto* ext = (WAVEFORMATEXTENSIBLE*) mix;
      isFloat = ext->SubFormat.Data1 == 3;
    }
    if (!isFloat || mix->wBitsPerSample != 32) {
      CoTaskMemFree(mix);
      return fail("the shared mix is not 32-bit float", E_FAIL);
    }

    sampleRate_ = (double) mix->nSamplesPerSec;
    channels_ = mix->nChannels;

    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                             0 /* default period */, 0, mix, nullptr);
    CoTaskMemFree(mix);
    if (FAILED(hr)) return fail("the audio client would not initialise", hr);

    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_ || FAILED(client_->SetEventHandle(event_)))
      return fail("no event handle", E_FAIL);

    hr = client_->GetBufferSize(&bufferFrames_);
    if (FAILED(hr)) return fail("no buffer size", hr);

    hr = client_->GetService(__uuidof(IAudioRenderClient), (void**) &renderClient_);
    if (FAILED(hr) || !renderClient_) return fail("no render client", hr);

    return true;
  }

  /** Start the stream. Only after the DSP is prepared. */
  bool run() {
    if (!client_ || !renderClient_) return false;
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { renderLoop(); });
    const HRESULT hr = client_->Start();
    if (FAILED(hr)) {
      stop();
      return fail("the stream would not start", hr);
    }
    return true;
  }

  void stop() {
    if (running_.exchange(false)) {
      if (event_) SetEvent(event_); // wake the loop so it can exit
      if (thread_.joinable()) thread_.join();
    }
    if (client_) client_->Stop();
    if (renderClient_) {
      renderClient_->Release();
      renderClient_ = nullptr;
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

private:
  bool fail(const char* what, HRESULT hr) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s (0x%08lx)", what, (unsigned long) hr);
    error_ = buf;
    return false;
  }

  /** Tell the scheduler this thread is audio.
   *
   *  A plain std::thread runs at normal priority, and on a loaded machine a
   *  10 ms shared-mode period is exactly the kind of deadline a browser tab
   *  or a build in the background makes it miss -- the click that sounds like
   *  a plugin bug and is not. MMCSS ("Pro Audio") is what every audio app on
   *  Windows asks for. avrt.dll is loaded by hand so nothing links against
   *  it: the function is optional, and a machine without it just runs the
   *  loop as before. */
  static void becomeAudioThread(HANDLE* taskOut) {
    *taskOut = nullptr;
    HMODULE avrt = LoadLibraryW(L"avrt.dll");
    if (!avrt) return;
    using SetFn = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);
    auto setFn = (SetFn) (void*) GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW");
    DWORD taskIndex = 0;
    if (setFn) *taskOut = setFn(L"Pro Audio", &taskIndex);
    // The module stays loaded for the thread's lifetime: the revert call needs
    // it, and a plugin process has it resident anyway.
  }
  static void resignAudioThread(HANDLE task) {
    if (!task) return;
    HMODULE avrt = GetModuleHandleW(L"avrt.dll");
    if (!avrt) return;
    using RevertFn = BOOL(WINAPI*)(HANDLE);
    auto revertFn = (RevertFn) (void*) GetProcAddress(avrt, "AvRevertMmThreadCharacteristics");
    if (revertFn) revertFn(task);
  }

  void renderLoop() {
    HANDLE mmcss = nullptr;
    becomeAudioThread(&mmcss);
    while (running_.load(std::memory_order_acquire)) {
      if (WaitForSingleObject(event_, 200) != WAIT_OBJECT_0) continue;
      if (!running_.load(std::memory_order_acquire)) break;

      UINT32 padding = 0;
      if (FAILED(client_->GetCurrentPadding(&padding))) continue;
      const UINT32 frames = bufferFrames_ - padding;
      if (frames == 0) continue;

      BYTE* buffer = nullptr;
      if (FAILED(renderClient_->GetBuffer(frames, &buffer)) || !buffer) continue;
      render_((float*) buffer, frames, channels_);
      renderClient_->ReleaseBuffer(frames, 0);
    }
    resignAudioThread(mmcss);
  }

  RenderFn render_;
  int deviceIndex_ = 0;
  std::string deviceName_;
  IAudioClient* client_ = nullptr;
  IAudioRenderClient* renderClient_ = nullptr;
  HANDLE event_ = nullptr;
  std::thread thread_;
  std::atomic<bool> running_{false};
  double sampleRate_ = 48000.0;
  uint32_t channels_ = 2;
  UINT32 bufferFrames_ = 0;
  bool comInitialised_ = false;
  std::string error_;
};

} // namespace standalone
} // namespace sonore

#endif // _WIN32
