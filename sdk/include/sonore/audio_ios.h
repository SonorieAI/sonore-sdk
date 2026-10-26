// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: audio in and out on iOS and iPadOS, through RemoteIO.
//
// iOS has one audio unit that is the hardware: RemoteIO, whose bus 0 plays and
// whose bus 1 records. It runs under the app's AVAudioSession, which says what
// kind of audio app this is (playback, or play-and-record), asks for a rate
// and a buffer, and is where the system says the route changed or a phone
// call took the audio away. The two classes here are the standalone's
// PlatformAudio and PlatformAudioInput on iOS, the same surface every other
// backend offers (scripts/backend-surface.mjs checks it), over ONE RemoteIO
// unit both share (RemoteIo below) -- an app with two would have two clocks.
//
// What iOS does differently:
//
//  - No device list. The route is the system's: the speaker, headphones, a
//    USB or Lightning interface, AirPods. RemoteIO follows it by itself.
//  - The rate is the session's. 48 kHz is asked for and usually given; the
//    DSP is prepared at whatever came. The buffer asked for is 5 ms.
//  - Recording needs the user's permission and a usage string in the app's
//    Info.plist (NSMicrophoneUsageDescription; sonore_add_standalone writes
//    one). The input opens only when RemoteIo was opened with it, which the
//    app does once the permission is granted.
//  - An interruption -- a call, an alarm, Siri -- stops the unit. When it
//    ends the session is made active again and the unit restarted. Media
//    services being reset (rare, and drastic) is reported as a lost device,
//    so the app reopens everything.
//
// Written against the Objective-C runtime for the session and the C
// AudioComponent API for the unit, like every Apple file here.
#pragma once

#include "apple_platform.h"

#if defined(SONORE_IOS)

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CFNotificationCenter.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "apple_block.h"
#include "audio_ring.h"

