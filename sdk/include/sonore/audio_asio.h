// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: ASIO output, without Steinberg's SDK.
//
// ASIO is the reason a Windows musician can run at 64 samples instead of 512.
// WASAPI in shared mode adds a buffer nobody asked for; in exclusive mode it
// is better and still not an interface driver talking to its own hardware.
// Every serious Windows DAW offers ASIO first, and a standalone that cannot
// is a standalone people use once.
//
// ── Why there is no asio.h here ──────────────────────────────────────────────
//
// Steinberg's ASIO SDK may not be redistributed. That is exactly why every
// framework makes you fetch it yourself, and why this SDK listed ASIO as
// absent for most of its life.
//
// It is not needed. IASIO is a COM vtable, and an interface declared in order
// to INTEROPERATE is not the other party's source. This file declares that
// vtable itself: the same thing it already does for LV2's UI descriptor and
// LV2's worker interface, neither of which is vendored either. No Steinberg
// code is compiled, linked, or shipped.
//
// I am not a lawyer and this is worth checking before a commercial release.
// What I can say precisely is what this file contains: a vtable layout, a
// registry key name, and our own code.
//
// ── What ASIO does differently from every other audio API ────────────────────
//
//   * A driver is instantiated with CoCreateInstance where the IID EQUALS the
//     CLSID. It is not a real COM interface and does not answer QueryInterface
//     for anything else.
//   * Buffers are PER CHANNEL and in the driver's own sample format, which is
//     usually 32-bit integer and is whatever the hardware actually wants.
//   * There is one driver at a time, machine-wide. Opening a second while the
//     first is running fails, and that is the hardware's rule rather than
//     ours.
#pragma once

#include "asio_format.h"

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>

#include <cstdint>
#include <functional>
#include <cstring>
#include <string>
#include <vector>

namespace sonore {
namespace asio {

using AsioBool = long;
using AsioError = long;
using AsioSampleRate = double;

enum : AsioError {
  kAsioOk = 0,
  kAsioSuccess = 0x3f4847a0, // ASE_SUCCESS, which future() returns instead of 0
};

struct AsioClockSource {
  long index;
  long associatedChannel;
  long associatedGroup;
  AsioBool isCurrentSource;
  char name[32];
};

struct AsioChannelInfo {
  long channel;
  AsioBool isInput;
  AsioBool isActive;
  long channelGroup;
  long type;
  char name[32];
};

struct AsioBufferInfo {
  AsioBool isInput;
  long channelNum;
  void* buffers[2];
};

struct AsioTimeStamp {
  long hi;
  unsigned long lo;
};
struct AsioSamples {
  long hi;
  unsigned long lo;
};

struct AsioCallbacks {
  void (*bufferSwitch)(long doubleBufferIndex, AsioBool directProcess);
  void (*sampleRateDidChange)(AsioSampleRate sRate);
  long (*asioMessage)(long selector, long value, void* message, double* opt);
  void* (*bufferSwitchTimeInfo)(void* params, long doubleBufferIndex, AsioBool directProcess);
};

/**
 * The driver interface, declared rather than included.
 *
 * The ORDER of these is the ABI: a vtable is positional, so a method inserted
 * or omitted shifts every one below it and the first call lands somewhere
 * else entirely. It is transcribed from the published interface and its shape
 * is checked at run time before anything is called -- see the sanity check in
 * open().
 */
class IAsio : public IUnknown {
public:
  virtual AsioBool init(void* sysHandle) = 0;
  virtual void getDriverName(char* name) = 0;
  virtual long getDriverVersion() = 0;
  virtual void getErrorMessage(char* text) = 0;
  virtual AsioError start() = 0;
  virtual AsioError stop() = 0;
  virtual AsioError getChannels(long* numInputChannels, long* numOutputChannels) = 0;
  virtual AsioError getLatencies(long* inputLatency, long* outputLatency) = 0;
  virtual AsioError getBufferSize(long* minSize, long* maxSize, long* preferredSize,
                                  long* granularity) = 0;
  virtual AsioError canSampleRate(AsioSampleRate sampleRate) = 0;
  virtual AsioError getSampleRate(AsioSampleRate* sampleRate) = 0;
  virtual AsioError setSampleRate(AsioSampleRate sampleRate) = 0;
  virtual AsioError getClockSources(AsioClockSource* clocks, long* numSources) = 0;
  virtual AsioError setClockSource(long reference) = 0;
  virtual AsioError getSamplePosition(AsioSamples* sPos, AsioTimeStamp* tStamp) = 0;
  virtual AsioError getChannelInfo(AsioChannelInfo* info) = 0;
  virtual AsioError createBuffers(AsioBufferInfo* bufferInfos, long numChannels, long bufferSize,
                                  AsioCallbacks* callbacks) = 0;
  virtual AsioError disposeBuffers() = 0;
  virtual AsioError controlPanel() = 0;
  virtual AsioError future(long selector, void* opt) = 0;
  virtual AsioError outputReady() = 0;
};

/** One installed driver, as the registry describes it. */
struct DriverInfo {
  std::string name;
  std::string clsid;
};

/**
 * Every ASIO driver installed on this machine.
 *
 * They live under HKLM\\SOFTWARE\\ASIO, one key per driver, each holding a
 * CLSID string. There is no enumeration API -- the registry IS the API, which
 * is why every host on Windows reads it the same way.
 *
 * A key without a CLSID is skipped rather than reported: an installer that
 * left half an entry behind is not a device a user can pick.
 */
inline std::vector<DriverInfo> listDrivers() {
  std::vector<DriverInfo> drivers;
  HKEY root = nullptr;
  // KEY_WOW64_64KEY: a 64-bit host must read the 64-bit view, and without it
  // a 32-bit build would silently enumerate a different set of drivers.
  if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\ASIO", 0, KEY_READ | KEY_WOW64_64KEY,
                    &root) != ERROR_SUCCESS)
    return drivers;

