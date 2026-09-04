// SPDX-License-Identifier: Apache-2.0
// A shim of the AudioUnit / AudioToolbox declarations au_wrapper.h uses.
//
// Written from Apple's published signatures. See ../README.md: a green
// syntax check here proves the wrapper is internally consistent, NOT that
// these declarations match Apple's. If one of them is wrong, the wrapper
// compiles happily against a fiction.
#pragma once

#include <CoreFoundation/CoreFoundation.h>

// ── Core scalar types ───────────────────────────────────────────────────────
typedef UInt32 AudioUnitPropertyID;
typedef UInt32 AudioUnitScope;
typedef UInt32 AudioUnitElement;
typedef UInt32 AudioUnitParameterID;
typedef Float32 AudioUnitParameterValue;
typedef UInt32 AudioUnitRenderActionFlags;
typedef UInt32 AudioUnitParameterUnit;
typedef UInt32 MusicDeviceInstrumentID;
typedef UInt32 MusicDeviceGroupID;
typedef UInt32 NoteInstanceID;

typedef struct ComponentInstanceRecord* AudioComponentInstance;
typedef AudioComponentInstance AudioUnit;
typedef struct OpaqueAudioComponent* AudioComponent;

// ── Buffers and timing ──────────────────────────────────────────────────────
struct AudioBuffer {
  UInt32 mNumberChannels;
  UInt32 mDataByteSize;
  void* mData;
};

struct AudioBufferList {
  UInt32 mNumberBuffers;
  AudioBuffer mBuffers[1];
};

struct AudioTimeStamp {
  Float64 mSampleTime;
  UInt64 mHostTime;
  Float64 mRateScalar;
  UInt64 mWordClockTime;
  UInt8 mSMPTETime[16];
  UInt32 mFlags;
  UInt32 mReserved;
};

struct AudioStreamBasicDescription {
  Float64 mSampleRate;
  UInt32 mFormatID;
  UInt32 mFormatFlags;
  UInt32 mBytesPerPacket;
  UInt32 mFramesPerPacket;
  UInt32 mBytesPerFrame;
  UInt32 mChannelsPerFrame;
  UInt32 mBitsPerChannel;
  UInt32 mReserved;
};

enum {
  kAudioFormatLinearPCM = 0x6C70636D,
  kAudioFormatFlagIsFloat = 1u << 0,
  kAudioFormatFlagIsPacked = 1u << 3,
  kAudioFormatFlagIsNonInterleaved = 1u << 5,
  kAudioFormatFlagsNativeFloatPacked = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
};

// ── Scopes, selectors and errors ────────────────────────────────────────────
enum {
  kAudioUnitScope_Global = 0,
  kAudioUnitScope_Input = 1,
  kAudioUnitScope_Output = 2,
  kAudioUnitScope_Group = 3,
  kAudioUnitScope_Part = 4,
};

enum {
  kAudioUnitErr_InvalidProperty = -10879,
  kAudioUnitErr_InvalidParameter = -10878,
  kAudioUnitErr_InvalidScope = -10866,
  kAudioUnitErr_InvalidElement = -10877,
  kAudioUnitErr_Uninitialized = -10867,
  kAudioUnitErr_PropertyNotWritable = -10865,
  kAudioUnitErr_InvalidPropertyValue = -10851,
  kAudioUnitErr_FormatNotSupported = -10868,
  kAudioUnitErr_TooManyFramesToProcess = -10874,
};

enum {
  kAudioUnitProperty_ClassInfo = 0,
  kAudioUnitProperty_ElementCount = 11,
  kAudioUnitProperty_Latency = 12,
  kAudioUnitProperty_SupportedNumChannels = 13,
  kAudioUnitProperty_MaximumFramesPerSlice = 14,
  kAudioUnitProperty_ParameterList = 3,
  kAudioUnitProperty_ParameterInfo = 4,
  kAudioUnitProperty_StreamFormat = 8,
  kAudioUnitProperty_SampleRate = 2,
  kAudioUnitProperty_SetRenderCallback = 23,
  kAudioUnitProperty_MakeConnection = 1,
  kAudioUnitProperty_FactoryPresets = 24,
  kAudioUnitProperty_PresentPreset = 36,
  kAudioUnitProperty_TailTime = 20,
  kAudioUnitProperty_BypassEffect = 21,
  kAudioUnitProperty_CocoaUI = 31,
  kAudioUnitProperty_ParameterClumpName = 35,
  kAudioUnitProperty_MIDIOutputCallbackInfo = 47,
  kAudioUnitProperty_MIDIOutputCallback = 48,
};