namespace sonore {
namespace standalone {

namespace iosaudio {

template <typename Ret, typename... Args>
inline Ret send(id target, const char* selector, Args... args) {
  using Fn = Ret (*)(id, SEL, Args...);
  return reinterpret_cast<Fn>(objc_msgSend)(target, sel_registerName(selector), args...);
}

/** The AVAudioSession, or nil where AVFAudio is not loaded. */
inline id session() {
  id cls = (id) objc_getClass("AVAudioSession");
  return cls ? send<id>(cls, "sharedInstance") : nil;
}

/** One of AVFAudio's NSString constants, by its symbol. */
inline id constant(const char* symbol) {
  id* p = (id*) dlsym(RTLD_DEFAULT, symbol);
  return p ? *p : nil;
}

inline std::string utf8(id s) {
  if (!s) return std::string();
  char text[256] = {};
  if (CFStringGetCString((CFStringRef) s, text, sizeof(text), kCFStringEncodingUTF8)) return text;
  return std::string();
}

/** AVAudioSessionCategoryOptions. */
enum : unsigned long {
  kOptionMixWithOthers = 0x1,
  kOptionDefaultToSpeaker = 0x8,
  kOptionAllowBluetoothA2DP = 0x20,
  kOptionAllowAirPlay = 0x40,
};

/** The session as this app wants it: play, or play and record; 48 kHz and a
 *  5 ms buffer if the hardware will; active. */
inline bool configure(bool record, std::string* error) {
  id s = session();
  if (!s) {
    if (error) *error = "AVAudioSession is not available";
    return false;
  }
  id category = constant(record ? "AVAudioSessionCategoryPlayAndRecord" : "AVAudioSessionCategoryPlayback");
  id mode = constant("AVAudioSessionModeDefault");
  // Recording: the speaker, not the earpiece a call would use, and Bluetooth
  // headphones for listening. Playing: alongside other apps' audio, as an
  // instrument in a session with a backing track is.
  const unsigned long options =
      record ? (kOptionDefaultToSpeaker | kOptionAllowBluetoothA2DP | kOptionAllowAirPlay) : kOptionMixWithOthers;
  id err = nil;
  if (!send<BOOL>(s, "setCategory:mode:options:error:", category, mode, options, &err)) {
    if (error) *error = "the audio session refused its category";
    return false;
  }
  send<BOOL>(s, "setPreferredSampleRate:error:", 48000.0, (id*) nullptr);
  send<BOOL>(s, "setPreferredIOBufferDuration:error:", 0.005, (id*) nullptr);
  if (!send<BOOL>(s, "setActive:error:", (BOOL) YES, &err)) {
    if (error) *error = "the audio session would not become active";
    return false;
  }
  return true;
}

inline double sampleRate() {
  id s = session();
  const double r = s ? send<double>(s, "sampleRate") : 0.0;
  return r > 0.0 ? r : 48000.0;
}

/** AVAudioSessionRecordPermission: undetermined, denied, granted. */
enum class Permission { Undetermined, Denied, Granted };

inline Permission recordPermission() {
  id s = session();
  if (!s) return Permission::Denied;
  const unsigned long p = send<unsigned long>(s, "recordPermission");
  if (p == 0x67726e74u) return Permission::Granted; // 'grnt'
  if (p == 0x64656e79u) return Permission::Denied;  // 'deny'
  return Permission::Undetermined;                 // 'undt'
}

/** The user's answer, from the system dialog: written by the block, read on
 *  the main thread's next look. */
inline std::atomic<int>& permissionAnswer() {
  static std::atomic<int> answer{-1};
  return answer;
}

using PermissionInvoke = void (*)(void* block, BOOL granted);
inline void permissionAnswered(void*, BOOL granted) { permissionAnswer().store(granted ? 1 : 0); }

/** Ask for the microphone. The dialog shows once per install; the answer
 *  arrives on another thread, in permissionAnswer(). */
inline void requestRecordPermission() {
  static ::sonore::detail::BlockLiteral<int, PermissionInvoke> block{};
  static const bool made = ::sonore::detail::makeGlobalBlock(block, &permissionAnswered, 0);
  id s = session();
  if (s && made) send<void>(s, "requestRecordPermission:", (void*) &block);
}

/**
 * Whether the sound goes where only the user hears it -- headphones, wired or
 * Bluetooth, an interface's outputs -- rather than the built-in speaker, where
 * the microphone would hear it too. AVAudioSessionPortDescription.portType.
 */
inline bool outputIsPrivate() {
  id s = session();
  id route = s ? send<id>(s, "currentRoute") : nil;
  id outputs = route ? send<id>(route, "outputs") : nil;
  const unsigned long n = outputs ? send<unsigned long>(outputs, "count") : 0;
  for (unsigned long i = 0; i < n; ++i) {
    id port = send<id>(outputs, "objectAtIndex:", i);
    const std::string type = utf8(port ? send<id>(port, "portType") : nil);
    if (type == "Headphones" || type == "BluetoothA2DPOutput" || type == "BluetoothLE" ||
        type == "BluetoothHFP" || type == "USBAudio" || type == "LineOut")
      return true;
  }
  return false;
}

} // namespace iosaudio

/**
 * The one RemoteIO unit, its render callback, and the session around it.
 * Opened by IosAudioOutput; IosAudioInput reads what its bus 1 records.
 */
class RemoteIo {
public:
  using RenderFn = std::function<void(float* interleaved, uint32_t frames, uint32_t channels)>;

  static RemoteIo& instance() {
    static RemoteIo io;
    return io;
  }

  /** Configure the session and the unit; `record` enables bus 1. */
  bool open(RenderFn render, bool record) {
    close();
    error_.clear();
    render_ = std::move(render);
    record_ = record;
    if (!iosaudio::configure(record, &error_)) return false;
    rate_ = iosaudio::sampleRate();

    AudioComponentDescription desc{};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = 0x72696f63; // 'rioc', kAudioUnitSubType_RemoteIO
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    AudioComponent component = AudioComponentFindNext(nullptr, &desc);
    if (!component || AudioComponentInstanceNew(component, &unit_) != noErr || !unit_) {
      unit_ = nullptr;
      error_ = "RemoteIO is not available";
      return false;
    }
    const UInt32 one = 1, zero = 0;
    AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &one, sizeof(one));
    AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1,
                         record ? &one : &zero, sizeof(one));