  for (DWORD index = 0;; ++index) {
    char name[256];
    DWORD nameLength = (DWORD) sizeof(name);
    if (RegEnumKeyExA(root, index, name, &nameLength, nullptr, nullptr, nullptr, nullptr) !=
        ERROR_SUCCESS)
      break;

    HKEY entry = nullptr;
    if (RegOpenKeyExA(root, name, 0, KEY_READ | KEY_WOW64_64KEY, &entry) != ERROR_SUCCESS)
      continue;
    char clsid[64] = {0};
    DWORD size = (DWORD) sizeof(clsid), type = 0;
    const bool haveClsid =
        RegQueryValueExA(entry, "CLSID", nullptr, &type, (LPBYTE) clsid, &size) == ERROR_SUCCESS &&
        type == REG_SZ && clsid[0];
    // The description is what a driver calls itself and the key name is what
    // the installer called it. They usually agree; when they do not, the key
    // name is the one every other host shows.
    RegCloseKey(entry);
    if (haveClsid) drivers.push_back({name, clsid});
  }
  RegCloseKey(root);
  return drivers;
}

/**
 * One ASIO driver, opened and streaming.
 *
 * ONE AT A TIME, machine-wide, and that is ASIO's rule rather than ours: the
 * callbacks are bare C function pointers with no user-data cookie, so a host
 * has nowhere to put a `this`. Every ASIO host on Windows keeps a single
 * static instance for exactly this reason, and a second driver opened while
 * the first is running fails at the hardware anyway.
 */
class AsioOutput {
public:
  /**
   * Fill `out` with `frames` frames of `channels` channels, having been given
   * the same many frames of `in`.
   *
   * `in` is null when no input was asked for, which is the common case for a
   * synth. It is NOT silence-filled in that case: a null pointer is a
   * question a DSP can answer, and a buffer of zeros is one it cannot tell
   * from a disconnected microphone.
   *
   * Called on the driver's own thread, which is the audio thread.
   */
  using RenderFn = std::function<void(const float* in, float* out, uint32_t frames,
                                      uint32_t channels)>;

  ~AsioOutput() { stop(); }

  double sampleRate() const { return sampleRate_; }
  uint32_t channels() const { return 2; }
  uint32_t bufferFrames() const { return (uint32_t) bufferFrames_; }

  /** The sample format the driver hands output back in, as an ASIO type code. */
  long format() const { return format_; }

  /** The sample format the driver takes input in. Not promised to be the one
   *  it hands output back in. */
  long inputFormat() const { return inputFormat_; }
  const std::string& error() const { return error_; }
  const std::string& driverName() const { return driverName_; }