enum {
  kAudioUnitParameterFlag_CFNameRelease = 1u << 4,
  kAudioUnitParameterFlag_HasClump = 1u << 20,
  kAudioUnitParameterFlag_DisplayLogarithmic = 1u << 22,
  kAudioUnitParameterFlag_IsWritable = 1u << 30,
  kAudioUnitParameterFlag_IsReadable = 1u << 29,
  kAudioUnitParameterFlag_HasCFNameString = 1u << 23,
};

enum {
  kAudioUnitParameterUnit_Generic = 0,
  kAudioUnitParameterUnit_Hertz = 3,
  kAudioUnitParameterUnit_Decibels = 7,
  kAudioUnitParameterUnit_Percent = 13,
  kAudioUnitParameterUnit_Seconds = 14,
  kAudioUnitParameterUnit_Milliseconds = 17,
  kAudioUnitParameterUnit_CustomUnit = 26,
};

enum {
  kAudioUnitRenderAction_PreRender = 1u << 2,
  kAudioUnitRenderAction_PostRender = 1u << 3,
  kAudioUnitRenderAction_OutputIsSilence = 1u << 4
};

enum {
  kAudioUnitInitializeSelect = 0x0001,
  kAudioUnitUninitializeSelect = 0x0002,
  kAudioUnitGetPropertyInfoSelect = 0x0003,
  kAudioUnitGetPropertySelect = 0x0004,
  kAudioUnitSetPropertySelect = 0x0005,
  kAudioUnitAddPropertyListenerSelect = 0x000A,
  kAudioUnitRemovePropertyListenerSelect = 0x000B,
  kAudioUnitRemovePropertyListenerWithUserDataSelect = 0x0012,
  kAudioUnitAddRenderNotifySelect = 0x000F,
  kAudioUnitRemoveRenderNotifySelect = 0x0011,
  kAudioUnitGetParameterSelect = 0x0006,
  kAudioUnitSetParameterSelect = 0x0007,
  kAudioUnitScheduleParametersSelect = 0x0010,
  kAudioUnitRenderSelect = 0x000E,
  kAudioUnitResetSelect = 0x0009,
  kMusicDeviceMIDIEventSelect = 0x0101,
  kMusicDeviceSysExSelect = 0x0102,
  kMusicDeviceStartNoteSelect = 0x0105,
  kMusicDeviceStopNoteSelect = 0x0106,
};

// ── Parameters ──────────────────────────────────────────────────────────────
struct AudioUnitParameterInfo {
  char name[52];
  CFStringRef unitName;
  UInt32 clumpID;
  CFStringRef cfNameString;
  AudioUnitParameterUnit unit;
  AudioUnitParameterValue minValue;
  AudioUnitParameterValue maxValue;
  AudioUnitParameterValue defaultValue;
  UInt32 flags;
};

struct AudioUnitParameterNameInfo {
  AudioUnitParameterID inID;
  SInt32 inDesiredLength;
  CFStringRef outName;
};

struct AudioUnitParameterEvent {
  AudioUnitScope scope;
  AudioUnitElement element;
  AudioUnitParameterID parameter;
  UInt32 eventType;
  union {
    struct {
      SInt32 startBufferOffset;
      UInt32 durationInFrames;
      AudioUnitParameterValue startValue;
      AudioUnitParameterValue endValue;
    } ramp;
    struct {
      UInt32 bufferOffset;
      AudioUnitParameterValue value;
    } immediate;
  } eventValues;
};

struct AUPreset {
  SInt32 presetNumber;
  CFStringRef presetName;
};

struct AudioUnitCocoaViewInfo {
  CFURLRef mCocoaAUViewBundleLocation;
  CFStringRef mCocoaAUViewClass[1];
};

// ── Callbacks ───────────────────────────────────────────────────────────────
typedef OSStatus (*AURenderCallback)(void* inRefCon, AudioUnitRenderActionFlags* ioActionFlags,
                                     const AudioTimeStamp* inTimeStamp, UInt32 inBusNumber,
                                     UInt32 inNumberFrames, AudioBufferList* ioData);

