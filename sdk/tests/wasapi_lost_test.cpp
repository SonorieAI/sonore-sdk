// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: a WASAPI stream whose device goes away says so and stops.
//
// Unplugging an interface invalidates its stream for good: every call fails
// with AUDCLNT_E_DEVICE_INVALIDATED from then on. The render loop treated
// that like any passing failure and retried on every wake, so the
// standalone stayed "open" and silent until the user noticed and picked a
// device by hand. It now raises takeDeviceLost() -- which the standalone's
// UI tick answers by reopening the default device -- and ends.
//
// No device is unplugged here: the loop is driven through a fake client that
// starts failing the way an unplugged one does. A friend of WasapiOutput puts
// the fakes where open() would have put the real ones.
//
// The same fakes, left where an earlier open() put them, show what a second
// open() does with them: it must release them, and leave COM initialised on
// its thread exactly as often as stop() uninitialises it. Those two run the
// real open(), against whatever devices the machine has -- none is fine.
//
// And a capture device whose mix is not float, which Windows will not
// convert: the input's fallback opened it anyway and read its samples as
// floats. A fake device (through open()'s second half, openDevice) has
// that mix; no real one here does.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <sonore/audio_ring.h>
#include <sonore/audio_wasapi.h>
#include <sonore/audio_wasapi_input.h>

namespace sonore {
namespace standalone {
struct WasapiOutputTest {
  static void attach(WasapiOutput& out, IAudioClient* client, IAudioRenderClient* render,
                     HANDLE event, UINT32 frames) {
    out.client_ = client;
    out.renderClient_ = render;
    out.event_ = event;
    out.bufferFrames_ = frames;
    out.channels_ = 2;
    out.render_ = [](float* buffer, uint32_t n, uint32_t ch) {
      std::memset(buffer, 0, (size_t) n * ch * sizeof(float));
    };
  }
};
struct WasapiInputTest {
  static void attach(WasapiInput& in, IAudioClient* client, IAudioCaptureClient* capture,
                     HANDLE event) {
    in.client_ = client;
    in.captureClient_ = capture;
    in.event_ = event;
    in.captureChannels_ = 2;
    in.ring_.reset(480 * 4);
    in.scratch_.assign(480 * 2, 0.0f);
  }
  static bool openDevice(WasapiInput& in, IMMDevice* device, double rate) {
    return in.openDevice(device, rate);
  }
};
} // namespace standalone
} // namespace sonore

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

/** Plays normally for two wakes, then fails as an unplugged device does --
 *  and keeps the loop awake throughout, as a running stream's event does. */