  /**
   * Open one driver by name and start it.
   *
   * By NAME rather than by index, because the registry order is not stable --
   * an installer can add a driver and renumber everything after it, and an
   * index remembered in a settings file would then point at somebody else's
   * hardware.
   */
  /**
   * Everything up to and including createBuffers -- which is where the rate
   * and the block size are settled -- and NOT a single callback yet.
   *
   * Split from run() for the same reason WasapiOutput is: a caller has to
   * prepare its DSP at the rate the device chose, and it cannot do that until
   * the device has chosen, and it must not still be doing it when the first
   * callback arrives. Anything in between -- a printf, a settings file
   * written to disk -- widens a window in which the audio thread is calling
   * into an unprepared DSP.
   *
   * That window was real. Remembering the driver name between the two cost a
   * file write, and a file write was enough to segfault it every time.
   */
  bool open(const std::string& wanted, RenderFn render, bool withInput = false) {
    stop();
    error_.clear();
    render_ = std::move(render);
    wantInput_ = withInput;

    const auto drivers = listDrivers();
    const DriverInfo* chosen = nullptr;
    for (const auto& d : drivers)
      if (d.name == wanted) chosen = &d;
    if (!chosen) {
      error_ = "no ASIO driver called \"" + wanted + "\" is installed";
      return false;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    comInitialised_ = true;

    CLSID clsid{};
    wchar_t wide[64] = {};
    MultiByteToWideChar(CP_ACP, 0, chosen->clsid.c_str(), -1, wide, 64);
    if (CLSIDFromString(wide, &clsid) != NOERROR) {
      error_ = "the driver's registry entry has an unreadable CLSID";
      return false;
    }
    // The IID is the CLSID. ASIO is not real COM and answers QueryInterface
    // for nothing else.
    if (FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid,
                                (void**) &driver_)) ||
        !driver_) {
      error_ = "the driver would not load; its hardware may not be connected";
      return false;
    }

    // init() takes a window handle on drivers that put up a dialog. The
    // desktop window is what every host passes when it has no window of its
    // own, and a driver that ignores it is the common case.
    if (!driver_->init(GetDesktopWindow())) {
      char why[128] = {0};
      driver_->getErrorMessage(why);
      error_ = std::string("the driver refused to initialise: ") + why;
      return fail();
    }
    driverName_ = wanted;

    long inputs = 0, outputs = 0;
    if (driver_->getChannels(&inputs, &outputs) != kAsioOk || outputs < 2) {
      error_ = "the driver has fewer than two output channels";
      return fail();
    }
    // Input is asked for and not demanded. An interface with no inputs is a
    // perfectly good output device, and refusing to open it because a caller
    // said `withInput` would take away the thing that does work.
    haveInput_ = wantInput_ && inputs >= 2;
    inputChannels_ = haveInput_ ? 2 : 0;

    long minSize = 0, maxSize = 0, preferred = 0, granularity = 0;
    if (driver_->getBufferSize(&minSize, &maxSize, &preferred, &granularity) != kAsioOk ||
        preferred <= 0) {
      error_ = "the driver would not say what buffer size it wants";
      return fail();
    }
    // The PREFERRED size, always. It is the one the driver is tuned for, and
    // a host that picks its own number is the host that gets blamed for the
    // crackle.
    bufferFrames_ = preferred;

    AsioSampleRate rate = 0.0;
    driver_->getSampleRate(&rate);
    if (!(rate > 0.0)) {
      // A driver that has never been told reports zero. 48 kHz is asked for
      // rather than assumed: if it cannot, whatever it settles on is read
      // back below.
      if (driver_->canSampleRate(48000.0) == kAsioOk) driver_->setSampleRate(48000.0);
      driver_->getSampleRate(&rate);
    }
    sampleRate_ = rate > 0.0 ? rate : 48000.0;

    AsioChannelInfo info{};
    info.channel = 0;
    info.isInput = 0;
    if (driver_->getChannelInfo(&info) != kAsioOk) {
      error_ = "the driver would not describe its output channels";
      return fail();
    }
    format_ = info.type;
    if (!formatIsSupported(format_)) {
      char why[96];
      std::snprintf(why, sizeof(why), "the driver's sample format (%ld) is not one this "
                                      "build converts", format_);
      error_ = why;
      return fail();
    }

