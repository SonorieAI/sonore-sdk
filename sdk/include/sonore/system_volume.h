// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: SystemAudioVolume: the machine's master output level and mute.
//
// The volume and mute of the default output device -- the ones the speaker
// icon in the taskbar or the menu bar changes. Windows reaches them through
// the default render endpoint's IAudioEndpointVolume, macOS through the
// default output device's virtual main volume (the property the volume keys
// drive), Linux through ALSA's "Master" mixer control, falling back to "PCM".
//
// ── Whose setting this is ───────────────────────────────────────────────────
//
// It is the USER's, for everything on the machine, which is why nothing in
// this SDK calls the setters on its own. They are here for an application
// that is asked to -- a practice tool with its own volume keys, a kiosk -- and
// a plugin has no business reaching for them. Reading is harmless; a meter
// that wants to warn "your system volume is muted" is the honest use.
//
// ── Gains are the OS's own scale ────────────────────────────────────────────
//
// 0..1 as each system defines it: Windows and macOS hand out an audio-tapered
// scalar (what their sliders show), ALSA a raw control range mapped linearly
// (what alsamixer's raw view shows). None of them is decibels, and the three
// do not agree with each other at 0.5 -- converting would need each device's
// own curve, which only Windows publishes.
//
// ── Linux ───────────────────────────────────────────────────────────────────
//
// libasound is dlopened, exactly as the ALSA audio backend does it: the SDK
// builds without ALSA's development package, and a machine without ALSA says
// isAvailable() == false instead of refusing to load the plugin. A PipeWire or
// PulseAudio desktop routes "default" through its own ALSA plugin, whose
// Master control is the desktop's volume.
//
// Every call opens the device, does one thing and closes it: these are rare
// calls from a UI, and holding a mixer or an endpoint open between them would
// pin a device the user may unplug. Never from the audio thread.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
// Before the first Windows header: mmdeviceapi.h includes windows.h itself.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include "win_com.h"
#elif defined(__APPLE__)
#include <CoreAudio/CoreAudio.h>
#elif defined(__linux__)
#include <dlfcn.h>
#endif

namespace sonore {
namespace sysvoldetail {

#if defined(_WIN32)

/** The default render endpoint's volume interface, for the length of one
 *  call. eConsole, as the WASAPI backend opens it: the device the speaker
 *  icon controls. */
template <class Fn>
bool withEndpointVolume(Fn&& fn) {
  comdetail::Apartment com;
  if (!com.usable()) return false;
  comdetail::Ref<IMMDeviceEnumerator> enumerator;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                              __uuidof(IMMDeviceEnumerator), enumerator.putVoid())) ||
      !enumerator)
    return false;
  comdetail::Ref<IMMDevice> device;
  if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.put())) || !device)
    return false;
  comdetail::Ref<IAudioEndpointVolume> volume;
  if (FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, nullptr,
                              volume.putVoid())) ||
      !volume)
    return false;
  return fn(volume.get());
}

#elif defined(__APPLE__)

// The selectors as numbers, because their NAMES moved: the volume one is
// kAudioHardwareServiceDeviceProperty_VirtualMainVolume from the macOS 12 SDK
// on and ...VirtualMasterVolume before it, and spelling either breaks the
// build (or warns) against the other. The four-character codes never changed.
constexpr AudioObjectPropertySelector kVirtualMainVolume = 0x766D7663; // 'vmvc'
constexpr AudioObjectPropertySelector kDeviceMute = 0x6D757465;        // 'mute'
constexpr AudioObjectPropertyScope kScopeOutput = 0x6F757470;          // 'outp'
constexpr AudioObjectPropertyElement kElementMain = 0;

inline AudioDeviceID defaultOutputDevice() {
  AudioObjectPropertyAddress address = {kAudioHardwarePropertyDefaultOutputDevice,
                                        kAudioObjectPropertyScopeGlobal, kElementMain};
  AudioDeviceID device = kAudioObjectUnknown;
  UInt32 size = (UInt32) sizeof(device);
  if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size,
                                 &device) != noErr)
    return kAudioObjectUnknown;
  return device;
}

inline bool readProperty(AudioObjectPropertySelector selector, void* value, UInt32 size) {
  const AudioDeviceID device = defaultOutputDevice();
  if (device == kAudioObjectUnknown) return false;
  AudioObjectPropertyAddress address = {selector, kScopeOutput, kElementMain};
  if (!AudioObjectHasProperty(device, &address)) return false;
  UInt32 got = size;
  return AudioObjectGetPropertyData(device, &address, 0, nullptr, &got, value) == noErr &&
         got == size;
}

