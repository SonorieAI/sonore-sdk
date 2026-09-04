// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: macOS audio output (the default-output AudioUnit).
//
// Compiled on macOS since 2026-09-01: the standalone builds there and its
// offline `--verify` passes under ctest.
//
// The output path on macOS IS an AudioUnit (AUHAL's default output), so this
// file speaks the same AudioComponent API as au_wrapper.h, just from the other
// side: we host the system's output unit and feed it a render callback.
#pragma once

#if defined(__APPLE__) || defined(SONORE_APPLE_SYNTAX_CHECK)

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

// kAudioObjectPropertyElementMain arrived in the macOS 12 SDK; before that the
// same constant was spelled ...Master, and it is still what older SDKs define.
// Naming it once here means the rest of this file reads the modern way and
// still compiles against an older toolchain.
#ifndef kAudioObjectPropertyElementMain
#define kAudioObjectPropertyElementMain kAudioObjectPropertyElementMaster
#endif

namespace sonore {
namespace standalone {

class CoreAudioOutput {
public:
  using RenderFn = std::function<void(float* interleaved, uint32_t frames, uint32_t channels)>;

  ~CoreAudioOutput() { stop(); }

  double sampleRate() const { return sampleRate_; }
  uint32_t channels() const { return channels_; }
  const std::string& error() const { return error_; }
  const std::string& deviceName() const { return deviceName_; }

  /** Which device the next open() will use. -1 (the default) means the one
   *  the system is currently pointing at. Indexes into listDevices(). */
  void setDeviceIndex(int index) { deviceIndex_ = index; }

  /**
   * Every device that can actually play something, in the order --device
   * numbers them.
   *
   * Filtered by OUTPUT stream configuration rather than by name. A Mac has
   * input-only devices, aggregate devices and virtual ones, and listing a
   * microphone as somewhere to send audio is a list that wastes the user's
   * time twice -- once choosing it and once working out why it is silent.
   */
  static std::vector<std::string> listDevices() {
    std::vector<std::string> names;
    for (AudioDeviceID id : outputDevices()) names.push_back(nameOf(id));
    return names;
  }

  /** Negotiate the unit; no audio flows until run(). */
  bool open(RenderFn render) {
    render_ = std::move(render);

    // HALOutput rather than DefaultOutput, so a device can be NAMED. The
    // default unit is the simpler one and follows the system setting on its
    // own, but it offers no way to say "that interface, not the built-in
    // speakers" -- and a standalone whose only output is whatever macOS
    // happens to be pointing at is a standalone people run once.
    //
    // The cost is that the system default has to be resolved by hand, below.
    AudioComponentDescription desc{};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_HALOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;

    AudioComponent component = AudioComponentFindNext(nullptr, &desc);
    if (!component) {
      error_ = "no HAL output unit";
      return false;
    }
    if (AudioComponentInstanceNew(component, &unit_) != noErr || !unit_) {
      error_ = "the output unit would not instantiate";
      return false;
    }

    // The device, BEFORE the format and before initialisation: the unit
    // negotiates its stream format against whatever device it is pointed at,
    // so pointing it afterwards would negotiate against the wrong one.
    const std::vector<AudioDeviceID> devices = outputDevices();
    AudioDeviceID device = kAudioObjectUnknown;
    if (deviceIndex_ >= 0 && (size_t) deviceIndex_ < devices.size()) {
      device = devices[(size_t) deviceIndex_];
    } else {
      device = defaultOutputDevice();
      // An index that is out of range is worth saying out loud rather than
      // silently treating as "the default": a device list changes when
      // something is unplugged, and a remembered number can outlive it.
      if (deviceIndex_ >= 0)
        std::fprintf(stderr,
                     "[sonore] audio device %d no longer exists (%zu present); "
                     "using the system default\n",
                     deviceIndex_, devices.size());
    }
    if (device == kAudioObjectUnknown) {
      error_ = "no output device -- nothing on this Mac can play audio";
      return false;
    }
    if (AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_CurrentDevice,
                             kAudioUnitScope_Global, 0, &device, sizeof(device)) != noErr) {
      error_ = "the output unit refused the chosen device";
      return false;
    }
    deviceName_ = nameOf(device);

    // Interleaved float stereo at the DEVICE's own rate. Asking for our format
    // on the INPUT scope of the output element is how AUHAL is driven.
    //
    // The rate comes from the device rather than the unit because the unit is
    // uninitialised and has not necessarily caught up with the device it was
    // just pointed at. Shared with everything else on the Mac either way: this
    // reads the rate, it never sets it.
    sampleRate_ = nominalSampleRate(device);
    if (sampleRate_ <= 0.0) {
      AudioStreamBasicDescription probe{};
      UInt32 probeSize = sizeof(probe);
      AudioUnitGetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0,
                           &probe, &probeSize);
      sampleRate_ = probe.mSampleRate > 0.0 ? probe.mSampleRate : 48000.0;
    }

    AudioStreamBasicDescription format{};

    std::memset(&format, 0, sizeof(format));
    format.mSampleRate = sampleRate_;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagsNativeFloatPacked; // interleaved
    format.mChannelsPerFrame = channels_;
    format.mFramesPerPacket = 1;
    format.mBitsPerChannel = 32;
    format.mBytesPerFrame = channels_ * sizeof(float);
    format.mBytesPerPacket = channels_ * sizeof(float);
    if (AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0,
                             &format, sizeof(format)) != noErr) {
      error_ = "the output unit refused float stereo";
      return false;
    }

