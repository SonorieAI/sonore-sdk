// SPDX-License-Identifier: Apache-2.0
// Sonore SDK test fixture: VST3 processors whose buses are not "one stereo
// bus each way".
//
// A VST3 host negotiates a speaker arrangement for every audio bus and then
// hands process() a buffer for each bus at that arrangement's width. sonore/
// host.h asked for "mono" as kSpeakerL -- one LEFT speaker, which a plugin
// that accepts mono and stereo refuses -- ignored the refusal, and passed one
// input and one output bus as wide as the caller asked whatever the plugin
// had: a sidechain bus that was never there, a second channel that did not
// exist.
//
// Written against the C API so it can CHECK rather than crash. Each processor
// compares what process() was handed with the arrangements it agreed to,
// counts every block that differs in the process environment (shared with
// the test), and leaves such a block untouched. One that matches comes out
// at twice its main input, plus the sidechain. The arrangement the host asked
// for on the main input is recorded too.
//
//   sidechain    in: main (mono or stereo) + aux mono   out: main, as the input
//   stereo only  in: main stereo                         out: main stereo

#include <vst3_c_api.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define FIXTURE_EXPORT extern "C" __declspec(dllexport)
#else
#define FIXTURE_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

// ── The process environment, which the test shares ─────────────────────────

int readEnv(const char* name) {
  char text[32] = {0};
#if defined(_WIN32)
  GetEnvironmentVariableA(name, text, (DWORD) sizeof(text));
#else
  if (const char* v = std::getenv(name)) std::snprintf(text, sizeof(text), "%s", v);
#endif
  return std::atoi(text);
}

void writeEnv(const char* name, const char* text) {
#if defined(_WIN32)
  SetEnvironmentVariableA(name, text);
#else
  setenv(name, text, 1);
#endif
}

void bump(const char* name) {
  char text[32];
  std::snprintf(text, sizeof(text), "%d", readEnv(name) + 1);
  writeEnv(name, text);
}

int channelsOf(Steinberg_Vst_SpeakerArrangement arrangement) {
  int n = 0;
  for (; arrangement; arrangement &= arrangement - 1) ++n;
  return n;
}

bool sameIid(const Steinberg_TUID a, const Steinberg_TUID b) { return std::memcmp(a, b, 16) == 0; }

// ── The variants ────────────────────────────────────────────────────────────

struct Variant {
  const char* name;
  Steinberg_TUID cid;
  bool sidechain;
};

const Variant kVariants[2] = {
    {"Sonore Bus Fixture (sidechain)",
     {'S', 'o', 'n', 'o', 'r', 'e', 'B', 'u', 's', 'S', 'i', 'd', 'e', '0', '0', '1'},
     true},
    {"Sonore Bus Fixture (stereo only)",
     {'S', 'o', 'n', 'o', 'r', 'e', 'B', 'u', 's', 'S', 't', 'e', 'r', '0', '0', '1'},
     false},
};

struct Fixture;
struct ComponentFace {
  Steinberg_Vst_IComponentVtbl* lpVtbl;
  Fixture* self;
};
struct ProcessorFace {
  Steinberg_Vst_IAudioProcessorVtbl* lpVtbl;
  Fixture* self;
};

struct Fixture {
  ComponentFace component;
  ProcessorFace processor;
  std::atomic<int> refs{1};
  const Variant* variant = nullptr;
  Steinberg_Vst_SpeakerArrangement in[2] = {Steinberg_Vst_SpeakerArr_kStereo,
                                            Steinberg_Vst_SpeakerArr_kMono};
  Steinberg_Vst_SpeakerArrangement out[1] = {Steinberg_Vst_SpeakerArr_kStereo};

  int inputs() const { return variant->sidechain ? 2 : 1; }
};

Fixture* fromComponent(void* p) { return static_cast<ComponentFace*>(p)->self; }
Fixture* fromProcessor(void* p) { return static_cast<ProcessorFace*>(p)->self; }

Steinberg_tresult queryFixture(Fixture* f, const Steinberg_TUID iid, void** obj) {
  if (!obj) return Steinberg_kInvalidArgument;
  if (sameIid(iid, Steinberg_FUnknown_iid) || sameIid(iid, Steinberg_IPluginBase_iid) ||
      sameIid(iid, Steinberg_Vst_IComponent_iid)) {
    *obj = &f->component;
  } else if (sameIid(iid, Steinberg_Vst_IAudioProcessor_iid)) {
    *obj = &f->processor;
  } else {
    *obj = nullptr;
    return Steinberg_kNoInterface;
  }
  f->refs.fetch_add(1);
  return Steinberg_kResultOk;
}

Steinberg_uint32 releaseFixture(Fixture* f) {
  const int left = f->refs.fetch_sub(1) - 1;
  if (left == 0) delete f;
  return (Steinberg_uint32) left;
}

