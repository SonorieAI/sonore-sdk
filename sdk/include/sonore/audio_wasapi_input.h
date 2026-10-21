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
// differs is that the DEVICE's rate is not ours to accept (the output picked
// the rate first and the DSP is prepared at it), so this asks Windows to
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
#include "audio_ring.h" // AudioRing, the capture side's hand-off
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

/**
 * One capture device in WASAPI shared mode, handed to the render side through
 * an AudioRing: the standalone's live input on Windows. See the file comment
 * for why it is its own object rather than a mode of WasapiOutput.
 *
 * open(index, rate) runs on the MAIN thread after the output has settled the
 * rate, since the DSP is already prepared at it; index 0 is the system
 * default. Windows is asked to convert to that rate, and when it will not and
 * the rates differ -- or the device's own mix is not 32-bit float -- open()
 * fails and error() says so. run() starts a capture
 * thread of its own that writes stereo float into the ring: a mono device
 * feeds both sides, and channels past the second are not used. The render
 * thread takes it with read(), which never blocks or allocates and pads a
 * short read with silence; droppedFrames() and starvedFrames() count what did
 * not fit and what was not there. stop() and the destructor run on the
 * thread that called open(). A device that goes away sets takeDeviceLost().
 */
class WasapiInput {
public:
  /** stop(), on the thread that called open(). */
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
          // Every endpoint gets an entry, named or not, as the output list
          // does: open() counts every active endpoint, and a list that
          // skipped one whose name could not be read numbered every
          // microphone after it one short -- picking one opened its
          // neighbour.
          std::string name = "(unnamed device)";
          IMMDevice* dev = nullptr;
          if (SUCCEEDED(collection->Item(i, &dev)) && dev) {
            IPropertyStore* props = nullptr;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && props) {
              PROPVARIANT value;
              PropVariantInit(&value);
              if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &value)) &&
                  value.vt == VT_LPWSTR && value.pwszVal) {
                char utf8[256] = {0};
                WideCharToMultiByte(CP_UTF8, 0, value.pwszVal, -1, utf8, sizeof(utf8) - 1,
                                    nullptr, nullptr);
                name = utf8;
              }
              PropVariantClear(&value);
              props->Release();
            }
            dev->Release();
          }
          names.push_back(name);
        }
        collection->Release();
      }
      enumerator->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return names;
  }

  /** The capture rate in Hz: the one open() asked for, or the device's own
   *  when it was given none; 0 before open(). */
  double sampleRate() const { return sampleRate_; }
  /** The device open() opened, by its friendly name, UTF-8. */
  const std::string& deviceName() const { return deviceName_; }
  /** Why the last open() or run() failed. */
  const std::string& error() const { return error_; }
  /** True once since the capture device went away and the stream stopped.
   *  Polled from the UI thread, which closes the input and says so. */
  bool takeDeviceLost() { return lost_.exchange(false, std::memory_order_acq_rel); }
  /** Frames captured that the ring had no room for: the reader fell behind. */
  uint64_t droppedFrames() const { return ring_.droppedFrames(); }
  /** Frames read() padded with silence because none had arrived. */
  uint64_t starvedFrames() const { return ring_.starvedFrames(); }
  /** Frames read() would deliver now -- none while the cushion is filling:
   *  what a reader paced by the device, rather than by the computer's clock,
   *  waits for. [reader] */
  size_t availableFrames() const { return ring_.readable(); }

  /** Open a capture endpoint and ask it for `wantedRate`.
   *
   *  The rate is not negotiable from this side: the OUTPUT device already
   *  picked it and the DSP is already prepared at it. A capture device
   *  running at a different rate is the normal case, not an error -- a 44.1k
   *  interface feeding a 48k mixer is most people's setup -- so this asks
   *  Windows to convert, which it does properly and for free. */
  bool open(int deviceIndex, double wantedRate) {
    // Whatever was open goes first, as the output's open() does: a second
    // open() leaked the first client, capture client and event, left a COM
    // initialisation stop() never balanced, and reset the ring under a
    // running capture thread.
    stop();
    error_.clear();
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
    // Held to the end of open(): the fallback below activates the SAME device
    // again. It looked the default microphone up instead, so a chosen device
    // that refused the converting format was swapped for the default -- under
    // the chosen one's name, in the chosen one's channel count.
    struct HeldDevice {
      IMMDevice* device;
      ~HeldDevice() { device->Release(); }
    } held{device};

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
    return openDevice(device, wantedRate);
  }

