// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the AAX format wrapper (AAX Native), for Pro Tools.
//
// Written against Avid's AAX SDK, which this repository does NOT carry. The
// SDK is Avid's, under their AAX SDK licence or the GPLv3; every developer who
// builds AAX downloads it under that licence themselves and points
// SONORE_AAX_SDK_ROOT at it. What is here is our code, under Apache-2.0, and it
// is only ever compiled against a copy of the SDK the builder obtained. Shipping
// the result is the same deal it is for every other framework: the AAX
// agreement with Avid, and a PACE signature on every binary. A developer build
// of Pro Tools loads unsigned plugins, which is how this file is tested.
//
// Include it AFTER clap_wrapper.h, as formats.h does: the CLAP wrapper owns the
// machinery every format shares (the DSP instance, parameters, state, both
// editors), and this file adapts it to AAX's shape, so a .aaxplugin cannot
// behave differently from the .clap and .vst3 built from the same source.
//
// AAX splits a plugin in two, and the split drives everything below:
//
//   1. The DATA MODEL (AAX_CEffectParameters): parameters, state, latency, on
//      the host's main thread. Ours holds the shared Instance.
//   2. The ALGORITHM: a plain C callback over a "context" struct whose fields
//      the description subscribes to host services -- audio, the buffer
//      length, MIDI nodes. It is normally kept apart from the data model so
//      it can run on a DSP card. AAX Native runs it in the same process, and
//      the description says it MUST stay with the data model
//      (Constraint_Location), so one private-data field carries a pointer to
//      the data model and the callback renders through the shared Instance
//      exactly as every other format's process call does.
//
// What this does not do: AAX DSP (HDX) and AudioSuite, which need a different
// algorithm build and an offline processing model; host-drawn page tables for
// control surfaces; and main buses wider than stereo, where Pro Tools' channel
// order differs from the conventional one and a wrong order is silent.
#pragma once

#include <AAX.h>
#include <AAX_CBinaryDisplayDelegate.h>
#include <AAX_CBinaryTaperDelegate.h>
#include <AAX_CEffectGUI.h>
#include <AAX_CEffectParameters.h>
#include <AAX_CParameter.h>
#include <AAX_CString.h>
#include <AAX_Errors.h>
#include <AAX_ICollection.h>
#include <AAX_IComponentDescriptor.h>
#include <AAX_IController.h>
#include <AAX_IDisplayDelegate.h>
#include <AAX_IEffectDescriptor.h>
#include <AAX_IMIDINode.h>
#include <AAX_IPropertyMap.h>
#include <AAX_ITaperDelegate.h>
#include <AAX_ITransport.h>
#include <AAX_IViewContainer.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "audio.h"
#include "gui.h"
#include "plugin.h"
#include "presets.h"

#ifndef SONORE_NUM_PARAMS
#error "Define SONORE_NUM_PARAMS (and struct SonoreDsp) before including aax_wrapper.h"
#endif

// The four-character codes Pro Tools files the plugin under, from
// sonore_add_aax(). The manufacturer is the developer's; the product is this
// plugin's. Both are written into every session that uses it, so they are
// chosen once and never changed.
#ifndef SONORE_AAX_MANUFACTURER
#define SONORE_AAX_MANUFACTURER "Sonr"
#endif
#ifndef SONORE_AAX_PRODUCT
#define SONORE_AAX_PRODUCT ""
#endif