// ── IComponent ──────────────────────────────────────────────────────────────

Steinberg_tresult SMTG_STDMETHODCALLTYPE cQuery(void* p, const Steinberg_TUID iid, void** obj) {
  return queryFixture(fromComponent(p), iid, obj);
}
Steinberg_uint32 SMTG_STDMETHODCALLTYPE cAddRef(void* p) {
  return (Steinberg_uint32) (fromComponent(p)->refs.fetch_add(1) + 1);
}
Steinberg_uint32 SMTG_STDMETHODCALLTYPE cRelease(void* p) { return releaseFixture(fromComponent(p)); }
Steinberg_tresult SMTG_STDMETHODCALLTYPE cInitialize(void*, struct Steinberg_FUnknown*) {
  return Steinberg_kResultOk;
}
Steinberg_tresult SMTG_STDMETHODCALLTYPE cTerminate(void*) { return Steinberg_kResultOk; }
Steinberg_tresult SMTG_STDMETHODCALLTYPE cControllerClassId(void*, Steinberg_TUID) {
  return Steinberg_kResultFalse; // no controller: the processor is all this is
}
Steinberg_tresult SMTG_STDMETHODCALLTYPE cSetIoMode(void*, Steinberg_Vst_IoMode) {
  return Steinberg_kResultOk;
}
Steinberg_int32 SMTG_STDMETHODCALLTYPE cBusCount(void* p, Steinberg_Vst_MediaType type,
                                                 Steinberg_Vst_BusDirection dir) {
  if (type != Steinberg_Vst_MediaTypes_kAudio) return 0;
  return dir == Steinberg_Vst_BusDirections_kInput ? fromComponent(p)->inputs() : 1;
}
Steinberg_tresult SMTG_STDMETHODCALLTYPE cBusInfo(void* p, Steinberg_Vst_MediaType type,
                                                  Steinberg_Vst_BusDirection dir,
                                                  Steinberg_int32 index,
                                                  struct Steinberg_Vst_BusInfo* bus) {
  Fixture* f = fromComponent(p);
  const bool input = dir == Steinberg_Vst_BusDirections_kInput;
  if (type != Steinberg_Vst_MediaTypes_kAudio || !bus || index < 0 ||
      index >= (input ? f->inputs() : 1))
    return Steinberg_kInvalidArgument;
  std::memset(bus, 0, sizeof(*bus));
  bus->mediaType = type;
  bus->direction = dir;
  bus->channelCount = channelsOf(input ? f->in[index] : f->out[index]);
  const char* name = index == 0 ? "Main" : "Sidechain";
  for (int i = 0; name[i] && i < 127; ++i) bus->name[i] = (Steinberg_char16) name[i];
  bus->busType = index == 0 ? Steinberg_Vst_BusTypes_kMain : Steinberg_Vst_BusTypes_kAux;
  bus->flags = index == 0 ? Steinberg_Vst_BusInfo_BusFlags_kDefaultActive : 0;
  return Steinberg_kResultOk;
}
Steinberg_tresult SMTG_STDMETHODCALLTYPE cRouting(void*, struct Steinberg_Vst_RoutingInfo*,
                                                  struct Steinberg_Vst_RoutingInfo*) {
  return Steinberg_kNotImplemented;
}
Steinberg_tresult SMTG_STDMETHODCALLTYPE cActivateBus(void*, Steinberg_Vst_MediaType,
                                                      Steinberg_Vst_BusDirection, Steinberg_int32,
                                                      Steinberg_TBool) {
  return Steinberg_kResultOk;
}
Steinberg_tresult SMTG_STDMETHODCALLTYPE cSetActive(void*, Steinberg_TBool) {
  return Steinberg_kResultOk;
}
Steinberg_tresult SMTG_STDMETHODCALLTYPE cState(void*, struct Steinberg_IBStream*) {
  return Steinberg_kResultOk;
}

Steinberg_Vst_IComponentVtbl kComponentVtbl = {
    cQuery,     cAddRef,  cRelease,     cInitialize,  cTerminate, cControllerClassId, cSetIoMode,
    cBusCount,  cBusInfo, cRouting,     cActivateBus, cSetActive, cState,             cState,
};

// ── IAudioProcessor ─────────────────────────────────────────────────────────

Steinberg_tresult SMTG_STDMETHODCALLTYPE pQuery(void* p, const Steinberg_TUID iid, void** obj) {
  return queryFixture(fromProcessor(p), iid, obj);
}
Steinberg_uint32 SMTG_STDMETHODCALLTYPE pAddRef(void* p) {
  return (Steinberg_uint32) (fromProcessor(p)->refs.fetch_add(1) + 1);
}
Steinberg_uint32 SMTG_STDMETHODCALLTYPE pRelease(void* p) { return releaseFixture(fromProcessor(p)); }

