// SPDX-License-Identifier: Apache-2.0
// Every Apple number the macOS hosting work restates, beside Apple's own name
// for it.
//
// host_au.h keeps the parameter flags, units, component types and flags it
// reads in host::detail::aucode, so the logic that reads them is portable and
// runs in mac_host_test.cpp on any machine. audio_workgroup.h and au_wrapper.h
// restate the render-context layout and two property ids that only recent
// SDKs, and only blocks-enabled compilers, declare. Each restatement is one
// place to be wrong, and nothing on Windows can say whether it is: the shim in
// tests/au_shim was written by the same hand from the same memory.
//
// So this list. Compiled against the shim by mac_syntax_mac_host.cpp, it
// proves the two copies agree; compiled against Apple's headers by
// mac_host_selftest.cpp on a Mac, it is the real check, and the self-test
// prints every pair that differs. A name Apple does not have is a compile
// error there, which is the quickest way for a Mac to say so.
#pragma once

#if !defined(__APPLE__)
#error "mac_host_constants.h compares against Apple's headers: macOS (or the au_shim syntax check) only"
#endif

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <sonore/audio_coreaudio.h>
#include <sonore/audio_workgroup.h>
#include <sonore/host.h>

#include <cstddef>

namespace mac_host_constants {

struct Pair {
  const char* name;
  long long ours;
  long long apples;
};

/** Report every pair through `report` and return how many differ. */
inline int compareAll(void (*report)(const Pair& pair, bool same)) {
  namespace c = sonore::host::detail::aucode;
  const Pair pairs[] = {
      {"kAudioUnitType_Effect", c::kTypeEffect, kAudioUnitType_Effect},
      {"kAudioUnitType_MusicDevice", c::kTypeMusicDevice, kAudioUnitType_MusicDevice},
      {"kAudioUnitType_MusicEffect", c::kTypeMusicEffect, kAudioUnitType_MusicEffect},
      {"kAudioUnitType_MIDIProcessor", c::kTypeMidiProcessor, kAudioUnitType_MIDIProcessor},
      {"kAudioUnitType_Generator", c::kTypeGenerator, kAudioUnitType_Generator},

      {"kAudioComponentFlag_Unsearchable", c::kFlagUnsearchable, kAudioComponentFlag_Unsearchable},
      {"kAudioComponentFlag_SandboxSafe", c::kFlagSandboxSafe, kAudioComponentFlag_SandboxSafe},
      {"kAudioComponentFlag_IsV3AudioUnit", c::kFlagIsV3AudioUnit, kAudioComponentFlag_IsV3AudioUnit},
      {"kAudioComponentFlag_RequiresAsyncInstantiation", c::kFlagRequiresAsyncInstantiation,
       kAudioComponentFlag_RequiresAsyncInstantiation},
      {"kAudioComponentFlag_CanLoadInProcess", c::kFlagCanLoadInProcess,
       kAudioComponentFlag_CanLoadInProcess},

      {"kAudioUnitParameterFlag_CFNameRelease", c::kParamCFNameRelease,
       kAudioUnitParameterFlag_CFNameRelease},
      {"kAudioUnitParameterFlag_MeterReadOnly", c::kParamMeterReadOnly,
       kAudioUnitParameterFlag_MeterReadOnly},
      {"kAudioUnitParameterFlag_HasClump", c::kParamHasClump, kAudioUnitParameterFlag_HasClump},
      {"kAudioUnitParameterFlag_ValuesHaveStrings", c::kParamValuesHaveStrings,
       kAudioUnitParameterFlag_ValuesHaveStrings},
      {"kAudioUnitParameterFlag_DisplayLogarithmic", c::kParamDisplayLogarithmic,
       kAudioUnitParameterFlag_DisplayLogarithmic},
      {"kAudioUnitParameterFlag_HasCFNameString", c::kParamHasCFNameString,
       kAudioUnitParameterFlag_HasCFNameString},
      {"kAudioUnitParameterFlag_IsReadable", c::kParamIsReadable, kAudioUnitParameterFlag_IsReadable},
      {"kAudioUnitParameterFlag_IsWritable", (long long) c::kParamIsWritable,
       (long long) (unsigned) kAudioUnitParameterFlag_IsWritable},

      {"kAudioUnitParameterUnit_Generic", c::kUnitGeneric, kAudioUnitParameterUnit_Generic},
      {"kAudioUnitParameterUnit_Indexed", c::kUnitIndexed, kAudioUnitParameterUnit_Indexed},
      {"kAudioUnitParameterUnit_Boolean", c::kUnitBoolean, kAudioUnitParameterUnit_Boolean},
      {"kAudioUnitParameterUnit_Percent", c::kUnitPercent, kAudioUnitParameterUnit_Percent},
      {"kAudioUnitParameterUnit_Seconds", c::kUnitSeconds, kAudioUnitParameterUnit_Seconds},
      {"kAudioUnitParameterUnit_SampleFrames", c::kUnitSampleFrames,
       kAudioUnitParameterUnit_SampleFrames},
      {"kAudioUnitParameterUnit_Phase", c::kUnitPhase, kAudioUnitParameterUnit_Phase},
      {"kAudioUnitParameterUnit_Rate", c::kUnitRate, kAudioUnitParameterUnit_Rate},
      {"kAudioUnitParameterUnit_Hertz", c::kUnitHertz, kAudioUnitParameterUnit_Hertz},
      {"kAudioUnitParameterUnit_Cents", c::kUnitCents, kAudioUnitParameterUnit_Cents},
      {"kAudioUnitParameterUnit_RelativeSemiTones", c::kUnitRelativeSemiTones,
       kAudioUnitParameterUnit_RelativeSemiTones},
      {"kAudioUnitParameterUnit_MIDINoteNumber", c::kUnitMIDINoteNumber,
       kAudioUnitParameterUnit_MIDINoteNumber},
      {"kAudioUnitParameterUnit_MIDIController", c::kUnitMIDIController,
       kAudioUnitParameterUnit_MIDIController},
      {"kAudioUnitParameterUnit_Decibels", c::kUnitDecibels, kAudioUnitParameterUnit_Decibels},
      {"kAudioUnitParameterUnit_LinearGain", c::kUnitLinearGain, kAudioUnitParameterUnit_LinearGain},
      {"kAudioUnitParameterUnit_Degrees", c::kUnitDegrees, kAudioUnitParameterUnit_Degrees},
      {"kAudioUnitParameterUnit_EqualPowerCrossfade", c::kUnitEqualPowerCrossfade,
       kAudioUnitParameterUnit_EqualPowerCrossfade},
      {"kAudioUnitParameterUnit_MixerFaderCurve1", c::kUnitMixerFaderCurve1,
       kAudioUnitParameterUnit_MixerFaderCurve1},
      {"kAudioUnitParameterUnit_Pan", c::kUnitPan, kAudioUnitParameterUnit_Pan},
      {"kAudioUnitParameterUnit_Meters", c::kUnitMeters, kAudioUnitParameterUnit_Meters},
      {"kAudioUnitParameterUnit_AbsoluteCents", c::kUnitAbsoluteCents,
       kAudioUnitParameterUnit_AbsoluteCents},
      {"kAudioUnitParameterUnit_Octaves", c::kUnitOctaves, kAudioUnitParameterUnit_Octaves},
      {"kAudioUnitParameterUnit_BPM", c::kUnitBPM, kAudioUnitParameterUnit_BPM},
      {"kAudioUnitParameterUnit_Beats", c::kUnitBeats, kAudioUnitParameterUnit_Beats},
      {"kAudioUnitParameterUnit_Milliseconds", c::kUnitMilliseconds,
       kAudioUnitParameterUnit_Milliseconds},
      {"kAudioUnitParameterUnit_Ratio", c::kUnitRatio, kAudioUnitParameterUnit_Ratio},
      {"kAudioUnitParameterUnit_CustomUnit", c::kUnitCustomUnit, kAudioUnitParameterUnit_CustomUnit},

      {"kAudioUnitProperty_RequestViewController", c::kPropertyRequestViewController,
       kAudioUnitProperty_RequestViewController},
      {"kAudioUnitProperty_RenderContextObserver", c::kPropertyRenderContextObserver,
       kAudioUnitProperty_RenderContextObserver},
      {"kAudioUnitProperty_RenderContextObserver (au_wrapper.h)",
       (long long) sonore::detail::kRenderContextObserverProperty,
       kAudioUnitProperty_RenderContextObserver},
      {"kAUParameterListener_AnyParameter", (long long) c::kAnyParameter,
       (long long) (unsigned) kAUParameterListener_AnyParameter},
      {"kAudioDevicePropertyIOThreadOSWorkgroup",
       (long long) sonore::standalone::CoreAudioOutput::kIOThreadOSWorkgroup,
       (long long) kAudioDevicePropertyIOThreadOSWorkgroup},

      // The names auStatusText prints: wrong ones would mislabel an error.
      {"kAudioUnitErr_InvalidProperty", -10879, kAudioUnitErr_InvalidProperty},
      {"kAudioUnitErr_InvalidParameter", -10878, kAudioUnitErr_InvalidParameter},
      {"kAudioUnitErr_InvalidElement", -10877, kAudioUnitErr_InvalidElement},
      {"kAudioUnitErr_NoConnection", -10876, kAudioUnitErr_NoConnection},
      {"kAudioUnitErr_FailedInitialization", -10875, kAudioUnitErr_FailedInitialization},
      {"kAudioUnitErr_TooManyFramesToProcess", -10874, kAudioUnitErr_TooManyFramesToProcess},
      {"kAudioUnitErr_FormatNotSupported", -10868, kAudioUnitErr_FormatNotSupported},
      {"kAudioUnitErr_Uninitialized", -10867, kAudioUnitErr_Uninitialized},
      {"kAudioUnitErr_InvalidScope", -10866, kAudioUnitErr_InvalidScope},
      {"kAudioUnitErr_PropertyNotWritable", -10865, kAudioUnitErr_PropertyNotWritable},
      {"kAudioUnitErr_CannotDoInCurrentContext", -10863, kAudioUnitErr_CannotDoInCurrentContext},
      {"kAudioUnitErr_InvalidPropertyValue", -10851, kAudioUnitErr_InvalidPropertyValue},
      {"kAudioUnitErr_PropertyNotInUse", -10850, kAudioUnitErr_PropertyNotInUse},
      {"kAudioUnitErr_Initialized", -10849, kAudioUnitErr_Initialized},
      {"kAudioUnitErr_InvalidOfflineRender", -10848, kAudioUnitErr_InvalidOfflineRender},
      {"kAudioUnitErr_Unauthorized", -10847, kAudioUnitErr_Unauthorized},
      {"kAudioComponentErr_InstanceTimedOut", -66754, kAudioComponentErr_InstanceTimedOut},
      {"kAudioUnitErr_MIDIOutputBufferFull", -66753, kAudioUnitErr_MIDIOutputBufferFull},
      {"kAudioComponentErr_DuplicateDescription", -66752, kAudioComponentErr_DuplicateDescription},
      {"kAudioComponentErr_UnsupportedType", -66751, kAudioComponentErr_UnsupportedType},
      {"kAudioComponentErr_TooManyInstances", -66750, kAudioComponentErr_TooManyInstances},
      {"kAudioComponentErr_InstanceInvalidated", -66749, kAudioComponentErr_InstanceInvalidated},
      {"kAudioComponentErr_NotPermitted", -66748, kAudioComponentErr_NotPermitted},
      {"kAudioComponentErr_InitializationTimedOut", -66747,
       kAudioComponentErr_InitializationTimedOut},
      {"kAudioComponentErr_InvalidFormat", -66746, kAudioComponentErr_InvalidFormat},
      {"kAudioUnitErr_RenderTimeout", -66745, kAudioUnitErr_RenderTimeout},
      {"kAudioUnitErr_ExtensionNotFound", -66744, kAudioUnitErr_ExtensionNotFound},

      // The render context, as audio_workgroup.h restates it.
      {"sizeof(AudioUnitRenderContext)", (long long) sizeof(sonore::detail::AudioUnitRenderContextMirror),
       (long long) sizeof(AudioUnitRenderContext)},
      {"offsetof(AudioUnitRenderContext, workgroup)",
       (long long) offsetof(sonore::detail::AudioUnitRenderContextMirror, workgroup),
       (long long) offsetof(AudioUnitRenderContext, workgroup)},
  };
  int differ = 0;
  for (const Pair& pair : pairs) {
    const bool same = pair.ours == pair.apples;
    if (!same) ++differ;
    if (report) report(pair, same);
  }
  return differ;
}

} // namespace mac_host_constants