inline bool writeProperty(AudioObjectPropertySelector selector, const void* value, UInt32 size) {
  const AudioDeviceID device = defaultOutputDevice();
  if (device == kAudioObjectUnknown) return false;
  AudioObjectPropertyAddress address = {selector, kScopeOutput, kElementMain};
  Boolean settable = 0;
  // A device with no hardware volume (an HDMI output, some interfaces)
  // has the property but not settable: say so rather than "succeed".
  if (!AudioObjectHasProperty(device, &address) ||
      AudioObjectIsPropertySettable(device, &address, &settable) != noErr || !settable)
    return false;
  return AudioObjectSetPropertyData(device, &address, 0, nullptr, size, value) == noErr;
}

#elif defined(__linux__)

/** The slice of libasound's simple-mixer API this needs, resolved at runtime.
 *  Handles are opaque pointers: the real typedefs live in alsa/asoundlib.h,
 *  which this deliberately does not need. */
struct AlsaMixerApi {
  int (*open)(void** mixer, int mode) = nullptr;
  int (*attach)(void* mixer, const char* name) = nullptr;
  int (*selemRegister)(void* mixer, void* options, void** classp) = nullptr;
  int (*load)(void* mixer) = nullptr;
  int (*close)(void* mixer) = nullptr;
  size_t (*idSizeof)() = nullptr;
  void (*idSetIndex)(void* id, unsigned index) = nullptr;
  void (*idSetName)(void* id, const char* name) = nullptr;
  void* (*findSelem)(void* mixer, const void* id) = nullptr;
  int (*getRange)(void* elem, long* lo, long* hi) = nullptr;
  int (*getVolume)(void* elem, int channel, long* value) = nullptr;
  int (*setVolumeAll)(void* elem, long value) = nullptr;
  int (*hasSwitch)(void* elem) = nullptr;
  int (*getSwitch)(void* elem, int channel, int* value) = nullptr;
  int (*setSwitchAll)(void* elem, int value) = nullptr;

  bool loaded() const {
    return open && attach && selemRegister && load && close && idSizeof && idSetIndex &&
           idSetName && findSelem && getRange && getVolume && setVolumeAll && hasSwitch &&
           getSwitch && setSwitchAll;
  }
};

template <class T>
void bindSymbol(void* lib, T& slot, const char* name) {
  slot = reinterpret_cast<T>(dlsym(lib, name));
}

inline const AlsaMixerApi& alsaMixerApi() {
  static const AlsaMixerApi api = [] {
    AlsaMixerApi a;
    // Versioned soname only -- the unversioned .so lives in dev packages.
    void* lib = dlopen("libasound.so.2", RTLD_LAZY);
    if (!lib) return a;
    bindSymbol(lib, a.open, "snd_mixer_open");
    bindSymbol(lib, a.attach, "snd_mixer_attach");
    bindSymbol(lib, a.selemRegister, "snd_mixer_selem_register");
    bindSymbol(lib, a.load, "snd_mixer_load");
    bindSymbol(lib, a.close, "snd_mixer_close");
    bindSymbol(lib, a.idSizeof, "snd_mixer_selem_id_sizeof");
    bindSymbol(lib, a.idSetIndex, "snd_mixer_selem_id_set_index");
    bindSymbol(lib, a.idSetName, "snd_mixer_selem_id_set_name");
    bindSymbol(lib, a.findSelem, "snd_mixer_find_selem");
    bindSymbol(lib, a.getRange, "snd_mixer_selem_get_playback_volume_range");
    bindSymbol(lib, a.getVolume, "snd_mixer_selem_get_playback_volume");
    bindSymbol(lib, a.setVolumeAll, "snd_mixer_selem_set_playback_volume_all");
    bindSymbol(lib, a.hasSwitch, "snd_mixer_selem_has_playback_switch");
    bindSymbol(lib, a.getSwitch, "snd_mixer_selem_get_playback_switch");
    bindSymbol(lib, a.setSwitchAll, "snd_mixer_selem_set_playback_switch_all");
    return a;
  }();
  return api;
}

/** Open "default", find its Master control (or PCM, which is what some
 *  cards and every dmix-only setup have instead), run `fn`, close. */
template <class Fn>
bool withMasterControl(Fn&& fn) {
  const AlsaMixerApi& api = alsaMixerApi();
  if (!api.loaded()) return false;
  void* mixer = nullptr;
  if (api.open(&mixer, 0) < 0 || !mixer) return false;
  bool ok = false;
  void* id = std::calloc(1, api.idSizeof());
  if (id && api.attach(mixer, "default") >= 0 && api.selemRegister(mixer, nullptr, nullptr) >= 0 &&
      api.load(mixer) >= 0) {
    const char* const names[] = {"Master", "PCM"};
    for (const char* name : names) {
      api.idSetIndex(id, 0);
      api.idSetName(id, name);
      if (void* elem = api.findSelem(mixer, id)) {
        ok = fn(api, elem);
        break;
      }
    }
  }
  std::free(id);
  api.close(mixer);
  return ok;
}

