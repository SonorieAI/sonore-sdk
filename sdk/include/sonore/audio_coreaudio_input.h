// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: macOS audio capture (AUHAL, input scope).
//
// Compiled on macOS since 2026-09-01 (the SDK workflow), but never RUN: no
// ctest opens a capture device and a CI runner has none. A human hearing
// themselves through a plugin is the moment this becomes real. Nothing here
// is claimed to work; it is claimed to be written and to compile.
//
// AUHAL is one unit with two halves and a flag for each. Capture means
// enabling element 1 (input) and DISABLING element 0 (output) on the same
// unit, which is the part of this API that catches everyone: leaving output
// enabled gives a unit that renders silence to the speakers and never calls
// the input callback.
#pragma once

#if defined(__APPLE__) || defined(SONORE_APPLE_SYNTAX_CHECK)

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>

// The macOS 12 SDK's spelling, from the older one where it is missing -- as
// audio_coreaudio.h does, by the SDK's version; whichever comes first defines it.
#if !defined(MAC_OS_VERSION_12_0) && !defined(kAudioObjectPropertyElementMain)
#define kAudioObjectPropertyElementMain kAudioObjectPropertyElementMaster
#endif

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace sonore {
namespace standalone {

class CoreAudioInput {
public:
  /** True once since the input device died -- a USB microphone unplugged,
   *  its driver gone. Polled by the standalone, which then closes the input
   *  rather than keep reading a ring nothing will fill again. */
  bool takeDeviceLost() { return lost_.exchange(false, std::memory_order_acq_rel); }

  ~CoreAudioInput() { stop(); }

  /** One entry. Enumerating real devices means AudioObjectGetPropertyData
   *  over kAudioHardwarePropertyDevices and a second pass for names, and
   *  listing devices this build has never once opened would be inventing a
   *  menu. The default input is what the OS sound panel points at. */
  static std::vector<std::string> listDevices() { return {std::string("System default input")}; }

  double sampleRate() const { return sampleRate_; }
  const std::string& deviceName() const { return deviceName_; }
  const std::string& error() const { return error_; }
  uint64_t droppedFrames() const { return ring_.droppedFrames(); }
  uint64_t starvedFrames() const { return ring_.starvedFrames(); }
  /** Frames read() would deliver now -- none while the cushion is filling:
   *  what a reader paced by the device, rather than by the computer's clock,
   *  waits for. [reader] */
  size_t availableFrames() const { return ring_.readable(); }

  bool open(int /*deviceIndex*/, double wantedRate) {
    // Whatever was open goes first, as the WASAPI input's open() does: a
    // second open() leaked the first unit and reset the ring under its
    // running callback. And the last failure is not this one's.
    stop();
    error_.clear();
    AudioComponentDescription desc{};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_HALOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;

    AudioComponent component = AudioComponentFindNext(nullptr, &desc);
    if (!component) return fail("no HAL output component");
    if (AudioComponentInstanceNew(component, &unit_) != noErr || !unit_)
      return fail("the HAL unit would not instantiate");

    // Input on, output OFF. Both, in that order, before anything else is set:
    // the unit's format properties are only valid once the scopes it will
    // actually use are decided.
    UInt32 on = 1, off = 0;
    if (AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1,
                             &on, sizeof(on)) != noErr)
      return fail("the unit would not enable input");
    if (AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0,
                             &off, sizeof(off)) != noErr)
      return fail("the unit would not disable output");

    // Pointed at the INPUT device. AUHAL starts out on the default OUTPUT
    // device (Apple TN2091), and on a Mac whose microphone and speakers are
    // separate devices -- every current Mac -- a unit left there has no input
    // to enable: the format below was refused, or the callback never came.
    AudioDeviceID input = kAudioObjectUnknown;
    UInt32 inputSize = sizeof(input);
    AudioObjectPropertyAddress defaultInput{};
    defaultInput.mSelector = kAudioHardwarePropertyDefaultInputDevice;
    defaultInput.mScope = kAudioObjectPropertyScopeGlobal;
    defaultInput.mElement = kAudioObjectPropertyElementMain;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &defaultInput, 0, nullptr,
                                   &inputSize, &input) != noErr ||
        input == kAudioObjectUnknown)
      return fail("no input device -- nothing on this Mac can record");
    if (AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_CurrentDevice,
                             kAudioUnitScope_Global, 0, &input, sizeof(input)) != noErr)
      return fail("the unit would not take the input device");
    device_ = input;

    // At the session's rate. AUHAL converts format on its input side but NOT
    // rate (TN2091), so a microphone at 44.1k under a 48k session had the
    // format below refused, with no word of why. The device is asked to run
    // at the session's rate -- what Audio MIDI Setup and every DAW do -- and
    // when it will not, the two rates are named, as on Windows.
    sampleRate_ = wantedRate > 0.0 ? wantedRate : 48000.0;
    double deviceRate = 0.0;
    if (!matchRate(input, sampleRate_, deviceRate)) {
      char buffer[200];
      std::snprintf(buffer, sizeof(buffer),
                    "the input runs at %.0f Hz and the output at %.0f Hz, and macOS would not "
                    "move the input to the output's rate",
                    deviceRate, sampleRate_);
      return fail(buffer);
    }
    AudioStreamBasicDescription format{};
    format.mSampleRate = sampleRate_;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    format.mChannelsPerFrame = 2;
    format.mBitsPerChannel = 32;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = 8;
    format.mBytesPerPacket = 8;
    // Scope OUTPUT, element 1: the side of the input element that faces US.
    // Setting the input scope of element 1 would be telling the microphone
    // what format to be, which is not ours to say.
    if (AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1,
                             &format, sizeof(format)) != noErr)
      return fail("the unit refused float stereo capture");

    AURenderCallbackStruct callback{};
    callback.inputProc = inputProc;
    callback.inputProcRefCon = this;
    if (AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_SetInputCallback,
                             kAudioUnitScope_Global, 0, &callback, sizeof(callback)) != noErr)
      return fail("the unit would not take an input callback");

    // The largest block the callback may be asked for, set before
    // initialising and read back after: the scratch the callback renders
    // into is sized from it HERE, because the IO buffer is the device's and
    // any other app can raise it -- a DAW running at 2048 -- and growing the
    // scratch in the callback was an allocation on the realtime thread.
    UInt32 maxFrames = kMaxSlice;
    AudioUnitSetProperty(unit_, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global,
                         0, &maxFrames, sizeof(maxFrames));
    if (AudioUnitInitialize(unit_) != noErr) return fail("the unit would not initialise");
    UInt32 size = sizeof(maxFrames);
    if (AudioUnitGetProperty(unit_, kAudioUnitProperty_MaximumFramesPerSlice,
                             kAudioUnitScope_Global, 0, &maxFrames, &size) != noErr ||
        maxFrames == 0)
      maxFrames = kMaxSlice;

    deviceName_ = "System default input";
    // Room for several of the largest blocks, so a reader a block behind
    // does not lose the newest one.
    ring_.reset(maxFrames * 4 > kBlock * 8 ? maxFrames * 4 : kBlock * 8);
    // The cushion the WASAPI input has, for the same two clocks: two of the
    // device's IO buffers before the first read, and again after a short
    // one. Two of the LARGEST slices it was -- 8192 frames, 170 ms of
    // monitoring latency at 48 kHz, for a device delivering 512 at a time.
    UInt32 ioFrames = 0;
    AudioObjectPropertyAddress bufferSize{};
    bufferSize.mSelector = kAudioDevicePropertyBufferFrameSize;
    bufferSize.mScope = kAudioObjectPropertyScopeGlobal;
    bufferSize.mElement = kAudioObjectPropertyElementMain;
    size = sizeof(ioFrames);
    if (AudioObjectGetPropertyData(device_, &bufferSize, 0, nullptr, &size, &ioFrames) != noErr)
      ioFrames = 0;
    size_t cushion = (size_t) ioFrames * 2;
    if (cushion < kBlock * 2) cushion = kBlock * 2;
    if (cushion > (size_t) maxFrames * 2) cushion = (size_t) maxFrames * 2;
    ring_.setPrime(cushion);
    scratch_.assign((size_t) maxFrames * 2, 0.0f);
    return true;
  }

  bool run() {
    if (!unit_) return false;
    if (AudioOutputUnitStart(unit_) != noErr) return fail("the capture stream would not start");
    running_.store(true, std::memory_order_release);
    lost_.store(false, std::memory_order_relaxed);
    const AudioObjectPropertyAddress alive = aliveAddress();
    listening_ = device_ != kAudioObjectUnknown &&
                 AudioObjectAddPropertyListener(device_, &alive, &aliveChanged, this) == noErr;
    return true;
  }

  void stop() {
    if (listening_) {
      const AudioObjectPropertyAddress alive = aliveAddress();
      AudioObjectRemovePropertyListener(device_, &alive, &aliveChanged, this);
      listening_ = false;
    }
    if (running_.exchange(false) && unit_) AudioOutputUnitStop(unit_);
    if (unit_) {
      AudioUnitUninitialize(unit_);
      AudioComponentInstanceDispose(unit_);
      unit_ = nullptr;
    }
  }

  void read(float* left, float* right, uint32_t frames) { ring_.read(left, right, frames); }