private:
  /** The rest of open(), once it holds the device: activate it, settle the
   *  format, size the ring. A function of its own so the format decisions
   *  can be driven with a fake device (wasapi_lost_test); open() keeps COM,
   *  the lookup and the device's lifetime. */
  bool openDevice(IMMDevice* device, double wantedRate) {
    HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**) &client_);
    if (FAILED(hr) || !client_) return fail("the capture client would not activate", hr);

    WAVEFORMATEX* mix = nullptr;
    hr = client_->GetMixFormat(&mix);
    if (FAILED(hr) || !mix) return fail("no capture mix format", hr);
    captureChannels_ = mix->nChannels;

    // Ask for the rate the DSP is prepared at, in float, keeping the device's
    // own channel count. AUTOCONVERTPCM is what makes Windows do the
    // conversion; without it a 44.1k microphone simply refuses to open on a
    // 48k session and the user is told to go and change a control panel.
    //
    // More than two channels go as WAVE_FORMAT_EXTENSIBLE, in the mix's own
    // speaker layout. A plain WAVEFORMATEX cannot carry a layout and Windows
    // refuses one of more than two channels outright (E_INVALIDARG, measured
    // on capture and render endpoints alike): a four-channel microphone
    // array failed the converting open, fell back to its own rate, and on a
    // session at another rate did not open at all.
    WAVEFORMATEXTENSIBLE asked{};
    WAVEFORMATEX& wanted = asked.Format;
    wanted.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    wanted.nChannels = mix->nChannels;
    wanted.nSamplesPerSec = (DWORD) (wantedRate > 0.0 ? wantedRate : mix->nSamplesPerSec);
    wanted.wBitsPerSample = 32;
    wanted.nBlockAlign = (WORD) (wanted.nChannels * wanted.wBitsPerSample / 8);
    wanted.nAvgBytesPerSec = wanted.nSamplesPerSec * wanted.nBlockAlign;
    wanted.cbSize = 0;
    if (wanted.nChannels > 2) {
      wanted.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
      wanted.cbSize = (WORD) (sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX));
      asked.Samples.wValidBitsPerSample = 32;
      // The mix's layout when it states one; none (0) is accepted as well.
      if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22)
        asked.dwChannelMask = ((const WAVEFORMATEXTENSIBLE*) mix)->dwChannelMask;
      // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, spelled out rather than taken from
      // ksmedia.h and its initguid ceremony; isFloat32 reads its Data1.
      asked.SubFormat = GUID{0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
    }

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
      //
      // Only a mix of 32-bit float, which is all captureLoop() reads. This
      // took the mix as it came: a 16-bit PCM one opened, and its samples
      // would have been read as floats -- noise at any level -- where the
      // output refuses the same mix by name.
      if (!isFloat32(mix)) {
        CoTaskMemFree(mix);
        return fail("the capture mix is not 32-bit float, and Windows would not convert it", hr);
      }
      client_->Release();
      client_ = nullptr;
      const HRESULT act = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                           (void**) &client_);
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
    // And a cushion before the first read: capture arrives a device period at
    // a time and the output takes its own period, on another clock, so the
    // render side waits for two periods' worth -- one packet and one block
    // -- rather than crackling while the ring finds its own level.
    REFERENCE_TIME period = 0, minimum = 0;
    size_t cushion = (size_t) bufferFrames / 2;
    if (SUCCEEDED(client_->GetDevicePeriod(&period, &minimum)) && period > 0)
      cushion = (size_t) ((double) period * sampleRate_ / 1.0e7) * 2;
    ring_.setPrime(cushion);
    scratch_.assign((size_t) bufferFrames * 2, 0.0f);
    return true;
  }

