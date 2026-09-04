// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: macOS audio capture (AUHAL, input scope).
//
// Compiled on macOS since 2026-09-01 (the SDK workflow).
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

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

namespace sonore {
namespace standalone {

class CoreAudioInput {
public:
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

  bool open(int /*deviceIndex*/, double wantedRate) {
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

    sampleRate_ = wantedRate > 0.0 ? wantedRate : 48000.0;
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

    if (AudioUnitInitialize(unit_) != noErr) return fail("the unit would not initialise");

    deviceName_ = "System default input";
    ring_.reset(kBlock * 8);
    scratch_.assign(kBlock * 2, 0.0f);
    return true;
  }

  bool run() {
    if (!unit_) return false;
    if (AudioOutputUnitStart(unit_) != noErr) return fail("the capture stream would not start");
    running_.store(true, std::memory_order_release);
    return true;
  }

  void stop() {
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

  bool fail(const char* what) {
    error_ = what;
    return false;
  }

  static OSStatus inputProc(void* refCon, AudioUnitRenderActionFlags* flags,
                            const AudioTimeStamp* timeStamp, UInt32 bus, UInt32 frames,
                            AudioBufferList*) {
    auto* self = (CoreAudioInput*) refCon;
    if (!self || !self->unit_) return noErr;
    // AUHAL hands the input callback NO buffer: the host provides one and
    // calls AudioUnitRender to pull into it. A callback that reads the
    // AudioBufferList argument reads null.
    if (self->scratch_.size() < (size_t) frames * 2) self->scratch_.assign((size_t) frames * 2, 0.0f);

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