private:
  static constexpr uint32_t kBlock = 512;
  /** What is asked for as the largest slice. AUHAL keeps to it, and the
   *  device's own buffer setting is normally far below. */
  static constexpr UInt32 kMaxSlice = 4096;

  bool fail(const char* what) {
    error_ = what;
    return false;
  }

  static AudioObjectPropertyAddress aliveAddress() {
    AudioObjectPropertyAddress addr{};
    addr.mSelector = kAudioDevicePropertyDeviceIsAlive;
    addr.mScope = kAudioObjectPropertyScopeGlobal;
    addr.mElement = kAudioObjectPropertyElementMain;
    return addr;
  }

  /** On a HAL notification thread: read whether the device still lives, and
   *  say so if it does not. Nothing else may happen here. */
  static OSStatus aliveChanged(AudioObjectID id, UInt32, const AudioObjectPropertyAddress*,
                               void* client) {
    UInt32 alive = 1, size = sizeof(alive);
    const AudioObjectPropertyAddress addr = aliveAddress();
    if (AudioObjectGetPropertyData(id, &addr, 0, nullptr, &size, &alive) != noErr || !alive)
      ((CoreAudioInput*) client)->lost_.store(true, std::memory_order_release);
    return noErr;
  }

  static AudioObjectPropertyAddress rateAddress() {
    AudioObjectPropertyAddress addr{};
    addr.mSelector = kAudioDevicePropertyNominalSampleRate;
    addr.mScope = kAudioObjectPropertyScopeGlobal;
    addr.mElement = kAudioObjectPropertyElementMain;
    return addr;
  }

  static double nominalRate(AudioDeviceID id) {
    const AudioObjectPropertyAddress addr = rateAddress();
    Float64 rate = 0.0;
    UInt32 size = sizeof(rate);
    if (AudioObjectGetPropertyData(id, &addr, 0, nullptr, &size, &rate) != noErr) return 0.0;
    return (double) rate;
  }

  /** True when the device runs at `rate`, after asking it to if it did not;
   *  `now` is left at the rate it runs at. The HAL applies a new rate on its
   *  own time and says so by notification. This runs on the thread opening
   *  the device, never the audio one, so it polls briefly instead. A device
   *  whose rate cannot be read is let through: the format call judges it. */
  static bool matchRate(AudioDeviceID id, double rate, double& now) {
    now = nominalRate(id);
    if (now <= 0.0 || std::fabs(now - rate) < 1.0) return true;
    const AudioObjectPropertyAddress addr = rateAddress();
    const Float64 wanted = rate;
    if (AudioObjectSetPropertyData(id, &addr, 0, nullptr, sizeof(wanted), &wanted) != noErr)
      return false;
    for (int tries = 0; tries < 50; ++tries) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      now = nominalRate(id);
      if (std::fabs(now - rate) < 1.0) return true;
    }
    return false;
  }

  static OSStatus inputProc(void* refCon, AudioUnitRenderActionFlags* flags,
                            const AudioTimeStamp* timeStamp, UInt32 bus, UInt32 frames,
                            AudioBufferList*) {
    auto* self = (CoreAudioInput*) refCon;
    if (!self || !self->unit_) return noErr;
    // AUHAL hands the input callback NO buffer: the host provides one and
    // calls AudioUnitRender to pull into it. A callback that reads the
    // AudioBufferList argument reads null. The scratch was sized in open()
    // for the largest slice the unit allows; a block past it is not rendered
    // -- the reader hears it as starvation -- rather than grown into here.
    if (self->scratch_.size() < (size_t) frames * 2) return noErr;

    AudioBufferList list{};
    list.mNumberBuffers = 1;
    list.mBuffers[0].mNumberChannels = 2;
    list.mBuffers[0].mDataByteSize = frames * 2 * sizeof(float);
    list.mBuffers[0].mData = self->scratch_.data();

    const OSStatus status =
        AudioUnitRender(self->unit_, flags, timeStamp, bus, frames, &list);
    if (status == noErr) self->ring_.write(self->scratch_.data(), frames);
    return noErr;
  }

  AudioUnit unit_ = nullptr;
  AudioDeviceID device_ = kAudioObjectUnknown;
  bool listening_ = false;
  std::atomic<bool> lost_{false};
  std::atomic<bool> running_{false};
  AudioRing ring_;
  std::vector<float> scratch_;
  double sampleRate_ = 0.0;
  std::string deviceName_, error_;
};

using PlatformAudioInput = CoreAudioInput;

} // namespace standalone
} // namespace sonore

#endif // __APPLE__