    // What we hand bus 0 and take from bus 1: interleaved float, stereo out,
    // mono in (an iPhone's microphone; an interface's first input).
    AudioStreamBasicDescription out = format(rate_, 2);
    if (AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &out, sizeof(out)) !=
        noErr) {
      error_ = "RemoteIO refused float stereo output";
      close();
      return false;
    }
    if (record) {
      AudioStreamBasicDescription in = format(rate_, 1);
      AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &in, sizeof(in));
    }
    const UInt32 maxFrames = kMaxFrames;
    AudioUnitSetProperty(unit_, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFrames,
                         sizeof(maxFrames));
    AURenderCallbackStruct cb{&RemoteIo::renderCallback, this};
    AudioUnitSetProperty(unit_, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof(cb));
    inScratch_.assign(kMaxFrames, 0.0f);
    stereo_.assign((size_t) kMaxFrames * 2, 0.0f);
    if (AudioUnitInitialize(unit_) != noErr) {
      error_ = "RemoteIO would not initialise";
      close();
      return false;
    }
    watchSession(true);
    return true;
  }

  bool start() {
    if (!unit_) return false;
    if (AudioOutputUnitStart(unit_) != noErr) {
      error_ = "RemoteIO would not start";
      return false;
    }
    running_ = true;
    return true;
  }

  void close() {
    watchSession(false);
    if (unit_) {
      if (running_) AudioOutputUnitStop(unit_);
      AudioUnitUninitialize(unit_);
      AudioComponentInstanceDispose(unit_);
      unit_ = nullptr;
    }
    running_ = false;
    ring_ = nullptr;
  }

  /** Where bus 1's frames go, stereo, while an input is listening. */
  void setInputRing(AudioRing* ring) { ring_.store(ring, std::memory_order_release); }

  bool isOpen() const { return unit_ != nullptr; }
  bool records() const { return unit_ != nullptr && record_; }
  double sampleRate() const { return rate_; }
  const std::string& error() const { return error_; }
  bool takeLost() { return lost_.exchange(false, std::memory_order_acq_rel); }

private:
  static constexpr UInt32 kMaxFrames = 4096;

  static AudioStreamBasicDescription format(double rate, UInt32 channels) {
    AudioStreamBasicDescription f{};
    f.mSampleRate = rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    f.mBytesPerPacket = 4 * channels;
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = 4 * channels;
    f.mChannelsPerFrame = channels;
    f.mBitsPerChannel = 32;
    return f;
  }

  static OSStatus renderCallback(void* refCon, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* ts, UInt32,
                                 UInt32 frames, AudioBufferList* io) {
    auto* self = static_cast<RemoteIo*>(refCon);
    if (frames > kMaxFrames || !io || io->mNumberBuffers < 1 || !io->mBuffers[0].mData) return noErr;
    // The microphone first, so the block that reads it hears this cycle's.
    AudioRing* ring = self->ring_.load(std::memory_order_acquire);
    if (self->record_ && ring) {
      AudioBufferList in{};
      in.mNumberBuffers = 1;
      in.mBuffers[0].mNumberChannels = 1;
      in.mBuffers[0].mDataByteSize = frames * (UInt32) sizeof(float);
      in.mBuffers[0].mData = self->inScratch_.data();
      if (AudioUnitRender(self->unit_, flags, ts, 1, frames, &in) == noErr) {
        const float* mono = (const float*) in.mBuffers[0].mData;
        for (UInt32 i = 0; i < frames; ++i) self->stereo_[2 * i] = self->stereo_[2 * i + 1] = mono[i];
        ring->write(self->stereo_.data(), frames);
      }
    }
    auto* out = (float*) io->mBuffers[0].mData;
    if (self->render_) self->render_(out, frames, 2);
    else std::memset(out, 0, sizeof(float) * frames * 2);
    return noErr;
  }

  // ── The session's news ──

  void watchSession(bool on) {
    CFNotificationCenterRef center = CFNotificationCenterGetLocalCenter();
    if (!center) return;
    if (on && !watching_) {
      CFNotificationCenterAddObserver(center, this, &RemoteIo::sessionNotice,
                                      CFSTR("AVAudioSessionInterruptionNotification"), nullptr,
                                      CFNotificationSuspensionBehaviorDeliverImmediately);
      CFNotificationCenterAddObserver(center, this, &RemoteIo::sessionNotice,
                                      CFSTR("AVAudioSessionMediaServicesWereResetNotification"), nullptr,
                                      CFNotificationSuspensionBehaviorDeliverImmediately);
      watching_ = true;
    } else if (!on && watching_) {
      CFNotificationCenterRemoveEveryObserver(center, this);
      watching_ = false;
    }
  }

  /** An interruption's end restarts the unit; a media-services reset is a
   *  lost device. On the thread that posted it, which for the session is the
   *  main one. */
  static void sessionNotice(CFNotificationCenterRef, void* observer, CFNotificationName name, const void*,
                            CFDictionaryRef info) {
    auto* self = static_cast<RemoteIo*>(observer);
    char text[96] = {};
    if (!name || !CFStringGetCString(name, text, sizeof(text), kCFStringEncodingUTF8)) return;
    if (std::strcmp(text, "AVAudioSessionMediaServicesWereResetNotification") == 0) {
      self->lost_.store(true, std::memory_order_release);
      return;
    }
    // AVAudioSessionInterruptionTypeKey: 1 began, 0 ended.
    long type = 1;
    if (info) {
      auto value = (CFNumberRef) CFDictionaryGetValue(info, CFSTR("AVAudioSessionInterruptionTypeKey"));
      if (value && CFGetTypeID(value) == CFNumberGetTypeID()) CFNumberGetValue(value, kCFNumberLongType, &type);
    }
    if (type == 0 && self->unit_ && self->running_) {
      iosaudio::configure(self->record_, nullptr);
      AudioOutputUnitStart(self->unit_);
    }
  }

  AudioComponentInstance unit_ = nullptr;
  RenderFn render_;
  bool record_ = false;
  bool running_ = false;
  bool watching_ = false;
  double rate_ = 48000.0;
  std::atomic<AudioRing*> ring_{nullptr};
  std::atomic<bool> lost_{false};
  std::vector<float> inScratch_, stereo_;
  std::string error_;
};