    AURenderCallbackStruct callback{};
    callback.inputProc = &CoreAudioOutput::renderThunk;
    callback.inputProcRefCon = this;
    if (AudioUnitSetProperty(unit_, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input,
                             0, &callback, sizeof(callback)) != noErr) {
      error_ = "the render callback was refused";
      return false;
    }

    return true;
  }

  bool run() {
    if (!unit_) return false;
    if (AudioUnitInitialize(unit_) != noErr || AudioOutputUnitStart(unit_) != noErr) {
      error_ = "the output unit would not start";
      return false;
    }
    running_.store(true, std::memory_order_release);
    return true;
  }

  void stop() {
    if (!unit_) return;
    running_.store(false, std::memory_order_release);
    AudioOutputUnitStop(unit_);
    AudioUnitUninitialize(unit_);
    AudioComponentInstanceDispose(unit_);
    unit_ = nullptr;
  }

private:
  // ── The HAL, asked about devices ──────────────────────────────────────────
  //
  // Four small queries, each written once. CoreAudio's property API is the
  // same three lines every time -- an address, a size, a read -- and the
  // interesting part is which constant goes in which field.

  static AudioObjectPropertyAddress address(AudioObjectPropertySelector selector,
                                            AudioObjectPropertyScope scope
                                                = kAudioObjectPropertyScopeGlobal) {
    AudioObjectPropertyAddress addr{};
    addr.mSelector = selector;
    addr.mScope = scope;
    addr.mElement = kAudioObjectPropertyElementMain;
    return addr;
  }

  /** Every device with at least one output channel, in the HAL's own order --
   *  which is stable for as long as nothing is plugged in or out, and is what
   *  a --device index means. */
  static std::vector<AudioDeviceID> outputDevices() {
    std::vector<AudioDeviceID> playable;
    AudioObjectPropertyAddress addr = address(kAudioHardwarePropertyDevices);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, nullptr, &size)
            != noErr || size == 0)
      return playable;

    std::vector<AudioDeviceID> all(size / sizeof(AudioDeviceID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size,
                                   all.data()) != noErr)
      return playable;

    for (AudioDeviceID id : all)
      if (outputChannels(id) > 0) playable.push_back(id);
    return playable;
  }

  /** How many output channels a device has. Zero means it is a microphone, and
   *  a microphone does not belong in a list of places to send audio. */
  static uint32_t outputChannels(AudioDeviceID id) {
    AudioObjectPropertyAddress addr =
        address(kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(id, &addr, 0, nullptr, &size) != noErr || size == 0)
      return 0;

    // AudioBufferList is variable-length -- one AudioBuffer per stream, tail
    // allocated. A fixed struct on the stack would be read past its end for
    // any device with more than one stream, which an aggregate device has.
    std::vector<unsigned char> storage(size, 0);
    auto* list = (AudioBufferList*) storage.data();
    if (AudioObjectGetPropertyData(id, &addr, 0, nullptr, &size, list) != noErr) return 0;

    uint32_t total = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i) total += list->mBuffers[i].mNumberChannels;
    return total;
  }

  static AudioDeviceID defaultOutputDevice() {
    AudioObjectPropertyAddress addr = address(kAudioHardwarePropertyDefaultOutputDevice);
    AudioDeviceID id = kAudioObjectUnknown;
    UInt32 size = sizeof(id);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, &id)
        != noErr)
      return kAudioObjectUnknown;
    return id;
  }

  static double nominalSampleRate(AudioDeviceID id) {
    AudioObjectPropertyAddress addr = address(kAudioDevicePropertyNominalSampleRate);
    Float64 rate = 0.0;
    UInt32 size = sizeof(rate);
    if (AudioObjectGetPropertyData(id, &addr, 0, nullptr, &size, &rate) != noErr) return 0.0;
    return (double) rate;
  }

  /** A device's name as UTF-8. CoreAudio answers in CFStringRef, which is
   *  reference counted, so this owns the release. */
  static std::string nameOf(AudioDeviceID id) {
    AudioObjectPropertyAddress addr = address(kAudioObjectPropertyName);
    CFStringRef name = nullptr;
    UInt32 size = sizeof(name);
    if (AudioObjectGetPropertyData(id, &addr, 0, nullptr, &size, &name) != noErr || !name)
      return std::string("device ") + std::to_string((unsigned long) id);

    // CFStringGetCStringPtr can return null for a string that is not already
    // in the encoding asked for, so the copy is the path that always works.
    const CFIndex length = CFStringGetLength(name);
    const CFIndex bytes = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    std::vector<char> buffer((size_t) bytes, 0);
    const bool ok = CFStringGetCString(name, buffer.data(), bytes, kCFStringEncodingUTF8);
    CFRelease(name);
    return ok ? std::string(buffer.data())
              : std::string("device ") + std::to_string((unsigned long) id);
  }

  static OSStatus renderThunk(void* refCon, AudioUnitRenderActionFlags*, const AudioTimeStamp*,
                              UInt32, UInt32 frames, AudioBufferList* io) {
    auto* self = (CoreAudioOutput*) refCon;
    if (!io || io->mNumberBuffers < 1 || !io->mBuffers[0].mData) return noErr;
    auto* out = (float*) io->mBuffers[0].mData;
    if (self->running_.load(std::memory_order_acquire)) {
      self->render_(out, frames, self->channels_);
    } else {
      std::memset(out, 0, frames * self->channels_ * sizeof(float));
    }
    return noErr;
  }

  RenderFn render_;
  AudioUnit unit_ = nullptr;
  std::atomic<bool> running_{false};
  double sampleRate_ = 48000.0;
  uint32_t channels_ = 2;
  int deviceIndex_ = -1;
  std::string error_, deviceName_;
};

} // namespace standalone
} // namespace sonore

#endif // __APPLE__