Steinberg_tresult SMTG_STDMETHODCALLTYPE pSetArrangements(void* p,
                                                          Steinberg_Vst_SpeakerArrangement* ins,
                                                          Steinberg_int32 numIns,
                                                          Steinberg_Vst_SpeakerArrangement* outs,
                                                          Steinberg_int32 numOuts) {
  Fixture* f = fromProcessor(p);
  char text[32];
  std::snprintf(text, sizeof(text), "%llu",
                (unsigned long long) (numIns > 0 && ins ? ins[0] : 0));
  writeEnv("SONORE_VST3_BUS_REQUESTED", text);
  // All the buses or none, as the specification has it; then each variant's
  // own rules. A refusal keeps what the processor had, and the host is meant
  // to ask getBusArrangement what that is.
  if (numIns != f->inputs() || numOuts != 1 || !ins || !outs) return Steinberg_kResultFalse;
  const Steinberg_Vst_SpeakerArrangement mono = Steinberg_Vst_SpeakerArr_kMono;
  const Steinberg_Vst_SpeakerArrangement stereo = Steinberg_Vst_SpeakerArr_kStereo;
  if (f->variant->sidechain) {
    if ((ins[0] != mono && ins[0] != stereo) || outs[0] != ins[0] || ins[1] != mono)
      return Steinberg_kResultFalse;
  } else if (ins[0] != stereo || outs[0] != stereo) {
    return Steinberg_kResultFalse;
  }
  for (int i = 0; i < numIns; ++i) f->in[i] = ins[i];
  f->out[0] = outs[0];
  return Steinberg_kResultOk;
}
Steinberg_tresult SMTG_STDMETHODCALLTYPE pGetArrangement(void* p, Steinberg_Vst_BusDirection dir,
                                                         Steinberg_int32 index,
                                                         Steinberg_Vst_SpeakerArrangement* arr) {
  Fixture* f = fromProcessor(p);
  const bool input = dir == Steinberg_Vst_BusDirections_kInput;
  if (!arr || index < 0 || index >= (input ? f->inputs() : 1)) return Steinberg_kInvalidArgument;
  *arr = input ? f->in[index] : f->out[index];
  return Steinberg_kResultOk;
}
Steinberg_tresult SMTG_STDMETHODCALLTYPE pCanProcess(void*, Steinberg_int32 size) {
  return size == Steinberg_Vst_SymbolicSampleSizes_kSample32 ? Steinberg_kResultOk
                                                             : Steinberg_kResultFalse;
}
Steinberg_uint32 SMTG_STDMETHODCALLTYPE pZero(void*) { return 0; }
Steinberg_tresult SMTG_STDMETHODCALLTYPE pSetup(void*, struct Steinberg_Vst_ProcessSetup*) {
  return Steinberg_kResultOk;
}
Steinberg_tresult SMTG_STDMETHODCALLTYPE pSetProcessing(void*, Steinberg_TBool) {
  return Steinberg_kResultOk;
}

bool busesMatch(const Steinberg_Vst_AudioBusBuffers* buses, Steinberg_int32 count,
                const Steinberg_Vst_SpeakerArrangement* agreed, int expected) {
  if (count != expected || (count > 0 && !buses)) return false;
  for (int b = 0; b < count; ++b) {
    const int width = channelsOf(agreed[b]);
    if (buses[b].numChannels != width) return false;
    if (width > 0 && !buses[b].Steinberg_Vst_AudioBusBuffers_channelBuffers32) return false;
    for (int c = 0; c < width; ++c)
      if (!buses[b].Steinberg_Vst_AudioBusBuffers_channelBuffers32[c]) return false;
  }
  return true;
}

Steinberg_tresult SMTG_STDMETHODCALLTYPE pProcess(void* p, struct Steinberg_Vst_ProcessData* data) {
  Fixture* f = fromProcessor(p);
  if (!data || data->numSamples <= 0) return Steinberg_kResultOk;
  if (!busesMatch(data->inputs, data->numInputs, f->in, f->inputs()) ||
      !busesMatch(data->outputs, data->numOutputs, f->out, 1)) {
    bump("SONORE_VST3_BUS_VIOLATIONS");
    return Steinberg_kResultOk;
  }
  float** in = data->inputs[0].Steinberg_Vst_AudioBusBuffers_channelBuffers32;
  float** out = data->outputs[0].Steinberg_Vst_AudioBusBuffers_channelBuffers32;
  const float* side =
      f->variant->sidechain ? data->inputs[1].Steinberg_Vst_AudioBusBuffers_channelBuffers32[0]
                            : nullptr;
  for (int c = 0; c < data->outputs[0].numChannels; ++c)
    for (int i = 0; i < data->numSamples; ++i)
      out[c][i] = 2.0f * in[c][i] + (side ? side[i] : 0.0f);
  return Steinberg_kResultOk;
}