#endif

} // namespace sysvoldetail

/**
 * The system's master output gain and mute. All static, as the reference's:
 * there is one master volume per machine.
 */
class SystemAudioVolume {
public:
  /** There is a default output whose volume this platform lets us reach. */
  static bool isAvailable() { return getGain() >= 0.0f; }

  /** 0..1 on the OS's own scale, or -1 when it cannot be read. */
  static float getGain() {
#if defined(_WIN32)
    float level = -1.0f;
    if (!sysvoldetail::withEndpointVolume([&](IAudioEndpointVolume* v) {
          return SUCCEEDED(v->GetMasterVolumeLevelScalar(&level));
        }))
      return -1.0f;
    return level;
#elif defined(__APPLE__)
    Float32 level = 0.0f;
    if (!sysvoldetail::readProperty(sysvoldetail::kVirtualMainVolume, &level,
                                    (UInt32) sizeof(level)))
      return -1.0f;
    return (float) level;
#elif defined(__linux__)
    float level = -1.0f;
    sysvoldetail::withMasterControl([&](const sysvoldetail::AlsaMixerApi& api, void* elem) {
      long lo = 0, hi = 0, value = 0;
      if (api.getRange(elem, &lo, &hi) < 0 || hi <= lo) return false;
      if (api.getVolume(elem, 0 /* front left */, &value) < 0) return false;
      level = (float) ((double) (value - lo) / (double) (hi - lo));
      return true;
    });
    return level;
#else
    return -1.0f;
#endif
  }

  /** Set the gain, clamped to 0..1. False when the device does not let it be
   *  set -- no hardware volume, no device, no permission. */
  static bool setGain(float gain) {
    if (!(gain >= 0.0f)) gain = 0.0f; // NaN too
    if (gain > 1.0f) gain = 1.0f;
#if defined(_WIN32)
    return sysvoldetail::withEndpointVolume([&](IAudioEndpointVolume* v) {
      return SUCCEEDED(v->SetMasterVolumeLevelScalar(gain, nullptr));
    });
#elif defined(__APPLE__)
    const Float32 level = gain;
    return sysvoldetail::writeProperty(sysvoldetail::kVirtualMainVolume, &level,
                                       (UInt32) sizeof(level));
#elif defined(__linux__)
    return sysvoldetail::withMasterControl([&](const sysvoldetail::AlsaMixerApi& api, void* elem) {
      long lo = 0, hi = 0;
      if (api.getRange(elem, &lo, &hi) < 0 || hi <= lo) return false;
      const long value = lo + (long) ((double) gain * (double) (hi - lo) + 0.5);
      return api.setVolumeAll(elem, value) >= 0;
    });
#else
    (void) gain;
    return false;
#endif
  }

  /** Muted, or false when it cannot be read (see isAvailable). */
  static bool isMuted() {
#if defined(_WIN32)
    BOOL muted = FALSE;
    if (!sysvoldetail::withEndpointVolume(
            [&](IAudioEndpointVolume* v) { return SUCCEEDED(v->GetMute(&muted)); }))
      return false;
    return muted != FALSE;
#elif defined(__APPLE__)
    UInt32 muted = 0;
    if (!sysvoldetail::readProperty(sysvoldetail::kDeviceMute, &muted, (UInt32) sizeof(muted)))
      return false;
    return muted != 0;
#elif defined(__linux__)
    bool muted = false;
    sysvoldetail::withMasterControl([&](const sysvoldetail::AlsaMixerApi& api, void* elem) {
      int on = 1;
      if (!api.hasSwitch(elem) || api.getSwitch(elem, 0, &on) < 0) return false;
      muted = on == 0; // ALSA's switch is "playback on", the inverse of mute
      return true;
    });
    return muted;
#else
    return false;
#endif
  }

  /** Mute or unmute the default output. False when the device does not let
   *  it be set. */
  static bool setMuted(bool shouldMute) {
#if defined(_WIN32)
    return sysvoldetail::withEndpointVolume([&](IAudioEndpointVolume* v) {
      return SUCCEEDED(v->SetMute(shouldMute ? TRUE : FALSE, nullptr));
    });
#elif defined(__APPLE__)
    const UInt32 muted = shouldMute ? 1u : 0u;
    return sysvoldetail::writeProperty(sysvoldetail::kDeviceMute, &muted, (UInt32) sizeof(muted));
#elif defined(__linux__)
    return sysvoldetail::withMasterControl([&](const sysvoldetail::AlsaMixerApi& api, void* elem) {
      return api.hasSwitch(elem) && api.setSwitchAll(elem, shouldMute ? 0 : 1) >= 0;
    });
#else
    (void) shouldMute;
    return false;
#endif
  }
};

} // namespace sonore