struct FakeClient : IAudioClient {
  HANDLE event = nullptr;
  std::atomic<int> paddingCalls{0};
  int failFrom = 3;
  /** From this call on the event no longer fires: the stream has stopped, as
   *  a real one does once its endpoint is gone. */
  int quietFrom = 1000000;
  /** Counted so a second open() can be caught leaking the first client. */
  std::atomic<int> releases{0};

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override {
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override {
    ++releases;
    return 1;
  }
  HRESULT STDMETHODCALLTYPE Initialize(AUDCLNT_SHAREMODE, DWORD, REFERENCE_TIME, REFERENCE_TIME,
                                       const WAVEFORMATEX*, LPCGUID) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE GetBufferSize(UINT32* frames) override {
    *frames = 480;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetStreamLatency(REFERENCE_TIME*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetCurrentPadding(UINT32* padding) override {
    const int n = ++paddingCalls;
    if (n < quietFrom) SetEvent(event);
    if (n >= failFrom) return AUDCLNT_E_DEVICE_INVALIDATED;
    *padding = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE IsFormatSupported(AUDCLNT_SHAREMODE, const WAVEFORMATEX*,
                                              WAVEFORMATEX**) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE GetMixFormat(WAVEFORMATEX**) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetDevicePeriod(REFERENCE_TIME*, REFERENCE_TIME*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE Start() override {
    SetEvent(event);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Stop() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE Reset() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE SetEventHandle(HANDLE) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE GetService(REFIID, void**) override { return E_NOTIMPL; }
};

struct FakeRender : IAudioRenderClient {
  std::vector<float> buffer = std::vector<float>(480 * 2, 0.0f);
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override {
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  HRESULT STDMETHODCALLTYPE GetBuffer(UINT32, BYTE** data) override {
    *data = (BYTE*) buffer.data();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE ReleaseBuffer(UINT32, DWORD) override { return S_OK; }
};

/** A microphone that delivers nothing for two wakes, then is unplugged. */
struct FakeCapture : IAudioCaptureClient {
  HANDLE event = nullptr;
  std::atomic<int> packetCalls{0};
  int quietFrom = 1000000; // as FakeClient's
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override {
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  HRESULT STDMETHODCALLTYPE GetBuffer(BYTE**, UINT32*, DWORD*, UINT64*, UINT64*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE ReleaseBuffer(UINT32) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE GetNextPacketSize(UINT32* packet) override {
    const int n = ++packetCalls;
    if (n < quietFrom) SetEvent(event);
    if (n >= 3) return AUDCLNT_E_DEVICE_INVALIDATED;
    *packet = 0;
    return S_OK;
  }
};

/** A capture client with a mix of the caller's choosing that may refuse the
 *  converting Initialize, as a driver without AUTOCONVERTPCM does. Records
 *  what Initialize was finally given. */
struct MixClient : IAudioClient {
  FakeCapture capture;
  WORD tag = WAVE_FORMAT_PCM;
  WORD bits = 16;
  WORD channels = 2;
  unsigned long subFormat = 1; // Data1 of the EXTENSIBLE subformat: 1 PCM, 3 float
  bool convertRefused = true;
  int initialisedBits = 0;
  WORD initialisedTag = 0;
  WORD initialisedChannels = 0;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override {
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  HRESULT STDMETHODCALLTYPE Initialize(AUDCLNT_SHAREMODE, DWORD flags, REFERENCE_TIME, REFERENCE_TIME,
                                       const WAVEFORMATEX* f, LPCGUID) override {
    // As every endpoint here answers, render and capture, with or without
    // conversion: more than two channels need WAVE_FORMAT_EXTENSIBLE, and a
    // plain WAVEFORMATEX of four or six is E_INVALIDARG.
    if (f->wFormatTag != WAVE_FORMAT_EXTENSIBLE && f->nChannels > 2) return E_INVALIDARG;
    if ((flags & 0x80000000 /* AUTOCONVERTPCM */) && convertRefused) return AUDCLNT_E_UNSUPPORTED_FORMAT;
    initialisedBits = f->wBitsPerSample;
    initialisedTag = f->wFormatTag;
    initialisedChannels = f->nChannels;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetBufferSize(UINT32* frames) override {
    *frames = 480;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetStreamLatency(REFERENCE_TIME*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetCurrentPadding(UINT32*) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE IsFormatSupported(AUDCLNT_SHAREMODE, const WAVEFORMATEX*,
                                              WAVEFORMATEX**) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE GetMixFormat(WAVEFORMATEX** out) override {
    auto* f = (WAVEFORMATEXTENSIBLE*) CoTaskMemAlloc(sizeof(WAVEFORMATEXTENSIBLE));
    std::memset(f, 0, sizeof(*f));
    f->Format.wFormatTag = tag;
    f->Format.nChannels = channels;
    f->Format.nSamplesPerSec = 48000;
    f->Format.wBitsPerSample = bits;
    f->Format.nBlockAlign = (WORD) (channels * bits / 8);
    f->Format.nAvgBytesPerSec = 48000u * f->Format.nBlockAlign;
    if (tag == WAVE_FORMAT_EXTENSIBLE) {
      f->Format.cbSize = 22;
      f->Samples.wValidBitsPerSample = bits;
      f->SubFormat.Data1 = subFormat;
    }
    *out = &f->Format;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDevicePeriod(REFERENCE_TIME* period, REFERENCE_TIME* minimum) override {
    *period = *minimum = 100000;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Start() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE Stop() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE Reset() override { return S_OK; }
  HRESULT STDMETHODCALLTYPE SetEventHandle(HANDLE) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE GetService(REFIID, void** out) override {
    *out = &capture;
    return S_OK;
  }
};

/** An endpoint that activates the MixClient it holds. */
struct MixDevice : IMMDevice {
  MixClient client;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override {
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  HRESULT STDMETHODCALLTYPE Activate(REFIID, DWORD, PROPVARIANT*, void** out) override {
    *out = &client;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OpenPropertyStore(DWORD, IPropertyStore** out) override {
    *out = nullptr;
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE GetId(LPWSTR* out) override {
    *out = nullptr;
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE GetState(DWORD* out) override {
    *out = DEVICE_STATE_ACTIVE;
    return S_OK;
  }
};

void captureMix() {
  std::printf("\n── a capture mix Windows will not convert ──\n");
  char line[260];
  {
    MixDevice pcm16; // the default: 16-bit PCM, conversion refused
    sonore::standalone::WasapiInput in;
    const bool opened = sonore::standalone::WasapiInputTest::openDevice(in, &pcm16, 48000.0);
    std::snprintf(line, sizeof(line), "a 16-bit PCM mix is refused, not read as float (\"%s\")",
                  in.error().c_str());
    check(!opened && in.error().find("32-bit float") != std::string::npos &&
              pcm16.client.initialisedBits == 0,
          line);
    in.stop();
  }
  {
    MixDevice int32;
    int32.client.tag = WAVE_FORMAT_EXTENSIBLE;
    int32.client.bits = 32;
    int32.client.subFormat = 1; // 32-bit INTEGER PCM: the right width, the wrong numbers
    sonore::standalone::WasapiInput in;
    check(!sonore::standalone::WasapiInputTest::openDevice(in, &int32, 48000.0),
          "...and so is a 32-bit integer mix in WAVE_FORMAT_EXTENSIBLE");
    in.stop();
  }
  {
    MixDevice float32;
    float32.client.tag = WAVE_FORMAT_EXTENSIBLE;
    float32.client.bits = 32;
    float32.client.subFormat = 3;
    sonore::standalone::WasapiInput in;
    const bool opened = sonore::standalone::WasapiInputTest::openDevice(in, &float32, 48000.0);
    check(opened && float32.client.initialisedBits == 32 && in.sampleRate() == 48000.0,
          "a 32-bit float mix still opens through the fallback, at its own rate");
    in.stop();
  }
  {
    MixDevice converting; // 16-bit PCM, but Windows converts: the float it asks for
    converting.client.convertRefused = false;
    sonore::standalone::WasapiInput in;
    const bool opened = sonore::standalone::WasapiInputTest::openDevice(in, &converting, 44100.0);
    check(opened && converting.client.initialisedTag == WAVE_FORMAT_IEEE_FLOAT &&
              in.sampleRate() == 44100.0,
          "and a 16-bit device Windows converts opens as float at the rate asked for");
    in.stop();
  }
  {
    // A four-channel microphone array at 48 kHz, the output at 44.1. Windows
    // converts it -- given a format it takes: a plain WAVEFORMATEX of more
    // than two channels is E_INVALIDARG on every endpoint here, so the
    // converting open failed, the fallback opened the mix at 48 kHz, and the
    // rates disagreed. The input would not open at all.
    MixDevice quad;
    quad.client.tag = WAVE_FORMAT_EXTENSIBLE;
    quad.client.bits = 32;
    quad.client.subFormat = 3;
    quad.client.channels = 4;
    quad.client.convertRefused = false;
    sonore::standalone::WasapiInput in;
    const bool opened = sonore::standalone::WasapiInputTest::openDevice(in, &quad, 44100.0);
    std::snprintf(line, sizeof(line),
                  "a four-channel device opens converted to the rate asked for (%s; %.0f Hz, "
                  "%u channels asked for as format %u)",
                  opened ? "opened" : in.error().c_str(), in.sampleRate(),
                  (unsigned) quad.client.initialisedChannels, (unsigned) quad.client.initialisedTag);
    check(opened && in.sampleRate() == 44100.0 && quad.client.initialisedChannels == 4 &&
              quad.client.initialisedTag == WAVE_FORMAT_EXTENSIBLE,
          line);
    in.stop();
  }
}

/** Open, open again, stop once, on a thread of its own; then ask COM whether
 *  that thread is still initialised. S_FALSE from CoInitializeEx means it is:
 *  the second open()'s initialisation was never balanced. */
template <typename OpenTwice>
bool comLeftInitialised(OpenTwice openTwice) {
  HRESULT after = E_FAIL;
  std::thread t([&] {
    openTwice();
    after = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(after)) CoUninitialize();
  });
  t.join();
  return after == S_FALSE;
}

void reopen() {
  std::printf("\n── WASAPI, opened a second time ──\n");
  // A second open() overwrote client_: the IAudioClient, render or capture
  // client and event of the first leaked, and its CoInitializeEx was one
  // more than stop() undid. It stops what was open first now.
  char line[200];
  {
    FakeClient first;
    FakeRender firstRender;
    first.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    sonore::standalone::WasapiOutput out;
    sonore::standalone::WasapiOutputTest::attach(out, &first, &firstRender, first.event, 480);
    const bool opened = out.open([](float*, uint32_t, uint32_t) {});
    std::snprintf(line, sizeof(line), "an output opened again releases the client it had "
                  "(%d release(s); the new open %s)", first.releases.load(),
                  opened ? "succeeded" : out.error().c_str());
    check(first.releases.load() == 1, line);
    out.stop();
  }
  {
    FakeClient first;
    FakeCapture firstCapture;
    first.event = firstCapture.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    sonore::standalone::WasapiInput in;
    sonore::standalone::WasapiInputTest::attach(in, &first, &firstCapture, first.event);
    const bool opened = in.open(0, 48000.0);
    std::snprintf(line, sizeof(line), "...and so does an input (%d release(s); the new open %s)",
                  first.releases.load(), opened ? "succeeded" : in.error().c_str());
    check(first.releases.load() == 1, line);
    in.stop();
  }
  const bool outputLeft = comLeftInitialised([] {
    sonore::standalone::WasapiOutput out;
    out.open([](float*, uint32_t, uint32_t) {});
    out.open([](float*, uint32_t, uint32_t) {});
    out.stop();
  });
  const bool inputLeft = comLeftInitialised([] {
    sonore::standalone::WasapiInput in;
    in.open(0, 48000.0);
    in.open(0, 48000.0);
    in.stop();
  });
  check(!outputLeft, "open, open, stop on an output leaves COM as the thread found it");
  check(!inputLeft, "...and on an input");
}

/** Waits up to two seconds for a stream to report its device lost. */
template <typename Stream>
bool lostWithin2s(Stream& s) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < until) {
    if (s.takeDeviceLost()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

void runTwice() {
  std::printf("\n── run() a second time ──\n");
  // run() assigned a new std::thread over the one it had started, which is
  // std::terminate while that one is joinable -- and it stays joinable after
  // its loop ends on a lost device, until stop(). So run() twice, or run()
  // to restart a stream whose device went away, ended the process.
  {
    FakeClient client;
    FakeRender render;
    client.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    client.failFrom = 1000000; // healthy throughout
    sonore::standalone::WasapiOutput out;
    sonore::standalone::WasapiOutputTest::attach(out, &client, &render, client.event, 480);
    const bool first = out.run();
    const bool second = out.run();
    const int before = client.paddingCalls.load();
    // Up to two seconds for the render thread to come round again: a fixed
    // 50 ms was not enough on a machine busy compiling.
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (client.paddingCalls.load() <= before && std::chrono::steady_clock::now() < until)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    check(first && second && client.paddingCalls.load() > before,
          "run() on an output already running is a no-op, and it keeps rendering");
    out.stop();
  }
  {
    FakeClient client;
    FakeRender render;
    client.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    sonore::standalone::WasapiOutput out;
    sonore::standalone::WasapiOutputTest::attach(out, &client, &render, client.event, 480);
    out.run();
    const bool lost = lostWithin2s(out);
    out.run(); // its device went away; this asks for the stream again
    check(lost && lostWithin2s(out),
          "run() after the device went away starts the stream again, which says so again");
    out.stop();
  }
  {
    FakeClient side;
    FakeCapture capture;
    side.event = capture.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    side.failFrom = 1000000;
    sonore::standalone::WasapiInput in;
    sonore::standalone::WasapiInputTest::attach(in, &side, &capture, capture.event);
    in.run();
    const bool lost = lostWithin2s(in);
    in.run();
    in.run();
    check(lost && lostWithin2s(in), "...and so for an input, run() twice included");
    in.stop();
  }
}

} // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("── WASAPI output, device lost ──\n");

  FakeClient client;
  FakeRender render;
  client.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

  sonore::standalone::WasapiOutput out;
  sonore::standalone::WasapiOutputTest::attach(out, &client, &render, client.event, 480);
  check(out.run(), "the stream starts on the fake device");

  bool lost = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!lost && std::chrono::steady_clock::now() < deadline) {
    lost = out.takeDeviceLost();
    if (!lost) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  check(lost, "a device that starts failing with AUDCLNT_E_DEVICE_INVALIDATED is reported lost");
  check(!out.takeDeviceLost(), "...once: the report is taken, not left standing");

  // And the loop has ENDED: an invalidated stream fails every call for good,
  // and retrying it on every wake was the silent, open-forever stream.
  const int callsAtReport = client.paddingCalls.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  char line[200];
  std::snprintf(line, sizeof(line), "and the render loop stops asking a device that is gone "
                "(%d calls at the report, %d after 300 ms)", callsAtReport,
                client.paddingCalls.load());
  check(client.paddingCalls.load() == callsAtReport, line);

  out.stop(); // releases the fakes and closes the event, as for a real device

  // The same for capture: an unplugged microphone is reported, and the loop
  // ends instead of waking every 200 ms to feed the effect silence.
  {
    FakeClient captureSide;
    FakeCapture capture;
    captureSide.event = capture.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    captureSide.failFrom = 1000000; // the CLIENT is fine; the capture interface fails
    sonore::standalone::WasapiInput in;
    sonore::standalone::WasapiInputTest::attach(in, &captureSide, &capture, capture.event);
    check(in.run(), "a capture stream starts on the fake microphone");
    bool inputLost = false;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!inputLost && std::chrono::steady_clock::now() < until) {
      inputLost = in.takeDeviceLost();
      if (!inputLost) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(inputLost, "an unplugged capture device is reported lost");
    const int at = capture.packetCalls.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::snprintf(line, sizeof(line), "and the capture loop stops asking it (%d calls, then %d)",
                  at, capture.packetCalls.load());
    check(capture.packetCalls.load() == at, line);
    in.stop();
  }
  // A device that goes QUIET: its event stops with its stream, so the loop is
  // never woken to find the failure. Reported from the wait's timeout; it
  // was not reported at all.
  {
    FakeClient quiet;
    FakeRender quietRender;
    quiet.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    quiet.quietFrom = 2; // the second call is the last wake
    quiet.failFrom = 3;  // the third, from the timeout, finds it gone
    sonore::standalone::WasapiOutput q;
    sonore::standalone::WasapiOutputTest::attach(q, &quiet, &quietRender, quiet.event, 480);
    q.run();
    bool quietLost = false;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!quietLost && std::chrono::steady_clock::now() < until) {
      quietLost = q.takeDeviceLost();
      if (!quietLost) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(quietLost, "an output whose stream stops waking it is still reported lost");
    q.stop();

    FakeClient micSide;
    FakeCapture quietMic;
    micSide.event = quietMic.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    micSide.failFrom = 1000000;
    quietMic.quietFrom = 2;
    sonore::standalone::WasapiInput mic;
    sonore::standalone::WasapiInputTest::attach(mic, &micSide, &quietMic, quietMic.event);
    mic.run();
    bool micLost = false;
    const auto micUntil = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!micLost && std::chrono::steady_clock::now() < micUntil) {
      micLost = mic.takeDeviceLost();
      if (!micLost) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(micLost, "...and so is a microphone whose stream stops waking it");
    mic.stop();
  }

  reopen();
  runTwice();
  captureMix();

  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  if (g_failures == 0) std::printf("SONORE WASAPI LOST PASSED\n");
  return g_failures == 0 ? 0 : 1;
}