struct AURenderCallbackStruct {
  AURenderCallback inputProc;
  void* inputProcRefCon;
};

struct AudioUnitConnection {
  AudioUnit sourceAudioUnit;
  UInt32 sourceOutputNumber;
  UInt32 destInputNumber;
};

typedef void (*AudioUnitPropertyListenerProc)(void* inRefCon, AudioUnit inUnit,
                                              AudioUnitPropertyID inID, AudioUnitScope inScope,
                                              AudioUnitElement inElement);

struct MusicDeviceNoteParams {
  UInt32 argCount;
  Float32 mPitch;
  Float32 mVelocity;
};

// ── The component plug-in interface ─────────────────────────────────────────
typedef OSStatus (*AudioComponentMethod)(void* self, ...);

struct AudioComponentDescription {
  OSType componentType;
  OSType componentSubType;
  OSType componentManufacturer;
  UInt32 componentFlags;
  UInt32 componentFlagsMask;
};

struct AudioComponentPlugInInterface {
  OSStatus (*Open)(void* self, AudioComponentInstance mInstance);
  OSStatus (*Close)(void* self);
  AudioComponentMethod (*Lookup)(SInt16 selector);
  void* reserved;
};

OSStatus AudioUnitGetProperty(AudioUnit, AudioUnitPropertyID, AudioUnitScope, AudioUnitElement,
                              void* outData, UInt32* ioDataSize);
OSStatus AudioUnitSetProperty(AudioUnit, AudioUnitPropertyID, AudioUnitScope, AudioUnitElement,
                              const void* inData, UInt32 inDataSize);
void AudioUnitParameterSet(AudioUnit, AudioUnitParameterID, AudioUnitScope, AudioUnitElement,
                           AudioUnitParameterValue, UInt32);

// ── MIDI output, channel layouts, and parameter events ──────────────────────
typedef OSStatus (*AUMIDIOutputCallback)(void* userData, const AudioTimeStamp* timeStamp,
                                         UInt32 midiOutNum, const struct MIDIPacketList* pktlist);

struct AUMIDIOutputCallbackStruct {
  AUMIDIOutputCallback midiOutputCallback;
  void* userData;
};

/** A supported input/output channel pair. -1 means "any". */
struct AUChannelInfo {
  SInt16 inChannels;
  SInt16 outChannels;
};

enum {
  kParameterEvent_Immediate = 1,
  kParameterEvent_Ramped = 0,
};

// ── The keys a saved AU preset dictionary carries ───────────────────────────
#define kAUPresetVersionKey "version"
#define kAUPresetTypeKey "type"
#define kAUPresetSubtypeKey "subtype"
#define kAUPresetManufacturerKey "manufacturer"
#define kAUPresetDataKey "data"
#define kAUPresetNameKey "name"
#define kAUPresetNumberKey "preset-number"

// ── Finding and instantiating an AudioUnit (the HAL output unit) ────────────
enum {
  kAudioUnitType_Output = 0x61756F75,
  kAudioUnitSubType_HALOutput = 0x6168616C,
  kAudioUnitSubType_DefaultOutput = 0x64656620,
  kAudioUnitManufacturer_Apple = 0x6170706C,
  kAudioOutputUnitProperty_CurrentDevice = 2000,
  kAudioOutputUnitProperty_EnableIO = 2003,
  kAudioOutputUnitProperty_SetInputCallback = 2005,
  kAudioOutputUnitProperty_HasIO = 2006,
};

AudioComponent AudioComponentFindNext(AudioComponent, const AudioComponentDescription*);
OSStatus AudioComponentInstanceNew(AudioComponent, AudioComponentInstance*);
OSStatus AudioComponentInstanceDispose(AudioComponentInstance);
AudioComponent AudioComponentInstanceGetComponent(AudioComponentInstance);
OSStatus AudioComponentGetDescription(AudioComponent, AudioComponentDescription*);
OSStatus AudioUnitInitialize(AudioUnit);
OSStatus AudioUnitUninitialize(AudioUnit);
OSStatus AudioOutputUnitStart(AudioUnit);
OSStatus AudioOutputUnitStop(AudioUnit);
OSStatus AudioUnitRender(AudioUnit, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32,
                         UInt32, AudioBufferList*);
