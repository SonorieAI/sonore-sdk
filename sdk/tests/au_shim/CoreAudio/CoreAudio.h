// SPDX-License-Identifier: Apache-2.0
// A shim of the CoreAudio HAL declarations the macOS device backends use.
// See ../README.md: internal consistency, not ABI correctness.
#pragma once

#include <AudioToolbox/AudioToolbox.h>

typedef UInt32 AudioObjectID;
typedef AudioObjectID AudioDeviceID;
typedef UInt32 AudioObjectPropertySelector;
typedef UInt32 AudioObjectPropertyScope;
typedef UInt32 AudioObjectPropertyElement;

struct AudioObjectPropertyAddress {
  AudioObjectPropertySelector mSelector;
  AudioObjectPropertyScope mScope;
  AudioObjectPropertyElement mElement;
};

enum {
  kAudioObjectSystemObject = 1,
  kAudioObjectUnknown = 0,
};

enum {
  kAudioObjectPropertyScopeGlobal = 0x676C6F62,
  kAudioObjectPropertyScopeInput = 0x696E7074,
  kAudioObjectPropertyScopeOutput = 0x6F757470,
  // Master is the older spelling; Main arrived in the macOS 12 SDK and the
  // backends define one from the other when it is missing.
  kAudioObjectPropertyElementMaster = 0,
  kAudioObjectPropertyName = 0x6C6E616D,
};

enum {
  kAudioHardwarePropertyDevices = 0x64657623,
  kAudioHardwarePropertyDefaultOutputDevice = 0x644F7574,
  kAudioHardwarePropertyDefaultInputDevice = 0x64496E20,
  kAudioDevicePropertyStreamConfiguration = 0x736C6179,
  kAudioDevicePropertyNominalSampleRate = 0x6E737274,
  kAudioDevicePropertyBufferFrameSize = 0x6673697A,
  kAudioDevicePropertyDeviceIsAlive = 0x6C69766E,
  kAudioDevicePropertyIOThreadOSWorkgroup = 0x6F737767, // oswg, macOS 11
};

typedef OSStatus (*AudioObjectPropertyListenerProc)(AudioObjectID, UInt32,
                                                    const AudioObjectPropertyAddress*, void*);
OSStatus AudioObjectAddPropertyListener(AudioObjectID, const AudioObjectPropertyAddress*,
                                        AudioObjectPropertyListenerProc, void*);
OSStatus AudioObjectRemovePropertyListener(AudioObjectID, const AudioObjectPropertyAddress*,
                                           AudioObjectPropertyListenerProc, void*);

OSStatus AudioObjectGetPropertyData(AudioObjectID, const AudioObjectPropertyAddress*, UInt32,
                                    const void*, UInt32*, void*);
OSStatus AudioObjectGetPropertyDataSize(AudioObjectID, const AudioObjectPropertyAddress*, UInt32,
                                        const void*, UInt32*);
OSStatus AudioObjectSetPropertyData(AudioObjectID, const AudioObjectPropertyAddress*, UInt32,
                                    const void*, UInt32, const void*);

// The default output's volume and mute (system_volume.h).
Boolean AudioObjectHasProperty(AudioObjectID, const AudioObjectPropertyAddress*);
OSStatus AudioObjectIsPropertySettable(AudioObjectID, const AudioObjectPropertyAddress*, Boolean*);
