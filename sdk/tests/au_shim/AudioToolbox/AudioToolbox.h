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
  kAudioUnitErr_NoConnection = -10876,
  kAudioUnitErr_FailedInitialization = -10875,
  kAudioUnitErr_InvalidFile = -10871,
  kAudioUnitErr_UnknownFileType = -10870,
  kAudioUnitErr_FileNotSpecified = -10869,
  kAudioUnitErr_CannotDoInCurrentContext = -10863,
  kAudioUnitErr_PropertyNotInUse = -10850,
  kAudioUnitErr_Initialized = -10849,
  kAudioUnitErr_InvalidOfflineRender = -10848,
  kAudioUnitErr_Unauthorized = -10847,
  kAudioUnitErr_MIDIOutputBufferFull = -66753,
  kAudioUnitErr_RenderTimeout = -66745,
  kAudioUnitErr_ExtensionNotFound = -66744,
  kAudioComponentErr_InstanceTimedOut = -66754,
  kAudioComponentErr_DuplicateDescription = -66752,
  kAudioComponentErr_UnsupportedType = -66751,
  kAudioComponentErr_TooManyInstances = -66750,
  kAudioComponentErr_InstanceInvalidated = -66749,
  kAudioComponentErr_NotPermitted = -66748,
  kAudioComponentErr_InitializationTimedOut = -66747,
  kAudioComponentErr_InvalidFormat = -66746,
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
  kAudioUnitProperty_OfflineRender = 37,
  kAudioUnitProperty_HostCallbacks = 27,
  kAudioUnitProperty_TailTime = 20,
  kAudioUnitProperty_BypassEffect = 21,
  kAudioUnitProperty_CocoaUI = 31,
  kAudioUnitProperty_ParameterClumpName = 35,
  kAudioUnitProperty_ParameterValueStrings = 16,
  kAudioUnitProperty_ParameterStringFromValue = 33,
  kAudioUnitProperty_ParameterValueFromString = 38,
  kAudioUnitProperty_AUHostIdentifier = 46,
  kAudioUnitProperty_MIDIOutputCallbackInfo = 47,
  kAudioUnitProperty_MIDIOutputCallback = 48,
  kAudioUnitProperty_RequestViewController = 56,
  kAudioUnitProperty_RenderContextObserver = 60,
};

typedef OSStatus (*HostCallback_GetBeatAndTempo)(void*, Float64*, Float64*);
typedef OSStatus (*HostCallback_GetMusicalTimeLocation)(void*, UInt32*, Float32*, UInt32*,
                                                        Float64*);
typedef OSStatus (*HostCallback_GetTransportState)(void*, Boolean*, Boolean*, Float64*, Boolean*,
                                                   Float64*, Float64*);
typedef OSStatus (*HostCallback_GetTransportState2)(void*, Boolean*, Boolean*, Boolean*, Float64*,
                                                    Boolean*, Float64*, Float64*);
typedef struct HostCallbackInfo {
  void* hostUserData;
  HostCallback_GetBeatAndTempo beatAndTempoProc;
  HostCallback_GetMusicalTimeLocation musicalTimeLocationProc;
  HostCallback_GetTransportState transportStateProc;
  HostCallback_GetTransportState2 transportStateProc2;
} HostCallbackInfo;

// kAudioUnitProperty_AUHostIdentifier's value, as AudioUnitProperties.h
// declares it (macOS only there). hostVersion was a NumVersion and is the
// same four bytes as a UInt32.
struct AUHostVersionIdentifier {
  CFStringRef hostName;
  UInt32 hostVersion;
};
typedef struct AUHostVersionIdentifier AUHostVersionIdentifier;

// Bit positions as AudioUnitProperties.h has them. The first version of this
// shim had IsReadable, IsWritable and HasCFNameString off by a bit or four and
// five unit numbers wrong -- harmless while only au_wrapper.h read them by
// name, wrong the moment host_au.h's portable mapping restated them as
// numbers. tests/mac_host_constants.h now holds the two in step.
enum : UInt32 {
  kAudioUnitParameterFlag_CFNameRelease = 1u << 4,
  kAudioUnitParameterFlag_OmitFromPresets = 1u << 13,
  kAudioUnitParameterFlag_PlotHistory = 1u << 14,
  kAudioUnitParameterFlag_MeterReadOnly = 1u << 15,
  kAudioUnitParameterFlag_HasClump = 1u << 20,
  kAudioUnitParameterFlag_ValuesHaveStrings = 1u << 21,
  kAudioUnitParameterFlag_DisplayLogarithmic = 1u << 22,
  kAudioUnitParameterFlag_IsHighResolution = 1u << 23,
  kAudioUnitParameterFlag_NonRealTime = 1u << 24,
  kAudioUnitParameterFlag_CanRamp = 1u << 25,
  kAudioUnitParameterFlag_ExpertMode = 1u << 26,
  kAudioUnitParameterFlag_HasCFNameString = 1u << 27,
  kAudioUnitParameterFlag_IsGlobalMeta = 1u << 28,
  kAudioUnitParameterFlag_IsElementMeta = 1u << 29,
  kAudioUnitParameterFlag_IsReadable = 1u << 30,
  kAudioUnitParameterFlag_IsWritable = 1u << 31,
};