/** The system output on iOS: RemoteIO's bus 0. */
class IosAudioOutput {
public:
  using RenderFn = RemoteIo::RenderFn;

  ~IosAudioOutput() { stop(); }

  static std::vector<std::string> listDevices() { return {"System output"}; }
  void setDeviceIndex(int) {}

  /** Recording is opened with the output when the user has already allowed
   *  it (setRecording): one unit for both, as RemoteIO is. */
  void setRecording(bool record) { record_ = record; }

  bool open(RenderFn render) {
    RemoteIo& io = RemoteIo::instance();
    if (!io.open(std::move(render), record_)) {
      error_ = io.error();
      return false;
    }
    return true;
  }
  bool run() {
    RemoteIo& io = RemoteIo::instance();
    if (!io.start()) {
      error_ = io.error();
      return false;
    }
    return true;
  }
  void stop() { RemoteIo::instance().close(); }
  bool takeDeviceLost() { return RemoteIo::instance().takeLost(); }
  double sampleRate() const { return RemoteIo::instance().sampleRate(); }
  uint32_t channels() const { return 2; }
  const std::string& deviceName() const { return name_; }
  const std::string& error() const { return error_; }

private:
  bool record_ = false;
  std::string name_ = "System output";
  std::string error_;
};

/** The system input on iOS: what RemoteIO's bus 1 records, into a ring. */
class IosAudioInput {
public:
  ~IosAudioInput() { stop(); }

  static std::vector<std::string> listDevices() { return {"System input"}; }

  bool open(int /*deviceIndex*/, double /*wantedRate*/) {
    stop();
    error_.clear();
    RemoteIo& io = RemoteIo::instance();
    if (!io.records()) {
      error_ = "the audio was opened without the microphone -- has the app been allowed to record?";
      return false;
    }
    ring_.reset(4096 * 4);
    ring_.setPrime(512);
    return true;
  }
  bool run() {
    if (!RemoteIo::instance().records()) return false;
    RemoteIo::instance().setInputRing(&ring_);
    listening_ = true;
    return true;
  }
  void stop() {
    if (listening_) RemoteIo::instance().setInputRing(nullptr);
    listening_ = false;
  }
  bool takeDeviceLost() { return false; } // the output reports the unit's loss
  void read(float* left, float* right, uint32_t frames) { ring_.read(left, right, frames); }
  double sampleRate() const { return RemoteIo::instance().sampleRate(); }
  uint64_t droppedFrames() const { return ring_.droppedFrames(); }
  uint64_t starvedFrames() const { return ring_.starvedFrames(); }
  size_t availableFrames() const { return ring_.readable(); }
  const std::string& deviceName() const { return name_; }
  const std::string& error() const { return error_; }

private:
  AudioRing ring_;
  bool listening_ = false;
  std::string name_ = "System input";
  std::string error_;
};

} // namespace standalone
} // namespace sonore

#endif // SONORE_IOS