Steinberg_Vst_IAudioProcessorVtbl kProcessorVtbl = {
    pQuery, pAddRef, pRelease,       pSetArrangements, pGetArrangement, pCanProcess,
    pZero,  pSetup,  pSetProcessing, pProcess,         pZero,
};

// ── The factory ─────────────────────────────────────────────────────────────

Steinberg_tresult SMTG_STDMETHODCALLTYPE fQuery(void* self, const Steinberg_TUID iid, void** obj) {
  if (!obj) return Steinberg_kInvalidArgument;
  if (sameIid(iid, Steinberg_FUnknown_iid) || sameIid(iid, Steinberg_IPluginFactory_iid)) {
    *obj = self;
    return Steinberg_kResultOk;
  }
  *obj = nullptr;
  return Steinberg_kNoInterface;
}
Steinberg_uint32 SMTG_STDMETHODCALLTYPE fRef(void*) { return 1; }
// With SONORE_VST3_BUS_UNTERMINATED set, the factory fills its vendor and
// every class's name to the last byte with no terminator, as strncpy does
// with a string as long as the field. The name is PClassInfo's last field.
Steinberg_tresult SMTG_STDMETHODCALLTYPE fInfo(void*, struct Steinberg_PFactoryInfo* info) {
  if (!info) return Steinberg_kInvalidArgument;
  std::memset(info, 0, sizeof(*info));
  std::snprintf(info->vendor, sizeof(info->vendor), "Sonorie");
  if (readEnv("SONORE_VST3_BUS_UNTERMINATED")) {
    std::memset(info->vendor, 'V', sizeof(info->vendor));
    std::snprintf(info->url, sizeof(info->url), "https://sonorie.com");
  }
  return Steinberg_kResultOk;
}
// With SONORE_VST3_BUS_GARBAGE_COUNT set, the factory says it has 2^31 - 1
// classes -- a count it never set -- and still describes only its two.
Steinberg_int32 SMTG_STDMETHODCALLTYPE fCount(void*) {
  return readEnv("SONORE_VST3_BUS_GARBAGE_COUNT") ? 0x7FFFFFFF : 2;
}
Steinberg_tresult SMTG_STDMETHODCALLTYPE fClassInfo(void*, Steinberg_int32 index,
                                                    struct Steinberg_PClassInfo* info) {
  if (!info || index < 0 || index >= 2) return Steinberg_kInvalidArgument;
  std::memset(info, 0, sizeof(*info));
  std::memcpy(info->cid, kVariants[index].cid, 16);
  info->cardinality = 0x7FFFFFFF;
  std::snprintf(info->category, sizeof(info->category), "Audio Module Class");
  std::snprintf(info->name, sizeof(info->name), "%s", kVariants[index].name);
  if (readEnv("SONORE_VST3_BUS_UNTERMINATED")) std::memset(info->name, 'C', sizeof(info->name));
  return Steinberg_kResultOk;
}
Steinberg_tresult SMTG_STDMETHODCALLTYPE fCreate(void*, Steinberg_FIDString cid,
                                                 Steinberg_FIDString iid, void** obj) {
  if (!obj) return Steinberg_kInvalidArgument;
  *obj = nullptr;
  for (const Variant& v : kVariants) {
    if (std::memcmp(cid, v.cid, 16) != 0) continue;
    auto* f = new Fixture();
    f->variant = &v;
    f->component = {&kComponentVtbl, f};
    f->processor = {&kProcessorVtbl, f};
    const Steinberg_tresult result = queryFixture(f, iid, obj);
    releaseFixture(f); // the creation reference; the query took its own
    return result;
  }
  return Steinberg_kInvalidArgument;
}

Steinberg_IPluginFactoryVtbl kFactoryVtbl = {fQuery, fRef,       fRef,   fInfo,
                                             fCount, fClassInfo, fCreate};
Steinberg_IPluginFactory kFactory = {&kFactoryVtbl};

} // namespace

#if defined(_WIN32)
FIXTURE_EXPORT bool InitDll() { return true; }
FIXTURE_EXPORT bool ExitDll() { return true; }
#elif defined(__APPLE__)
FIXTURE_EXPORT bool bundleEntry(void*) { return true; }
FIXTURE_EXPORT bool bundleExit() { return true; }
#else
FIXTURE_EXPORT bool ModuleEntry(void*) { return true; }
FIXTURE_EXPORT bool ModuleExit() { return true; }
#endif

FIXTURE_EXPORT Steinberg_IPluginFactory* GetPluginFactory() { return &kFactory; }