namespace sonore {
namespace aax {

using clapwrap::Instance;

// ── Identity ─────────────────────────────────────────────────────────────────

/** 'abcd' from "abcd": the order AAX compares and stores type IDs in. */
constexpr AAX_CTypeID fourCC(const char* s) {
  return ((AAX_CTypeID) (uint8_t) s[0] << 24) | ((AAX_CTypeID) (uint8_t) s[1] << 16) |
         ((AAX_CTypeID) (uint8_t) s[2] << 8) | (AAX_CTypeID) (uint8_t) s[3];
}

/** A code DERIVED from the plugin's reverse-URI id and a salt: stable across
 *  builds and distinct per product and per layout, without anyone choosing a
 *  code and pasting it into the source -- which a generator would get wrong
 *  once and then ship forever. Letters and digits only, an upper-case letter
 *  first, because these appear in session files and in Avid's tools. */
inline AAX_CTypeID derivedCode(const char* id, const char* salt) {
  uint64_t h = 0xcbf29ce484222325ULL;
  for (const char* p = id; p && *p; ++p) h = (h ^ (uint8_t) *p) * 0x100000001b3ULL;
  for (const char* p = salt; p && *p; ++p) h = (h ^ (uint8_t) *p) * 0x100000001b3ULL;
  static const char kAlnum[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  char code[4];
  code[0] = kAlnum[h % 26];
  h /= 26;
  for (int i = 1; i < 4; ++i) {
    code[i] = kAlnum[h % 62];
    h /= 62;
  }
  return fourCC(code);
}

inline AAX_CTypeID manufacturerId() {
  static_assert(sizeof(SONORE_AAX_MANUFACTURER) == 5, "SONORE_AAX_MANUFACTURER is four characters");
  return fourCC(SONORE_AAX_MANUFACTURER);
}

/** The product code: the one CMake was given, or one derived from the id. */
inline AAX_CTypeID productId() {
  static_assert(sizeof(SONORE_AAX_PRODUCT) == 1 || sizeof(SONORE_AAX_PRODUCT) == 5,
                "SONORE_AAX_PRODUCT is four characters, or empty to derive it from the id");
  if (sizeof(SONORE_AAX_PRODUCT) == 5) return fourCC(SONORE_AAX_PRODUCT);
  return derivedCode(kDesc.id, "aax.product");
}

/** Each layout a plugin offers is a separate plugin TYPE to Pro Tools, with an
 *  id of its own, and a session names the type it used: these are forever. */
inline AAX_CTypeID typeIdFor(uint32_t channels) {
  return derivedCode(kDesc.id, channels == 1 ? "aax.native.mono" : "aax.native.stereo");
}

inline AAX_EStemFormat stemFor(uint32_t channels) {
  return channels == 1 ? AAX_eStemFormat_Mono : AAX_eStemFormat_Stereo;
}

/** The layouts this plugin offers Pro Tools: mono and stereo, where the
 *  descriptor's range allows them. Wider buses are not offered (see the
 *  banner). */
inline bool offersWidth(uint32_t channels) {
  return channels <= 2 && clapwrap::channelCountAllowed(channels);
}

/** The ids AAX knows parameters by: the table's index, as VST3's are, because
 *  the index IS a parameter's identity in every format and every saved
 *  session. "p0", "p1", ... -- well inside AAX's 31 characters. */
inline const char* paramId(int index) {
  static char ids[SONORE_NUM_PARAMS > 0 ? SONORE_NUM_PARAMS : 1][12];
  static bool ready = false;
  if (!ready) {
    for (int i = 0; i < SONORE_NUM_PARAMS; ++i) std::snprintf(ids[i], sizeof(ids[i]), "p%d", i);
    ready = true;
  }
  return index >= 0 && index < SONORE_NUM_PARAMS ? ids[index] : "";
}

/** "p12" back to 12; -1 for anything else, the master bypass included. */
inline int paramIndexOf(AAX_CParamID id) {
  if (!id || id[0] != 'p' || id[1] < '0' || id[1] > '9') return -1;
  int n = 0;
  for (const char* c = id + 1; *c; ++c) {
    if (*c < '0' || *c > '9' || n > SONORE_NUM_PARAMS) return -1;
    n = n * 10 + (*c - '0');
  }
  return n < SONORE_NUM_PARAMS ? n : -1;
}

inline bool isBypassId(AAX_CParamID id) {
  return id && std::strcmp(id, cDefaultMasterBypassID) == 0;
}

/** Effects get Pro Tools' master bypass; instruments do not, as in every
 *  other format: there is no dry signal to pass, and a host treats an
 *  instrument's bypass as its own mute. */
inline bool hasBypass() { return !kDesc.isInstrument; }

/** Where Pro Tools files the plugin in its menus, from the descriptor's
 *  category word. An instrument is a software generator whatever it says. */
inline uint32_t categoryOf() {
  if (kDesc.isInstrument) return AAX_ePlugInCategory_SWGenerators;
  std::string c = kDesc.category ? kDesc.category : "";
  for (char& ch : c) ch = (char) std::tolower((unsigned char) ch);
  const auto has = [&](const char* word) { return c.find(word) != std::string::npos; };
  if (has("eq") || has("filter")) return AAX_ePlugInCategory_EQ;
  if (has("dynamic") || has("compress") || has("limit") || has("gate") || has("expand"))
    return AAX_ePlugInCategory_Dynamics;
  if (has("reverb")) return AAX_ePlugInCategory_Reverb;
  if (has("delay") || has("echo")) return AAX_ePlugInCategory_Delay;
  if (has("modulat") || has("chorus") || has("phaser") || has("flanger") || has("tremolo"))
    return AAX_ePlugInCategory_Modulation;
  if (has("distort") || has("saturat") || has("drive") || has("amp") || has("fuzz") ||
      has("harmonic"))
    return AAX_ePlugInCategory_Harmonic;
  if (has("pitch")) return AAX_ePlugInCategory_PitchShift;
  if (has("noise") || has("restor")) return AAX_ePlugInCategory_NoiseReduction;
  if (has("dither")) return AAX_ePlugInCategory_Dither;
  if (has("spatial") || has("pan") || has("stereo") || has("imag"))
    return AAX_ePlugInCategory_SoundField;
  if (kDesc.producesMidi) return AAX_ePlugInCategory_MIDIEffect;
  return AAX_ePlugInCategory_Effect;
}

/** "1.2.3" packed as AU packs it: major << 16 | minor << 8 | patch. */
inline uint32_t packedVersion() {
  int major = 0, minor = 0, patch = 0;
  std::sscanf(kDesc.version ? kDesc.version : "1.0.0", "%d.%d.%d", &major, &minor, &patch);
  return ((uint32_t) major << 16) | ((uint32_t) (minor & 0xff) << 8) | (uint32_t) (patch & 0xff);
}

// ── Parameters ───────────────────────────────────────────────────────────────

/** The plugin's own curve between AAX's normalised value and the plain one.
 *  Through plugin.h's functions, not a copy: the host's automation lane and
 *  the plugin's own knob have to agree about where a value sits, and that is
 *  only certain when one function answers both. */
class Taper : public AAX_ITaperDelegate<float> {
public:
  explicit Taper(int index) : index_(index) {}
  AAX_ITaperDelegate<float>* Clone() const override { return new Taper(*this); }
  float GetMaximumValue() const override { return param().maxValue; }
  float GetMinimumValue() const override { return param().minValue; }
  float ConstrainRealValue(float value) const override {
    const ParamInfo& p = param();
    const float v = clampToRange(p, value);
    return p.stepCount > 0 ? snapToStep(p, v) : v;
  }
  float NormalizedToReal(double normalized) const override {
    const ParamInfo& p = param();
    double plain = toPlainValue(p, normalized);
    // A stepped control lands ON a step, or the automation lane reads back a
    // value the plugin can never produce.
    if (p.stepCount > 0) plain = (double) snapToStep(p, (float) plain);
    return (float) plain;
  }
  double RealToNormalized(float value) const override { return toNormalisedValue(param(), value); }

private:
  const ParamInfo& param() const { return kDesc.params[index_]; }
  int index_;
};

/** The text a host shows and parses: the same functions every format's
 *  readout uses, so "-6.0 dB" reads the same in Pro Tools as in the plugin. */
class Display : public AAX_IDisplayDelegate<float> {
public:
  explicit Display(int index) : index_(index) {}
  AAX_IDisplayDelegate<float>* Clone() const override { return new Display(*this); }
  bool ValueToString(float value, AAX_CString* out) const override {
    return ValueToString(value, 64, out);
  }
  bool ValueToString(float value, int32_t maxChars, AAX_CString* out) const override {
    if (!out) return false;
    char text[96];
    formatParamValue(kDesc.params[index_], value, text, sizeof(text));
    // Pro Tools asks for a length when the readout is narrow (a control
    // surface's scribble strip): cut to it rather than let it overflow.
    if (maxChars > 0 && maxChars < (int32_t) sizeof(text)) text[maxChars] = '\0';
    out->Set(text);
    return true;
  }
  bool StringToValue(const AAX_CString& text, float* value) const override {
    return value && parseParamValue(kDesc.params[index_], text.Get(), value);
  }

private:
  int index_;
};

// ── The algorithm's context ──────────────────────────────────────────────────

class Effect;

/** What the private-data field holds: the data model the callback renders
 *  through. Written by Effect::ResetFieldData when the host sets up the
 *  instance's context. */
struct PrivateData {
  Effect* effect;
};

/**
 * The context the algorithm is called with. Each field is one host service the
 * description subscribes, by its index (AAX_FIELD_INDEX). A field this plugin
 * has no use for -- a MIDI node an effect never reads, a sidechain it does not
 * take -- is still described, as a few bytes of private data nothing reads,
 * because the host fills fields by index and an index left undescribed is a
 * field it never fills.
 */
struct RenderContext {
  float** inputs;         // main input channels, then the sidechain
  float** outputs;        // main output channels, then each aux stem's
  int32_t* numSamples;    // this call's length
  AAX_CTimestamp* clock;  // the host's sample clock
  AAX_IMIDINode* midiIn;  // notes and controllers in
  AAX_IMIDINode* midiOut; // MIDI the plugin emits
  int32_t* sideChain;     // index of the sidechain channel within `inputs`
  PrivateData* data;      // the data model
};

constexpr AAX_CFieldIndex kFieldInputs = AAX_FIELD_INDEX(RenderContext, inputs);
constexpr AAX_CFieldIndex kFieldOutputs = AAX_FIELD_INDEX(RenderContext, outputs);
constexpr AAX_CFieldIndex kFieldNumSamples = AAX_FIELD_INDEX(RenderContext, numSamples);
constexpr AAX_CFieldIndex kFieldClock = AAX_FIELD_INDEX(RenderContext, clock);
constexpr AAX_CFieldIndex kFieldMidiIn = AAX_FIELD_INDEX(RenderContext, midiIn);
constexpr AAX_CFieldIndex kFieldMidiOut = AAX_FIELD_INDEX(RenderContext, midiOut);
constexpr AAX_CFieldIndex kFieldSideChain = AAX_FIELD_INDEX(RenderContext, sideChain);
constexpr AAX_CFieldIndex kFieldData = AAX_FIELD_INDEX(RenderContext, data);

/** A sidechain is offered to an effect whose DSP takes one: Pro Tools' key
 *  input, mono. */
inline bool takesSidechain() {
  return clapwrap::TakesSidechain<SonoreDsp>::value && !kDesc.isInstrument;
}

/** Pro Tools gives aux outputs as mono or stereo stems only, so a wider aux
 *  bus is not offered, and the buses after it are not either: they are
 *  addressed by position in one array, and skipping one would move the rest. */
inline uint32_t auxStemCount() {
  uint32_t n = 0;
  while (n < clapwrap::numAuxOutputs() && clapwrap::auxBusChannels(n) <= 2) ++n;
  return n;
}

// The most frames one call brings. AAX Native's largest buffer is 1024
// (AAX_eAudioBufferLengthNative_Max); a longer call, which the host does not
// make, is rendered in slices of this rather than refused.
constexpr uint32_t kMaxFrames = 1024;

// ── The data model ───────────────────────────────────────────────────────────

/** The chunk id for the SNRS state blob: the same versioned blob every format
 *  saves, so a session moved between hosts carries the same sound. */
constexpr AAX_CTypeID kChunkId = fourCC("SNRS");

/**
 * One instance: the shared Instance every format drives, inside AAX's data
 * model. Pro Tools creates it through create(), calls the parameter and chunk
 * methods on its main thread, and the algorithm callback renders through it
 * on the audio thread (render()).
 */
class Effect : public AAX_CEffectParameters {
public:
  static AAX_CEffectParameters* AAX_CALLBACK create() { return new Effect(); }

  Effect() {
    for (int i = 0; i < SONORE_NUM_PARAMS && i < kDesc.numParams; ++i)
      shared.params[i] = kDesc.params[i].defaultValue;
    // A preset loaded from the editor reaches the host through this object's
    // parameters, not through a clap host -- the shared instance has none.
    shared.formatContext = this;
    shared.onOwnStateLoad = [](void* context) { static_cast<Effect*>(context)->syncHostParams(); };
    shared.onPresetSelected = [](void* context) { static_cast<Effect*>(context)->syncHostParams(); };
  }

  Instance shared;

  /** [main thread] State no parameter carries has changed -- a file the user
   *  picked in the editor -- and the session should be offered for saving. */
  void stateDirtyNow() { stateDirtyPending.store(true, std::memory_order_release); }

  // ── Setup ──────────────────────────────────────────────────────────────────

  AAX_Result EffectInit() override {
    if (hasBypass()) {
      std::unique_ptr<AAX_IParameter> bypass(new AAX_CParameter<bool>(
          cDefaultMasterBypassID, AAX_CString("Master Bypass"), false,
          AAX_CBinaryTaperDelegate<bool>(), AAX_CBinaryDisplayDelegate<bool>("bypass", "on"), true));
      bypass->SetNumberOfSteps(2);
      bypass->SetType(AAX_eParameterType_Discrete);
      mParameterManager.AddParameter(bypass.release());
    }
    for (int i = 0; i < SONORE_NUM_PARAMS; ++i) {
      const ParamInfo& p = kDesc.params[i];
      // A parameter the plugin REPORTS (a meter) is not the host's to write
      // or automate, as in every format.
      const bool automatable = p.automatable && !p.readOnly;
      std::unique_ptr<AAX_IParameter> param(new AAX_CParameter<float>(
          paramId(i), AAX_CString(p.label), p.defaultValue, Taper(i), Display(i), automatable));
      if (p.stepCount > 0) {
        param->SetNumberOfSteps((uint32_t) (p.stepCount > 1 ? p.stepCount : 2));
        param->SetType(AAX_eParameterType_Discrete);
      } else {
        // Pro Tools' fine-adjust resolution: 128 steps per sweep is coarse
        // for a frequency, and the host's default is coarser still.
        param->SetNumberOfSteps(1024);
        param->SetType(AAX_eParameterType_Continuous);
      }
      mParameterManager.AddParameter(param.release());
    }

    // Who is running us. Pro Tools names itself; the version is not given.
    AAX_CString host;
    if (Controller() && Controller()->GetHostName(&host) == AAX_SUCCESS)
      shared.hostInfo.name = host.Get();
    clapwrap::sendHostInfo(shared.dsp, shared.hostInfo);

    AAX_CSampleRate rate = 48000.0f;
    if (Controller()) Controller()->GetSampleRate(&rate);
    AAX_EStemFormat stem = AAX_eStemFormat_Stereo;
    if (Controller()) Controller()->GetOutputStemFormat(&stem);
    const uint32_t width = AAX_STEM_FORMAT_CHANNEL_COUNT(stem);
    shared.sampleRate = rate > 0 ? (double) rate : 48000.0;
    shared.maxFrames = kMaxFrames;
    shared.setMainLayout(width >= 1 && width <= 2 ? width : 2, nullptr);
    prepare();
    return AAX_SUCCESS;
  }

  /** [main thread] Size everything for the session: the one place allocation
   *  is allowed. Pro Tools fixes a session's rate when it opens, so this runs
   *  once per instance, before the first render. */
  void prepare() {
    clapwrap::prepareDsp(&shared);
    const uint32_t latency = clapwrap::dspLatency(shared.dsp);
    shared.reportedLatency = latency;
    // Room for the dry path to follow a latency that moves while running:
    // Pro Tools re-compensates when told, without stopping the plugin.
    shared.bypass.prepare(shared.sampleRate, kMaxFrames, latency, shared.mainChannels,
                          clapwrap::HasLatency<SonoreDsp>::value
                              ? clapwrap::BypassState::roomFor(latency, shared.sampleRate)
                              : 0);
    for (auto& v : scScratch) v.assign(kMaxFrames, 0.0f);
    if (Controller()) Controller()->SetSignalLatency((int32_t) latency);
    prepared.store(true, std::memory_order_release);
  }

  /** The algorithm's private data: a pointer back to this object. Called by
   *  the host when it sets up (or resets) the instance's context. */
  AAX_Result ResetFieldData(AAX_CFieldIndex field, void* data, uint32_t size) const override {
    if (field == kFieldData) {
      if (!data || size != sizeof(PrivateData)) return AAX_ERROR_INVALID_ARGUMENT;
      std::memset(data, 0, size);
      static_cast<PrivateData*>(data)->effect = const_cast<Effect*>(this);
      return AAX_SUCCESS;
    }
    return AAX_CEffectParameters::ResetFieldData(field, data, size);
  }

  // ── Parameters ─────────────────────────────────────────────────────────────

  /** Every change, from automation, the host's controls or our own editor, as
   *  the host settles it: into the value the audio thread reads. */
  AAX_Result UpdateParameterNormalizedValue(AAX_CParamID id, double value,
                                            AAX_EUpdateSource source) override {
    const AAX_Result result = AAX_CEffectParameters::UpdateParameterNormalizedValue(id, value, source);
    if (result != AAX_SUCCESS) return result;
    if (isBypassId(id)) {
      shared.bypass.engaged = value >= 0.5;
    } else {
      const int index = paramIndexOf(id);
      if (index >= 0) {
        const ParamInfo& p = kDesc.params[index];
        float plain = (float) toPlainValue(p, value);
        if (p.stepCount > 0) plain = snapToStep(p, plain);
        shared.params[index] = clampToRange(p, plain);
      }
    }
    return AAX_SUCCESS;
  }

  /** [main thread] The host's copy of every parameter, from the shared
   *  values: after the plugin loaded state of its own -- a preset from the
   *  editor -- so Pro Tools' readouts and control surfaces show it. As
   *  current values, not edits: a preset choice must not write a point on
   *  every automation lane. */
  void syncHostParams() {
    for (int i = 0; i < SONORE_NUM_PARAMS; ++i) {
      const ParamInfo& p = kDesc.params[i];
      AAX_CEffectParameters::UpdateParameterNormalizedValue(
          paramId(i), toNormalisedValue(p, (float) shared.params[i]), AAX_eUpdateSource_Chunk);
    }
    ++mNumPlugInChanges;
  }

  // ── State ──────────────────────────────────────────────────────────────────
  // One chunk, the SNRS blob, in place of the AAX library's parameter chunk:
  // a sampler's file, the editor size and the preset the session was on are
  // not parameters, and a chunk of parameters alone would lose them.

  AAX_Result GetNumberOfChunks(int32_t* count) const override {
    if (!count) return AAX_ERROR_NULL_ARGUMENT;
    *count = 1;
    return AAX_SUCCESS;
  }

  AAX_Result GetChunkIDFromIndex(int32_t index, AAX_CTypeID* id) const override {
    if (!id) return AAX_ERROR_NULL_ARGUMENT;
    if (index != 0) return AAX_ERROR_INVALID_CHUNK_INDEX;
    *id = kChunkId;
    return AAX_SUCCESS;
  }

  /** Builds the blob and answers its size; GetChunk then hands over that
   *  same blob, so the size the host allocated for is the size it gets. */
  AAX_Result GetChunkSize(AAX_CTypeID id, uint32_t* size) const override {
    if (!size) return AAX_ERROR_NULL_ARGUMENT;
    if (id != kChunkId) return AAX_ERROR_INVALID_CHUNK_ID;
    saveBlob(chunkOut);
    *size = (uint32_t) chunkOut.size();
    return AAX_SUCCESS;
  }

  AAX_Result GetChunk(AAX_CTypeID id, AAX_SPlugInChunk* chunk) const override {
    if (!chunk) return AAX_ERROR_NULL_ARGUMENT;
    if (id != kChunkId) return AAX_ERROR_INVALID_CHUNK_ID;
    if (chunkOut.empty()) saveBlob(chunkOut); // a host that skipped GetChunkSize
    chunk->fSize = (int32_t) chunkOut.size();
    chunk->fVersion = (int32_t) clapwrap::kStateVersion;
    chunk->fChunkID = kChunkId;
    std::memcpy(chunk->fData, chunkOut.data(), chunkOut.size());
    chunkOut.clear();
    return AAX_SUCCESS;
  }

  /** A session restored, one of Pro Tools' own presets, or a compare. The
   *  bypass stays where the HOST has it: Pro Tools owns its insert's bypass,
   *  and a preset saved bypassed must not bypass the track it is loaded on. */
  AAX_Result SetChunk(AAX_CTypeID id, const AAX_SPlugInChunk* chunk) override {
    if (!chunk) return AAX_ERROR_NULL_ARGUMENT;
    if (id != kChunkId || chunk->fSize < 0) return AAX_ERROR_INVALID_CHUNK_ID;
    const bool bypassed = shared.bypass.engaged;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(chunk->fData);
    const size_t total = (size_t) chunk->fSize;
    size_t pos = 0;
    const bool ok = clapwrap::loadStateBody(&shared, [&](void* out, size_t want) -> size_t {
      const size_t take = want < total - pos ? want : total - pos;
      if (take) std::memcpy(out, bytes + pos, take);
      pos += take;
      return take;
    });
    shared.bypass.engaged = bypassed;
    if (!ok) return AAX_ERROR_INCORRECT_CHUNK_SIZE;
    syncHostParams();
    return AAX_SUCCESS;
  }

  /** Pro Tools' compare light: is the plugin still at the chunk it was given? */
  AAX_Result CompareActiveChunk(const AAX_SPlugInChunk* chunk, AAX_CBoolean* equal) const override {
    if (!chunk || !equal) return AAX_ERROR_NULL_ARGUMENT;
    if (chunk->fChunkID != kChunkId) {
      *equal = false;
      return AAX_SUCCESS;
    }
    std::vector<uint8_t> now;
    saveBlob(now);
    *equal = chunk->fSize == (int32_t) now.size() &&
             std::memcmp(chunk->fData, now.data(), now.size()) == 0;
    return AAX_SUCCESS;
  }

  // ── The main thread's clock ────────────────────────────────────────────────

  /** Pro Tools calls this regularly on its main thread: where what the audio
   *  thread noticed is told to the host, which it may not do itself. */
  AAX_Result TimerWakeup() override {
    // A latency that moved -- a look-ahead set longer -- is re-compensated
    // by the host when told, with the plugin running.
    const uint32_t latency = clapwrap::dspLatency(shared.dsp);
    if (latency != shared.reportedLatency && Controller()) {
      shared.reportedLatency = latency;
      Controller()->SetSignalLatency((int32_t) latency);
    }
    // State no parameter carries (a sampler's file): counted as a change, so
    // the session is offered for saving.
    if (stateDirtyPending.exchange(false, std::memory_order_acq_rel)) ++mNumPlugInChanges;
    clapwrap::reportOutputGuard(&shared);
    return AAX_CEffectParameters::TimerWakeup();
  }

  AAX_Result NotificationReceived(AAX_CTypeID type, const void* data, uint32_t size) override {
    switch (type) {
      case AAX_eNotificationEvent_EnteringOfflineMode:
        shared.offline = true;
        break;
      case AAX_eNotificationEvent_ExitingOfflineMode:
        shared.offline = false;
        break;
      case AAX_eNotificationEvent_SideChainBeingConnected:
        sidechainConnected.store(true, std::memory_order_relaxed);
        break;
      case AAX_eNotificationEvent_SideChainBeingDisconnected:
        sidechainConnected.store(false, std::memory_order_relaxed);
        break;
      case AAX_eNotificationEvent_TrackNameChanged:
        // The track's name: `data` is the host's AAX_IString itself.
        if (data) {
          const AAX_IString* name = static_cast<const AAX_IString*>(data);
          shared.trackInfo.name = name->Get() ? name->Get() : "";
          clapwrap::sendTrackInfo(shared.dsp, shared.trackInfo);
        }
        break;
      default:
        break;
    }
    return AAX_CEffectParameters::NotificationReceived(type, data, size);
  }

  // ── Rendering ──────────────────────────────────────────────────────────────

  /** [audio thread] One call's audio, through the shared machinery. */
  void render(RenderContext* ctx) {
    const int32_t total = ctx->numSamples ? *ctx->numSamples : 0;
    if (total <= 0 || !ctx->outputs) return;
    const uint32_t nch = shared.mainChannels;
    if (!prepared.load(std::memory_order_acquire)) {
      // Rendered before the data model was set up: a host bug, answered with
      // silence rather than with a DSP whose buffers prepare() never sized.
      for (uint32_t c = 0; c < nch; ++c)
        if (ctx->outputs[c]) std::memset(ctx->outputs[c], 0, sizeof(float) * (size_t) total);
      return;
    }

    // The host's MIDI and the editor's edits and keys, for the whole call,
    // before it is sliced: a key the editor queued lands at the call's start.
    shared.midi.clear();
    if (clapwrap::wantsMidiIn() && ctx->midiIn) readMidi(ctx->midiIn, total);
    drainUiEvents();

    for (int32_t done = 0; done < total;) {
      const uint32_t frames = (uint32_t) (total - done < (int32_t) kMaxFrames ? total - done : (int32_t) kMaxFrames);
      renderSlice(ctx, (uint32_t) done, frames);
      done += (int32_t) frames;
    }
    shared.midi.clear();
  }

private:
  /** [audio thread] Notes and controllers from Pro Tools' MIDI node, at their
   *  offsets within the call. */
  void readMidi(AAX_IMIDINode* node, int32_t frames) {
    AAX_CMidiStream* stream = node->GetNodeBuffer();
    if (!stream || !stream->mBuffer) return;
    for (uint32_t i = 0; i < stream->mBufferSize; ++i) {
      const AAX_CMidiPacket& packet = stream->mBuffer[i];
      if (packet.mLength < 1 || packet.mLength > 3 || shared.midi.isFull()) continue;
      const int status = packet.mData[0];
      if (shortMessageLength(status) <= 0) continue; // SysEx and stray bytes
      const int offset = (int) packet.mTimestamp < frames ? (int) packet.mTimestamp : frames - 1;
      shared.midi.addEvent(MidiMessage(status, packet.mLength > 1 ? packet.mData[1] : 0,
                                       packet.mLength > 2 ? packet.mData[2] : 0),
                           offset < 0 ? 0 : offset);
    }
  }

  /** [audio thread] The editor's edits and keys, for this call. The host was
   *  already told from the main thread; this only has to reach the DSP. */
  void drainUiEvents() {
    UiEventQueue::Event e;
    while (shared.uiEvents.pop(&e)) {
      switch (e.kind) {
        case UiEventQueue::Event::Kind::ParamSet:
          if (e.index >= 0 && e.index < SONORE_NUM_PARAMS)
            shared.params[e.index] = clampToRange(kDesc.params[e.index], e.value);
          break;
        case UiEventQueue::Event::Kind::NoteOn: {
          const int velocity = (int) e.value;
          shared.midi.addEvent(MidiMessage::noteOn(0, e.index, velocity < 1 ? 1 : velocity), 0);
          break;
        }
        case UiEventQueue::Event::Kind::NoteOff:
          shared.midi.addEvent(MidiMessage::noteOff(0, e.index), 0);
          break;
        default:
          break;
      }
    }
  }

  /** [audio thread] The host's timeline. Pro Tools counts in ticks, 960,000
   *  to the quarter note, and says where the bar starts as bars, beats and
   *  ticks into the beat; everything else in TransportInfo is quarter notes. */
  TransportInfo readTransport() {
    TransportInfo info;
    const AAX_ITransport* t = Transport();
    if (!t) return info;
    double tempo = 0.0;
    if (t->GetCurrentTempo(&tempo) == AAX_SUCCESS && tempo > 0.0) {
      info.tempo = tempo;
      info.hasTempo = true;
    }
    int32_t num = 0, den = 0;
    if (t->GetCurrentMeter(&num, &den) == AAX_SUCCESS && num > 0 && den > 0) {
      info.timeSigNumerator = num;
      info.timeSigDenominator = den;
    }
    bool playing = false;
    if (t->IsTransportPlaying(&playing) == AAX_SUCCESS) info.isPlaying = playing;
    uint32_t perQuarter = 0;
    int64_t ticks = 0;
    if (t->GetTicksPerQuarter(&perQuarter) == AAX_SUCCESS && perQuarter > 0 &&
        t->GetCurrentTickPosition(&ticks) == AAX_SUCCESS) {
      info.positionBeats = (double) ticks / (double) perQuarter;
      info.hasBeats = true;
      bool looping = false;
      int64_t loopStart = 0, loopEnd = 0;
      if (t->GetCurrentLoopPosition(&looping, &loopStart, &loopEnd) == AAX_SUCCESS) {
        info.isLooping = looping;
        if (looping && loopEnd > loopStart) {
          info.loopStartBeats = (double) loopStart / (double) perQuarter;
          info.loopEndBeats = (double) loopEnd / (double) perQuarter;
          info.hasLoop = true;
        }
      }
      int64_t sample = 0;
      int32_t bars = 0, beats = 0;
      int64_t beatTicks = 0;
      uint32_t perBeat = 0;
      if (t->GetCurrentNativeSampleLocation(&sample) == AAX_SUCCESS) {
        if (shared.sampleRate > 0.0) {
          info.positionSeconds = (double) sample / shared.sampleRate;
          info.hasSeconds = true;
        }
        if (t->GetBarBeatPosition(&bars, &beats, &beatTicks, sample) == AAX_SUCCESS &&
            t->GetCurrentTicksPerBeat(&perBeat) == AAX_SUCCESS && bars > 0 && beats > 0) {
          const double intoBar = ((double) (beats - 1) * perBeat + (double) beatTicks) / perQuarter;
          info.barStartBeats = info.positionBeats - intoBar;
          info.barNumber = bars - 1;
        }
      }
    }
    return info;
  }

  void renderSlice(RenderContext* ctx, uint32_t offset, uint32_t frames) {
    const auto blockStart = std::chrono::steady_clock::now(); // the load meter's clock
    const uint32_t nch = shared.mainChannels;
    float* chans[2] = {nullptr, nullptr};
    for (uint32_t c = 0; c < nch; ++c) chans[c] = ctx->outputs[c] + offset;

    // AAX hands input and output as separate buffers, which may or may not be
    // the same memory. The wrappers process in place, so the input is copied
    // into the output first -- or, for an instrument, the output starts silent.
    for (uint32_t c = 0; c < nch; ++c) {
      const float* in = (!kDesc.isInstrument && ctx->inputs) ? ctx->inputs[c] : nullptr;
      if (in) {
        if (in + offset != chans[c]) std::memmove(chans[c], in + offset, sizeof(float) * frames);
      } else {
        std::memset(chans[c], 0, sizeof(float) * frames);
      }
    }

    // The key input: Pro Tools' sidechain is one channel, at the index the
    // context names. Copied, so the DSP gets the two-channel block every
    // format gives it and cannot write into the host's input.
    float* scChans[2] = {nullptr, nullptr};
    if constexpr (clapwrap::TakesSidechain<SonoreDsp>::value) {
      if (!kDesc.isInstrument) {
        scChans[0] = scScratch[0].data();
        scChans[1] = scScratch[1].data();
        std::memset(scChans[0], 0, sizeof(float) * frames);
        const int32_t index = ctx->sideChain ? *ctx->sideChain : -1;
        if (ctx->inputs && index >= (int32_t) nch && sidechainConnected.load(std::memory_order_relaxed) &&
            ctx->inputs[index])
          std::memcpy(scChans[0], ctx->inputs[index] + offset, sizeof(float) * frames);
        std::memcpy(scChans[1], scChans[0], sizeof(float) * frames);
      }
    }

    if (clapwrap::HasLatency<SonoreDsp>::value)
      shared.bypass.follow(clapwrap::dspLatency(shared.dsp));
    clapwrap::bypassCapture(shared.bypass, chans, nch, frames);
    AudioBlock<float> block(chans, nch, (size_t) frames);
    AudioBlock<float> scBlock(scChans, 2, scChans[0] ? (size_t) frames : 0);

    // Aux stems write straight into the host's buffers, after the main ones.
    // A host that described them and then gave no buffer gets none written:
    // the DSP is handed no aux buses rather than a null pointer.
    uint32_t nAux = auxStemCount();
    {
      uint32_t flat = nch;
      for (uint32_t b = 0; b < nAux; ++b)
        for (uint32_t c = 0; c < clapwrap::auxBusChannels(b); ++c, ++flat)
          if (!ctx->outputs[flat]) nAux = 0;
    }
    AudioBlock<float> auxBlocks[clapwrap::kMaxAuxOutputs] = {};
    float* auxPtrs[clapwrap::kMaxAuxOutputs][2] = {};
    {
      uint32_t flat = nch;
      for (uint32_t b = 0; b < nAux; ++b) {
        const uint32_t width = clapwrap::auxBusChannels(b);
        for (uint32_t c = 0; c < width; ++c, ++flat) {
          auxPtrs[b][c] = ctx->outputs[flat] + offset;
          std::memset(auxPtrs[b][c], 0, sizeof(float) * frames);
        }
        auxBlocks[b] = AudioBlock<float>(auxPtrs[b], width, (size_t) frames);
      }
    }

    // This slice's share of the call's MIDI: the events inside it, moved to
    // its own start. A call is a single slice in practice (see kMaxFrames).
    MidiBuffer& midi = sliceMidi;
    midi.clear();
    for (const auto& e : shared.midi) {
      if (e.samplePosition < (int) offset || e.samplePosition >= (int) (offset + frames)) continue;
      midi.addEvent(e.message, e.samplePosition - (int) offset);
    }

    shared.midiOut.clear();
    uint8_t roles[clapwrap::kMaxAudioChannels];
    const uint32_t numRoles = shared.mainRoles(roles, clapwrap::kMaxAudioChannels);
    shared.expression.clear();
    midi.clampPositions((int) frames);
    if (kDesc.supportsMpe) shared.mpe.process(midi, shared.expression);
    ProcessContext pc{block,
                      auxBlocks,
                      nAux,
                      scBlock,
                      midi,
                      shared.midiOut,
                      numRoles ? roles : nullptr,
                      &shared.expression,
                      nullptr, // every aux stem Pro Tools gives is in use
                      true,
                      shared.offline};
    trackNotes(shared.notes, pc.midi);
    if (clapwrap::WantsTransport<SonoreDsp>::value)
      clapwrap::sendTransport(shared.dsp, readTransport());
    clapwrap::snapshotParams(&shared);
    clapwrap::runDspCtx(shared.dsp, pc, shared.paramsBlock);

    // MIDI the DSP emitted, to Pro Tools' output node at its offsets.
    if (kDesc.producesMidi && ctx->midiOut) {
      for (const auto& e : shared.midiOut) {
        AAX_CMidiPacket packet{};
        packet.mData[0] = (unsigned char) e.message.getRawStatus();
        packet.mData[1] = (unsigned char) e.message.getRawData1();
        packet.mData[2] = (unsigned char) e.message.getRawData2();
        const int length = shortMessageLength(packet.mData[0]);
        if (length <= 0) continue;
        packet.mLength = (uint32_t) length;
        packet.mTimestamp = (uint32_t) ((e.samplePosition < 0 ? 0 : e.samplePosition) + (int) offset);
        packet.mIsImmediate = false;
        ctx->midiOut->PostMIDIPacket(&packet);
      }
    }

    // Recorded, not announced: the host is told from TimerWakeup.
    if (clapwrap::dspConsumeStateDirty(shared.dsp))
      stateDirtyPending.store(true, std::memory_order_release);
    clapwrap::bypassApply(shared.bypass, chans, nch, frames);
    // NaN or infinity on any bus leaves as silence (output_guard.h).
    clapwrap::guardOutputs(&shared, chans, nch, auxBlocks, nAux, frames);

    shared.meter.push(measureBlock(chans[0], (size_t) frames));
    shared.scope.push(chans, 1, frames);
    shared.load.record(
        std::chrono::duration<double>(std::chrono::steady_clock::now() - blockStart).count(), frames);
  }

  void saveBlob(std::vector<uint8_t>& out) const {
    out.clear();
    clapwrap::saveStateBody(&shared, [&](const void* data, size_t size) -> size_t {
      const uint8_t* p = static_cast<const uint8_t*>(data);
      out.insert(out.end(), p, p + size);
      return size;
    });
  }

  std::atomic<bool> prepared{false};
  std::atomic<bool> stateDirtyPending{false};
  // Assumed connected until Pro Tools says otherwise: it announces a key
  // input being disconnected, and an index into the inputs is only given
  // while there is one.
  std::atomic<bool> sidechainConnected{true};
  std::vector<float> scScratch[2];
  MidiBuffer sliceMidi;
  mutable std::vector<uint8_t> chunkOut;
};

/** The algorithm entry point: every instance Pro Tools is running in this
 *  call, each through its own data model. */
inline void AAX_CALLBACK renderAudio(RenderContext* const begin[], const void* end) {
  for (RenderContext* const* walk = begin; walk < end; ++walk) {
    RenderContext* ctx = *walk;
    if (ctx && ctx->data && ctx->data->effect) ctx->data->effect->render(ctx);
  }
}

// ── The editor ───────────────────────────────────────────────────────────────

/**
 * The plugin's window inside Pro Tools'. The same choice of editor the other
 * formats make -- native, or the page in the OS webview -- embedded in the
 * view container the host gives (an HWND on Windows, an NSView on macOS).
 */
class Gui : public AAX_CEffectGUI {
public:
  static AAX_IEffectGUI* AAX_CALLBACK create() { return new Gui(); }

  /** Closed here as well as in DeleteViewContainer: a host that tears the
   *  GUI down without taking the container away first would otherwise leave
   *  a child window pointing at a data model that is about to go. While the
   *  data model is still reachable, which it is not in the destructor. */
  AAX_Result Uninitialize() override {
    closeEditor();
    return AAX_CEffectGUI::Uninitialize();
  }

  /** In the window's own pixels: the logical size times the display scale
   *  the editor was opened at (see windowScale). */
  AAX_Result GetViewSize(AAX_Point* size) const override {
    if (!size) return AAX_ERROR_NULL_ARGUMENT;
    const Effect* e = effect();
    size->horz = (float) (e ? e->shared.guiWidth : (uint32_t) SONORE_UI_WIDTH) * scale_;
    size->vert = (float) (e ? e->shared.guiHeight : (uint32_t) SONORE_UI_HEIGHT) * scale_;
    return AAX_SUCCESS;
  }

  AAX_Result ParameterUpdated(AAX_CParamID) override {
    // Nothing to push: both editors read the shared values on their own clock.
    return AAX_SUCCESS;
  }

protected:
  void CreateViewContents() override {}

  void CreateViewContainer() override {
    Effect* e = effect();
    void* parent = GetViewContainerPtr();
    if (!e || !parent) return;
    const AAX_EViewContainer_Type type = GetViewContainerType();
#if defined(_WIN32)
    if (type != AAX_eViewContainer_Type_HWND) return;
#elif defined(__APPLE__)
    if (type != AAX_eViewContainer_Type_NSView) return;
#endif
    openEditor(e, parent);
  }

  void DeleteViewContainer() override { closeEditor(); }

private:
  Effect* effect() { return dynamic_cast<Effect*>(GetEffectParameters()); }

  /** What a logical pixel is worth in the window Pro Tools gave. AAX has no
   *  call that says, unlike VST3's content scale and CLAP's set_scale, so it is
   *  read off the window: on Windows its DPI, which for a host that is not
   *  DPI-aware is 96, and Windows scales the whole window instead. macOS
   *  windows are in points already. GetDpiForWindow is resolved at runtime,
   *  as the native window does it: a missing import is a plugin that fails to
   *  load, with no message, on an older Windows. */
  static float windowScale(void* parent) {
#if defined(_WIN32)
    typedef UINT(WINAPI * GetDpiForWindowFn)(HWND);
    static const GetDpiForWindowFn fn = [] {
      HMODULE user32 = GetModuleHandleW(L"user32.dll");
      return user32 ? (GetDpiForWindowFn) (void*) GetProcAddress(user32, "GetDpiForWindow") : nullptr;
    }();
    if (fn && parent) {
      const UINT dpi = fn((HWND) parent);
      if (dpi > 0) return (float) dpi / 96.0f;
    }
#else
    (void) parent;
#endif
    return 1.0f;
  }

  /** The host's window, at the editor's size in its own pixels. */
  void fitHostWindow(Effect* e) {
    if (!GetViewContainer()) return;
    AAX_Point size((float) e->shared.guiHeight * scale_, (float) e->shared.guiWidth * scale_);
    GetViewContainer()->SetViewSize(size);
  }
  const Effect* effect() const { return dynamic_cast<const Effect*>(GetEffectParameters()); }

  /** Ask Pro Tools for a new window size; it is the host's window. */
  void requestResize(int width, int height) {
    Effect* e = effect();
    if (!e || !GetViewContainer()) return;
    uint32_t w = width > 0 ? (uint32_t) width : 1u;
    uint32_t h = height > 0 ? (uint32_t) height : 1u;
    clapwrap::clampEditorSize(&w, &h);
    AAX_Point size((float) h * scale_, (float) w * scale_);
    if (GetViewContainer()->SetViewSize(size) != AAX_SUCCESS) return;
    e->shared.guiWidth = w;
    e->shared.guiHeight = h;
    // The native editor lays out in logical units at its own scale; the
    // webview's window is made in the window's pixels.
    if (e->shared.guiIsNative) e->shared.nativeEditor.setSize((int) w, (int) h);
#if defined(SONORE_HAS_WEBVIEW_BACKEND)
    else e->shared.webview.setSize((uint32_t) ((float) w * scale_), (uint32_t) ((float) h * scale_));
#endif
  }

  /** The shared editor host, with edits reaching Pro Tools as well as the DSP.
   *  An edit the host is not told about records no automation, makes no undo
   *  step and is overwritten by the next automation read; one the DSP is not
   *  told about is recorded and never heard. Both, on this (main) thread. */
  gfx::EditorHost makeHost(Effect* e) {
    gfx::EditorHost host = clapwrap::makeEditorHost(&e->shared);
    host.setParameter = [e, queue = host.setParameter](int index, float plain) {
      queue(index, plain);
      if (!uiMayWrite(kDesc, index)) return;
      const ParamInfo& p = kDesc.params[index];
      e->SetParameterNormalizedValue(paramId(index), toNormalisedValue(p, clampToRange(p, plain)));
    };
    host.beginGesture = [e, queue = host.beginGesture](int index) {
      queue(index);
      if (uiMayWrite(kDesc, index)) e->TouchParameter(paramId(index));
    };
    host.endGesture = [e, queue = host.endGesture](int index) {
      queue(index);
      if (uiMayWrite(kDesc, index)) e->ReleaseParameter(paramId(index));
    };
    // Pro Tools has no context menu to offer a plugin control.
    host.showContextMenu = [](int, int, int) {};
    return host;
  }

  void prepareNative(Effect* e) {
    e->shared.nativeEditor.setResizeLimits(kDesc.editorLimits);
    e->shared.nativeEditor.onRequestResize = [this](int w, int h) { requestResize(w, h); };
    e->shared.nativeEditor.setContentFactory(clapwrap::customEditorFactory(e->shared.dsp));
  }

  bool openNative(Effect* e, void* parent) {
    prepareNative(e);
    if (!gfx::NativeEditor::isAvailable() ||
        !e->shared.nativeEditor.open(parent, kDesc.params, (int) kDesc.numParams, makeHost(e),
                                     (int) e->shared.guiWidth, (int) e->shared.guiHeight))
      return false;
    // The scale apart from the size: the editor lays its tree out in logical
    // units, and handing it the device size would draw every control at
    // half its size on a 200% display.
    if (scale_ != 1.0f) e->shared.nativeEditor.setScale(scale_);
    e->shared.guiIsNative = true;
    return true;
  }

  void openEditor(Effect* e, void* parent) {
    e->shared.uiEchoValid = false;
    const EditorChoice choice = clapwrap::editorChoice();
    if (choice.backend == EditorBackend::None) return;
    const float scale = windowScale(parent);
    const bool rescaled = scale != scale_;
    scale_ = scale;
    // The host sized its window from GetViewSize before it knew the scale.
    if (rescaled) fitHostWindow(e);
    if (choice.backend == EditorBackend::Native) {
      open_ = openNative(e, parent);
      return;
    }
#if defined(SONORE_HAS_WEBVIEW_BACKEND)
    e->shared.guiIsNative = false;
    e->shared.webview.onMessage = [this, e](const BridgeMessage& m) { onMessage(e, m); };
    e->shared.webview.onTick = [e]() { tick(e); };
#if defined(_WIN32)
    // In the window's pixels: the page is laid out in logical ones and the
    // webview draws each at the window's scale.
    const bool ok = e->shared.webview.create(
        (HWND) parent, (uint32_t) ((float) e->shared.guiWidth * scale_),
        (uint32_t) ((float) e->shared.guiHeight * scale_), clapwrap::uiHtml(),
        clapwrap::bridgeScriptFor(e->shared), kDesc.id);
#else
    const bool ok = e->shared.webview.create(parent, e->shared.guiWidth, e->shared.guiHeight,
                                             clapwrap::uiHtml(), clapwrap::bridgeScriptFor(e->shared),
                                             kDesc.id);
#endif
    if (ok) {
      e->shared.webview.setVisible(true);
      open_ = true;
      return;
    }
    // No webview runtime: the native editor needs none, so the plugin loses
    // its designed face and keeps every control, as in the other formats.
#endif
    open_ = openNative(e, parent);
  }

  void closeEditor() {
    if (!open_) return;
    open_ = false;
    Effect* e = effect();
    if (!e) return;
    e->shared.nativeEditor.close();
    // The resize callback captured this object, and the editor outlives it.
    e->shared.nativeEditor.onRequestResize = nullptr;
    e->shared.guiIsNative = false;
#if defined(SONORE_HAS_WEBVIEW_BACKEND)
    e->shared.webview.destroy();
#endif
  }

  /** The ~30 Hz editor clock: push whatever the page does not know yet. */
  static void tick(Effect* e) {
    if (e->shared.guiIsNative) {
      e->shared.nativeEditor.tick();
      return;
    }
#if defined(SONORE_HAS_WEBVIEW_BACKEND)
    if (!e->shared.webview.ready()) return;
    e->shared.webview.eval(clapwrap::uiTickScript(e->shared));
#endif
  }

  /** A message from the page, on the main thread: edits to the host and the
   *  DSP both, as the native editor's host does above. */
  void onMessage(Effect* e, const BridgeMessage& message) {
    if ((message.kind == BridgeMessage::Kind::Set || message.kind == BridgeMessage::Kind::GestureBegin ||
         message.kind == BridgeMessage::Kind::GestureEnd) &&
        !uiMayWrite(kDesc, message.index))
      return;
    UiEventQueue::Event ev;
    switch (message.kind) {
      case BridgeMessage::Kind::Ready:
        clapwrap::pageReady(&e->shared);
        return;
#if defined(SONORE_HAS_WEBVIEW_BACKEND)
      case BridgeMessage::Kind::CaptureKeys:
        e->shared.webview.captureKeys = message.value >= 0.5;
        return;
      case BridgeMessage::Kind::ChooseFile: {
        const std::string path = FileDialog::byMode(message.mode, e->shared.webview.nativeWindow());
        clapwrap::sendFile(e->shared.dsp, message.purpose.c_str(), path.c_str());
        e->shared.uiStateDirty = true;
        // A file the plugin loaded moved no parameter; counted as a change so
        // Pro Tools offers to save the session that now needs it.
        if (!path.empty()) e->stateDirtyNow();
        e->shared.webview.eval(fileAnswerScript(message.purpose, path));
        return;
      }
#endif
      case BridgeMessage::Kind::Set: {
        if (message.index < 0 || message.index >= SONORE_NUM_PARAMS) return;
        const ParamInfo& p = kDesc.params[message.index];
        const float plain = clampToRange(p, (float) message.value);
        ev.kind = UiEventQueue::Event::Kind::ParamSet;
        ev.index = message.index;
        ev.value = plain;
        // Written here as well as queued: the page's echo must not push the
        // old value back on the next tick.
        e->shared.params[message.index] = plain;
        e->shared.uiEcho[message.index] = plain;
        e->SetParameterNormalizedValue(paramId(message.index), toNormalisedValue(p, plain));
        break;
      }
      case BridgeMessage::Kind::GestureBegin:
        if (message.index >= 0 && message.index < SONORE_NUM_PARAMS)
          e->TouchParameter(paramId(message.index));
        return;
      case BridgeMessage::Kind::GestureEnd:
        if (message.index >= 0 && message.index < SONORE_NUM_PARAMS)
          e->ReleaseParameter(paramId(message.index));
        return;
      case BridgeMessage::Kind::LoadPreset:
        // The state path, as every format's preset bar: the whole set at once,
        // and the host told the values rather than handed a burst of edits.
        clapwrap::loadPresetByIndex(&e->shared, message.index);
        return;
      case BridgeMessage::Kind::NoteOn:
        if (message.note < 0 || message.note > 127) return;
        ev.kind = UiEventQueue::Event::Kind::NoteOn;
        ev.index = message.note;
        ev.value = (float) (message.velocity < 1 ? 1 : (message.velocity > 127 ? 127 : message.velocity));
        break;
      case BridgeMessage::Kind::NoteOff:
        if (message.note < 0 || message.note > 127) return;
        ev.kind = UiEventQueue::Event::Kind::NoteOff;
        ev.index = message.note;
        break;
      default:
        return;
    }
    e->shared.uiEvents.push(ev);
  }

  bool open_ = false;
  float scale_ = 1.0f;
};

// ── Description ──────────────────────────────────────────────────────────────

/** Names at the lengths Pro Tools picks between for narrow places: the full
 *  name, then 31, 16, 8 and 4 characters. */
template <typename Add>
inline void addNames(const char* name, Add&& add) {
  const std::string full = name && *name ? name : "Sonore Plugin";
  std::string last;
  add(full.c_str());
  last = full;
  for (size_t length : {31u, 16u, 8u, 4u}) {
    if (full.size() <= length) continue;
    std::string cut = full.substr(0, length);
    while (!cut.empty() && cut.back() == ' ') cut.pop_back();
    if (cut.empty() || cut == last) continue;
    add(cut.c_str());
    last = cut;
  }
}

/** One plugin TYPE: the algorithm at one channel width. */
inline AAX_Result describeType(AAX_IEffectDescriptor* effect, uint32_t channels) {
  AAX_IComponentDescriptor* comp = effect->NewComponentDescriptor();
  if (!comp) return AAX_ERROR_NULL_OBJECT;
  AAX_Result err = AAX_SUCCESS;
  const auto check = [&](AAX_Result r) { if (err == AAX_SUCCESS && r != AAX_SUCCESS) err = r; };
  const auto unused = [&](AAX_CFieldIndex field) {
    check(comp->AddPrivateData(field, sizeof(float), AAX_ePrivateDataOptions_DefaultOptions));
  };

  check(comp->AddAudioIn(kFieldInputs));
  check(comp->AddAudioOut(kFieldOutputs));
  check(comp->AddAudioBufferLength(kFieldNumSamples));
  check(comp->AddClock(kFieldClock));
  if (clapwrap::wantsMidiIn())
    check(comp->AddMIDINode(kFieldMidiIn, AAX_eMIDINodeType_LocalInput, kDesc.name, 0xffff));
  else
    unused(kFieldMidiIn);
  if (kDesc.producesMidi)
    check(comp->AddMIDINode(kFieldMidiOut, AAX_eMIDINodeType_LocalOutput, kDesc.name, 0xffff));
  else
    unused(kFieldMidiOut);
  if (takesSidechain())
    check(comp->AddSideChainIn(kFieldSideChain));
  else
    unused(kFieldSideChain);
  check(comp->AddPrivateData(kFieldData, sizeof(PrivateData), AAX_ePrivateDataOptions_DefaultOptions));
  for (uint32_t b = 0; b < auxStemCount(); ++b) {
    const AuxBusInfo* info = kDesc.auxOutputs ? &kDesc.auxOutputs[b] : nullptr;
    // "mono" or "stereo" in the name, as Avid asks: it is how a user finds
    // an output of the width they want in the routing menu.
    const uint32_t width = clapwrap::auxBusChannels(b);
    const std::string name = std::string(info && info->name ? info->name : "Aux") +
                             (width == 1 ? " (mono)" : " (stereo)");
    check(comp->AddAuxOutputStem(0, (int32_t) stemFor(width), name.c_str()));
  }

  AAX_IPropertyMap* props = comp->NewPropertyMap();
  if (!props) return AAX_ERROR_NULL_OBJECT;
  check(props->AddProperty(AAX_eProperty_ManufacturerID, (AAX_CPropertyValue) manufacturerId()));
  check(props->AddProperty(AAX_eProperty_ProductID, (AAX_CPropertyValue) productId()));
  check(props->AddProperty(AAX_eProperty_PlugInID_Native, (AAX_CPropertyValue) typeIdFor(channels)));
  check(props->AddProperty(AAX_eProperty_InputStemFormat, (AAX_CPropertyValue) stemFor(channels)));
  check(props->AddProperty(AAX_eProperty_OutputStemFormat, (AAX_CPropertyValue) stemFor(channels)));
  check(props->AddProperty(AAX_eProperty_CanBypass, hasBypass()));
  // The algorithm renders through the data model, so it must run beside it.
  check(props->AddProperty(AAX_eProperty_Constraint_Location,
                           (AAX_CPropertyValue) AAX_eConstraintLocationMask_DataModel));
  // One instance per track, never one per channel: the DSP is multichannel.
  check(props->AddProperty(AAX_eProperty_Constraint_MultiMonoSupport, false));
  if (clapwrap::WantsTransport<SonoreDsp>::value)
    check(props->AddProperty(AAX_eProperty_UsesTransport, true));
  if (takesSidechain())
    check(props->AddProperty(AAX_eProperty_SideChainStemFormat, (AAX_CPropertyValue) AAX_eStemFormat_Mono));
  check(comp->AddProcessProc_Native(reinterpret_cast<AAX_CProcessProc>(&renderAudio), props));
  check(effect->AddComponent(comp));
  return err;
}

inline AAX_Result describe(AAX_ICollection* collection) {
  AAX_IEffectDescriptor* effect = collection->NewDescriptor();
  if (!effect) return AAX_ERROR_NULL_OBJECT;
  AAX_Result err = AAX_SUCCESS;
  const auto check = [&](AAX_Result r) { if (err == AAX_SUCCESS && r != AAX_SUCCESS) err = r; };

  addNames(kDesc.name, [&](const char* n) { check(effect->AddName(n)); });
  check(effect->AddCategory(categoryOf()));
  check(effect->AddProcPtr(reinterpret_cast<void*>(&Effect::create), kAAX_ProcPtrID_Create_EffectParameters));
  if (clapwrap::editorChoice().backend != EditorBackend::None)
    check(effect->AddProcPtr(reinterpret_cast<void*>(&Gui::create), kAAX_ProcPtrID_Create_EffectGUI));

  AAX_IPropertyMap* props = effect->NewPropertyMap();
  if (!props) return AAX_ERROR_NULL_OBJECT;
  // With no face of its own, Pro Tools draws one from the parameters.
  if (clapwrap::editorChoice().backend == EditorBackend::None)
    check(props->AddProperty(AAX_eProperty_UsesClientGUI, true));
  // Chunks on the main thread: a state load reaches the editor and the DSP's
  // own state, neither of which belongs to any other thread.
  check(props->AddProperty(AAX_eProperty_RequiresChunkCallsOnMainThread, true));
  check(effect->SetProperties(props));

  bool any = false;
  for (uint32_t width : {1u, 2u})
    if (offersWidth(width)) {
      check(describeType(effect, width));
      any = true;
    }
  // A descriptor whose range has neither mono nor stereo has nothing to offer
  // Pro Tools: refused with a reason rather than registered empty.
  if (!any) return AAX_ERROR_INVALID_ARGUMENT;

  check(collection->AddEffect(kDesc.id, effect));
  check(collection->SetManufacturerName(kDesc.vendor && *kDesc.vendor ? kDesc.vendor : "Sonorie"));
  addNames(kDesc.name, [&](const char* n) { check(collection->AddPackageName(n)); });
  check(collection->SetPackageVersion(packedVersion()));
  return err;
}

} // namespace aax
} // namespace sonore

/** Pro Tools' entry into the plugin, called by the AAX library's exports. */
AAX_Result GetEffectDescriptions(AAX_ICollection* outCollection) {
  return outCollection ? sonore::aax::describe(outCollection) : AAX_ERROR_NULL_ARGUMENT;
}