enum {
  kAudioUnitParameterUnit_Generic = 0,
  kAudioUnitParameterUnit_Indexed = 1,
  kAudioUnitParameterUnit_Boolean = 2,
  kAudioUnitParameterUnit_Percent = 3,
  kAudioUnitParameterUnit_Seconds = 4,
  kAudioUnitParameterUnit_SampleFrames = 5,
  kAudioUnitParameterUnit_Phase = 6,
  kAudioUnitParameterUnit_Rate = 7,
  kAudioUnitParameterUnit_Hertz = 8,
  kAudioUnitParameterUnit_Cents = 9,
  kAudioUnitParameterUnit_RelativeSemiTones = 10,
  kAudioUnitParameterUnit_MIDINoteNumber = 11,
  kAudioUnitParameterUnit_MIDIController = 12,
  kAudioUnitParameterUnit_Decibels = 13,
  kAudioUnitParameterUnit_LinearGain = 14,
  kAudioUnitParameterUnit_Degrees = 15,
  kAudioUnitParameterUnit_EqualPowerCrossfade = 16,
  kAudioUnitParameterUnit_MixerFaderCurve1 = 17,
  kAudioUnitParameterUnit_Pan = 18,
  kAudioUnitParameterUnit_Meters = 19,
  kAudioUnitParameterUnit_AbsoluteCents = 20,
  kAudioUnitParameterUnit_Octaves = 21,
  kAudioUnitParameterUnit_BPM = 22,
  kAudioUnitParameterUnit_Beats = 23,
  kAudioUnitParameterUnit_Milliseconds = 24,
  kAudioUnitParameterUnit_Ratio = 25,
  kAudioUnitParameterUnit_CustomUnit = 26,
};

enum { kAudioUnitParameterName_Full = -1 };

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

struct AudioUnitParameterStringFromValue {
  AudioUnitParameterID inParamID;
  const AudioUnitParameterValue* inValue;
  CFStringRef outString;
};

struct AudioUnitParameterValueFromString {
  AudioUnitParameterID inParamID;
  CFStringRef inString;
  AudioUnitParameterValue outValue;
};

// AudioUnitUtilities.h: how an AU (or its view) tells listeners a parameter
// moved -- one parameter, or with kAUParameterListener_AnyParameter, all.
struct AudioUnitParameter {
  AudioUnit mAudioUnit;
  AudioUnitParameterID mParameterID;
  AudioUnitScope mScope;
  AudioUnitElement mElement;
};
typedef struct OpaqueAUParameterListener* AUParameterListenerRef;
OSStatus AUParameterSet(AUParameterListenerRef, void*, const AudioUnitParameter*,
                        AudioUnitParameterValue, UInt32);
OSStatus AUParameterListenerNotify(AUParameterListenerRef, void*, const AudioUnitParameter*);
#define kAUParameterListener_AnyParameter 0xFFFFFFFF

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

// ── Hosting an AU (host_au.h) ───────────────────────────────────────────────
enum {
  kAudioUnitType_Effect = 0x61756678,        // aufx
  kAudioUnitType_MusicDevice = 0x61756D75,   // aumu
  kAudioUnitType_MusicEffect = 0x61756D66,   // aumf
  kAudioUnitType_MIDIProcessor = 0x61756D69, // aumi
  kAudioUnitType_Generator = 0x6175676E,     // augn
};

typedef UInt32 AudioComponentFlags;
enum {
  kAudioComponentFlag_Unsearchable = 1,
  kAudioComponentFlag_SandboxSafe = 2,
  kAudioComponentFlag_IsV3AudioUnit = 4,
  kAudioComponentFlag_RequiresAsyncInstantiation = 8,
  kAudioComponentFlag_CanLoadInProcess = 0x10,
};
typedef UInt32 AudioComponentInstantiationOptions;

enum {
  kAudioTimeStampSampleTimeValid = 1u << 0,
  kAudioTimeStampHostTimeValid = 1u << 1,
};