public:
  /** Start capturing on a thread of its own. True when already capturing;
   *  false, with error() saying why, before a successful open() or when the
   *  stream will not start. After a lost device it starts afresh. */
  bool run() {
    if (!client_ || !captureClient_) return false;
    // As the output's run(): a second std::thread over a joinable one is
    // std::terminate. Still capturing, nothing to do; ended on a lost
    // device, joined and asked for again.
    if (thread_.joinable()) {
      if (looping_.load(std::memory_order_acquire)) return true;
      thread_.join();
    }
    lost_.store(false, std::memory_order_relaxed);
    running_.store(true, std::memory_order_release);
    looping_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { captureLoop(); });
    const HRESULT hr = client_->Start();
    if (FAILED(hr)) {
      stop();
      return fail("the capture stream would not start", hr);
    }
    return true;
  }

  /** Stop the capture thread and join it, then release the stream; on the
   *  thread that called open(). Harmless when nothing is open. */
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
  friend struct WasapiInputTest; // the device-loss test drives the loop with fakes
  std::atomic<bool> lost_{false};

  // 40 ms. Long enough that a scheduling hiccup on either side is absorbed,
  // short enough that a player does not hear their own hands late.
  static constexpr REFERENCE_TIME kBufferDuration = 400000; // 100-ns units

  /** 32-bit IEEE float, plainly or as WAVE_FORMAT_EXTENSIBLE's subformat:
   *  the output's test (audio_wasapi.h), Data1 of the KSDATAFORMAT GUID
   *  being the wave format tag, 3 for float. */
  static bool isFloat32(const WAVEFORMATEX* f) {
    bool isFloat = f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22)
      isFloat = ((const WAVEFORMATEXTENSIBLE*) f)->SubFormat.Data1 == 3;
    return isFloat && f->wBitsPerSample == 32;
  }

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
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    bool gone = false;
    while (running_.load(std::memory_order_acquire)) {
      if (WaitForSingleObject(event_, 200) != WAIT_OBJECT_0) {
        // No wake for 200 ms: a microphone that went away stops its stream
        // and its event, and a loop that only asked on a wake never found
        // out. Asked on the timeout too, as the output does.
        UINT32 waiting = 0;
        const HRESULT quiet = captureClient_->GetNextPacketSize(&waiting);
        if (running_.load(std::memory_order_acquire) &&
            (quiet == AUDCLNT_E_DEVICE_INVALIDATED || quiet == AUDCLNT_E_SERVICE_NOT_RUNNING)) {
          gone = true;
          break;
        }
        continue;
      }
      if (!running_.load(std::memory_order_acquire)) break;

      UINT32 packet = 0;
      HRESULT hr = S_OK;
      while (SUCCEEDED(hr = captureClient_->GetNextPacketSize(&packet)) && packet > 0) {
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        hr = captureClient_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
        if (FAILED(hr)) break;

        // Through the scratch open() sized for the whole device buffer, a
        // piece at a time should a packet ever be larger: growing it here
        // was an allocation on the capture thread.
        const UINT32 room = (UINT32) (scratch_.size() / 2);
        for (UINT32 done = 0; done < frames && room > 0;) {
          const UINT32 take = frames - done < room ? frames - done : room;
          // AUDCLNT_BUFFERFLAGS_SILENT means the pointer is not worth reading
          // and the frames are still real time that must not go missing --
          // dropping them would make the stream shorter than the clock.
          if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            std::memset(scratch_.data(), 0, (size_t) take * 2 * sizeof(float));
          } else {
            const uint32_t ch = captureChannels_ ? captureChannels_ : 1;
            const float* in = (const float*) data + (size_t) done * ch;
            for (UINT32 i = 0; i < take; ++i) {
              scratch_[(size_t) i * 2] = in[(size_t) i * ch];
              scratch_[(size_t) i * 2 + 1] = ch > 1 ? in[(size_t) i * ch + 1] : in[(size_t) i * ch];
            }
          }
          ring_.write(scratch_.data(), take);
          done += take;
        }
        captureClient_->ReleaseBuffer(frames);
      }
      // An unplugged microphone fails every call from here on: reported, and
      // the loop ends, rather than waking every 200 ms to feed the effect
      // silence that looks exactly like a quiet room.
      if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING) {
        gone = true;
        break;
      }
    }
    if (com) CoUninitialize(); // balanced only when it succeeded
    // Out of the loop before the loss is told, as the output's loop does, so
    // a run() on hearing of it finds this thread done.
    looping_.store(false, std::memory_order_release);
    if (gone) lost_.store(true, std::memory_order_release);
  }

  IAudioClient* client_ = nullptr;
  IAudioCaptureClient* captureClient_ = nullptr;
  HANDLE event_ = nullptr;
  std::thread thread_;
  std::atomic<bool> running_{false};
  /** The capture thread is still in its loop; false once it has left it. */
  std::atomic<bool> looping_{false};
  bool comInitialised_ = false;

  AudioRing ring_;
  std::vector<float> scratch_;
  uint32_t captureChannels_ = 2;
  double sampleRate_ = 0.0;
  std::string deviceName_, error_;
};

/** The live-input backend the standalone uses on this platform. */
using PlatformAudioInput = WasapiInput;

} // namespace standalone
} // namespace sonore

#endif // _WIN32