    // The input format ASKED FOR SEPARATELY, because ASIO does not promise the
    // two agree: getChannelInfo takes an isInput, and a driver whose converters
    // differ front to back is entitled to answer differently. Reading input
    // through the output's format is silence at best and a full-scale buzz at
    // worst, and neither of those says which of the two went wrong.
    inputFormat_ = format_;
    if (haveInput_) {
      AsioChannelInfo in{};
      in.channel = 0;
      in.isInput = 1;
      if (driver_->getChannelInfo(&in) != kAsioOk || !formatIsSupported(in.type)) {
        // Not fatal. The output still works, and a caller that asked for input
        // and did not get it is told so by hasInput() -- which is the whole
        // reason that method exists.
        haveInput_ = false;
        inputChannels_ = 0;
      } else {
        inputFormat_ = in.type;
      }
    }

    // Input first, then output. ASIO takes ONE array describing both, and one
    // createBuffers call for the pair -- there is no separate input device to
    // open, because an interface is one piece of hardware with one clock.
    //
    // The first two of each. A stereo standalone wants the pair the user has
    // plugged their monitors into, which on every interface is 1-2.
    int slot = 0;
    for (uint32_t i = 0; i < inputChannels_; ++i, ++slot) {
      buffers_[slot].isInput = 1;
      buffers_[slot].channelNum = (long) i;
      buffers_[slot].buffers[0] = buffers_[slot].buffers[1] = nullptr;
    }
    for (int i = 0; i < 2; ++i, ++slot) {
      buffers_[slot].isInput = 0;
      buffers_[slot].channelNum = i;
      buffers_[slot].buffers[0] = buffers_[slot].buffers[1] = nullptr;
    }

    instance_ = this;
    scratch_.assign((size_t) bufferFrames_ * 2, 0.0f);
    inputScratch_.assign((size_t) bufferFrames_ * 2, 0.0f);

    // A MEMBER, not a local, and this is not a style preference.
    //
    // createBuffers takes a POINTER to the callback table and the driver keeps
    // that pointer -- it does not copy the four function addresses out of it.
    // A table on the stack therefore lives exactly as long as the frame it was
    // declared in, and every callback after that reads whatever has since been
    // written over it.
    //
    // This was a local until the day open() and run() became separate
    // functions. Until then driver->start() ran in the same frame, so the
    // memory was still intact when the driver first looked at it, and the bug
    // was invisible. Splitting the function let open() return -- and the very
    // first callback jumped through a function pointer that was now a piece of
    // some other stack. Instant segfault, on both drivers here.
    //
    // Nothing about the old arrangement was correct; it was lucky. A member
    // lives as long as the driver that was handed its address.
    callbacks_ = AsioCallbacks{};
    callbacks_.bufferSwitch = &AsioOutput::onBufferSwitch;
    callbacks_.sampleRateDidChange = &AsioOutput::onSampleRateChanged;
    callbacks_.asioMessage = &AsioOutput::onMessage;
    callbacks_.bufferSwitchTimeInfo = nullptr;
    if (driver_->createBuffers(buffers_, (long) (inputChannels_ + 2), bufferFrames_,
                               &callbacks_) != kAsioOk) {
      error_ = "the driver would not give us buffers";
      instance_ = nullptr;
      return fail();
    }
    buffersMade_ = true;

    return true;
  }

  /** Start the clock. Every callback happens after this returns. */
  bool run() {
    if (!driver_ || !buffersMade_) {
      error_ = "run() before a successful open()";
      return false;
    }
    if (driver_->start() != kAsioOk) {
      error_ = "the driver would not start";
      return fail();
    }
    running_ = true;
    return true;
  }

  /** open() and run() together, for callers with nothing to do in between. */
  bool start(const std::string& wanted, RenderFn render, bool withInput = false) {
    return open(wanted, std::move(render), withInput) && run();
  }

  /** Whether input really was opened. A caller asks for it; the hardware
   *  decides. */
  bool hasInput() const { return haveInput_; }

  /**
   * Open the driver's own control panel.
   *
   * Where buffer size and clock source actually live on every interface worth
   * owning -- ASIO deliberately has no API for setting them, because they are
   * the driver's business and a host guessing at them is how a host ends up
   * fighting the hardware.
   *
   * Only while open: a panel needs an initialised driver, and a caller that
   * asks before starting gets false rather than a dialog belonging to
   * nothing.
   */
  bool showControlPanel() {
    if (!driver_) return false;
    return driver_->controlPanel() == kAsioOk;
  }

  void stop() {
    if (driver_) {
      if (running_) driver_->stop();
      if (buffersMade_) driver_->disposeBuffers();
      driver_->Release();
      driver_ = nullptr;
    }
    running_ = false;
    buffersMade_ = false;
    instance_ = nullptr;
    if (comInitialised_) {
      CoUninitialize();
      comInitialised_ = false;
    }
  }