typedef AudioComponentInstance MusicDeviceComponent;

OSStatus AudioComponentCopyName(AudioComponent, CFStringRef*);
OSStatus AudioComponentGetVersion(AudioComponent, UInt32*);
OSStatus AudioUnitGetPropertyInfo(AudioUnit, AudioUnitPropertyID, AudioUnitScope, AudioUnitElement,
                                  UInt32* outDataSize, Boolean* outWritable);
OSStatus AudioUnitGetParameter(AudioUnit, AudioUnitParameterID, AudioUnitScope, AudioUnitElement,
                               AudioUnitParameterValue* outValue);
OSStatus AudioUnitSetParameter(AudioUnit, AudioUnitParameterID, AudioUnitScope, AudioUnitElement,
                               AudioUnitParameterValue inValue, UInt32 inBufferOffsetInFrames);
OSStatus AudioUnitReset(AudioUnit, AudioUnitScope, AudioUnitElement);
OSStatus MusicDeviceMIDIEvent(MusicDeviceComponent, UInt32 inStatus, UInt32 inData1, UInt32 inData2,
                              UInt32 inOffsetSampleFrame);
OSStatus MusicDeviceSysEx(MusicDeviceComponent, const UInt8* inData, UInt32 inLength);

// AudioUnitUtilities.h: the event listener a host hears a unit's own edits
// through. One listener type serves both the old parameter API and events.
typedef UInt32 AudioUnitEventType;
enum {
  kAudioUnitEvent_ParameterValueChange = 0,
  kAudioUnitEvent_BeginParameterChangeGesture = 1,
  kAudioUnitEvent_EndParameterChangeGesture = 2,
  kAudioUnitEvent_PropertyChange = 3,
};
struct AudioUnitProperty {
  AudioUnit mAudioUnit;
  AudioUnitPropertyID mPropertyID;
  AudioUnitScope mScope;
  AudioUnitElement mElement;
};
struct AudioUnitEvent {
  AudioUnitEventType mEventType;
  union {
    AudioUnitParameter mParameter;
    AudioUnitProperty mProperty;
  } mArgument;
};
typedef AUParameterListenerRef AUEventListenerRef;
typedef void (*AUEventListenerProc)(void* inCallbackRefCon, void* inObject,
                                    const AudioUnitEvent* inEvent, UInt64 inEventHostTime,
                                    AudioUnitParameterValue inParameterValue);
OSStatus AUEventListenerCreate(AUEventListenerProc inProc, void* inCallbackRefCon,
                               CFRunLoopRef inRunLoop, CFStringRef inRunLoopMode,
                               Float32 inNotificationInterval, Float32 inValueChangeGranularity,
                               AUEventListenerRef* outListener);
OSStatus AUEventListenerAddEventType(AUEventListenerRef inListener, void* inObject,
                                     const AudioUnitEvent* inEvent);
OSStatus AUListenerDispose(AUParameterListenerRef inListener);

// The render context kAudioUnitProperty_RenderContextObserver delivers
// (macOS 11). The observer itself is a block, which no shim can declare.
#include <os/workgroup.h>
struct AudioUnitRenderContext {
  os_workgroup_t workgroup;
  UInt32 reserved[6];
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

// ── ExtAudioFile: a file decoded to PCM (system_audio_format.h) ─────────────
typedef struct OpaqueExtAudioFile* ExtAudioFileRef;
typedef UInt32 ExtAudioFilePropertyID;
enum {
  kExtAudioFileProperty_FileDataFormat = 0x66666D74,   // 'ffmt'
  kExtAudioFileProperty_ClientDataFormat = 0x63666D74, // 'cfmt'
  kExtAudioFileProperty_FileLengthFrames = 0x2366726D, // '#frm'
};
OSStatus ExtAudioFileOpenURL(CFURLRef inURL, ExtAudioFileRef* outExtAudioFile);
OSStatus ExtAudioFileDispose(ExtAudioFileRef inExtAudioFile);
OSStatus ExtAudioFileRead(ExtAudioFileRef inExtAudioFile, UInt32* ioNumberFrames,
                          AudioBufferList* ioData);
OSStatus ExtAudioFileGetProperty(ExtAudioFileRef inExtAudioFile,
                                 ExtAudioFilePropertyID inPropertyID, UInt32* ioPropertyDataSize,
                                 void* outPropertyData);
OSStatus ExtAudioFileSetProperty(ExtAudioFileRef inExtAudioFile,
                                 ExtAudioFilePropertyID inPropertyID, UInt32 inPropertyDataSize,
                                 const void* inPropertyData);