private:
  bool fail() {
    if (driver_) {
      driver_->Release();
      driver_ = nullptr;
    }
    return false;
  }

  /** [audio-thread] The driver wants the next block. */
  static void onBufferSwitch(long index, AsioBool) {
    AsioOutput* self = instance_;
    if (!self || !self->render_) return;
    const uint32_t frames = (uint32_t) self->bufferFrames_;
    for (uint32_t ch = 0; ch < self->inputChannels_; ++ch)
      self->readChannel(self->buffers_[ch].buffers[index], (int) ch, frames);
    self->render_(self->haveInput_ ? self->inputScratch_.data() : nullptr, self->scratch_.data(),
                  frames, 2);
    for (int ch = 0; ch < 2; ++ch)
      self->writeChannel(self->buffers_[self->inputChannels_ + (uint32_t) ch].buffers[index], ch,
                         frames);
    // outputReady tells a driver it may play what we just wrote, and shaves a
    // buffer off the latency on the ones that support it. Drivers that do not
    // return an error, which is not a failure.
    self->driver_->outputReady();
  }

  /**
   * One channel of our interleaved float into the driver's own format.
   *
   * ASIO hands out PER-CHANNEL buffers in whatever the hardware wants, which
   * is usually 32-bit integer -- the float path every other API offers is the
   * exception here, not the rule.
   *
   * Clamped before conversion, not after. A sample above full scale wraps to
   * the opposite sign when it is truncated into an integer, and a wrap is a
   * full-scale click rather than the clipping a listener would forgive.
   */
  void writeChannel(void* destination, int channel, uint32_t frames) {
    if (!destination) return;
    for (uint32_t i = 0; i < frames; ++i)
      writeSample(destination, i, format_, scratch_[(size_t) i * 2 + (size_t) channel]);
  }

  /** One channel of the driver's own format into our interleaved float. The
   *  mirror of writeChannel, and deliberately next to it: two conversions
   *  that must agree about a format are two conversions that should be read
   *  together. */
  void readChannel(const void* source, int channel, uint32_t frames) {
    if (!source) return;
    for (uint32_t i = 0; i < frames; ++i)
      inputScratch_[(size_t) i * 2 + (size_t) channel] = readSample(source, i, inputFormat_);
  }

  static void onSampleRateChanged(AsioSampleRate rate) {
    if (instance_ && rate > 0.0) instance_->sampleRate_ = rate;
  }

  /** The driver asking what we support. Answering honestly matters: a driver
   *  told we handle a reset we do not would hand us one and wait. */
  static long onMessage(long selector, long, void*, double*) {
    switch (selector) {
      case 1: // kAsioSelectorSupported
      case 4: // kAsioEngineVersion
        return 2;
      default:
        return 0;
    }
  }

  IAsio* driver_ = nullptr;
  /** Input pair then output pair, which is the order createBuffers is given
   *  them in and therefore the order the driver hands them back. */
  AsioBufferInfo buffers_[4]{};
  AsioCallbacks callbacks_{};
  std::vector<float> scratch_, inputScratch_;
  RenderFn render_;
  std::string error_, driverName_;
  double sampleRate_ = 48000.0;
  long bufferFrames_ = 0;
  long format_ = kAsioInt32Lsb, inputFormat_ = kAsioInt32Lsb;
  uint32_t inputChannels_ = 0;
  bool wantInput_ = false, haveInput_ = false;
  bool running_ = false, buffersMade_ = false, comInitialised_ = false;

  /** ASIO's callbacks carry no user data, so the instance has to be findable
   *  from a bare function. One driver at a time is the price, and it is a
   *  price the API sets rather than this class. */
  static inline AsioOutput* instance_ = nullptr;
};

} // namespace asio
} // namespace sonore

#endif // _WIN32
