// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the VST3 wrapper ends a 64-bit block the way it ends a 32-bit
// one.
//
// A DSP that changes its own state -- a learned value, a captured snapshot --
// says so through consumeStateDirty(), and the wrapper marks the session
// dirty so the host offers to save. The float path asked after every block;
// the double path returned before reaching that line, so in a host running a
// 64-bit pipeline the session was never marked and closed without asking.
//
// The DSP below is written over the sample type, which is what earns the
// 64-bit path, and reports a change on every block.
//
// And a preset the page loads while audio runs reaches a block whole. The
// web page's LoadPreset wrote the values one at a time and queued each for
// the audio thread as well, so a block could run on half of one preset and
// half of the other -- the torn load every other path stopped making when
// writeParamsTogether came in. The probe's two presets set every parameter to
// 0.25 or every one to 0.75, so a block whose parameters differ is torn.
#define SONORE_BUILD_VST3 1
#define SONORE_NUM_PARAMS 8

#include <atomic>

#include <sonore/dsp.h>

static std::atomic<long long> g_blocks{0}, g_torn{0};

struct SonoreDsp {
  void prepare(const sonore::ProcessSpec&) {}
  template <typename Sample>
  void process(sonore::AudioBlock<Sample>&, const float* params) {
    for (int i = 1; i < SONORE_NUM_PARAMS; ++i)
      if (params[i] != params[0]) {
        g_torn.fetch_add(1, std::memory_order_relaxed);
        break;
      }
    g_blocks.fetch_add(1, std::memory_order_relaxed);
  }
  bool consumeStateDirty() { return true; }
};

#include <sonore/plugin.h>
#include <sonore/presets.h>

static const sonore::ParamInfo kParamTable[SONORE_NUM_PARAMS] = {
    {"p0", "P0", "", 0.0f, 1.0f, 0.5f, 0}, {"p1", "P1", "", 0.0f, 1.0f, 0.5f, 0},
    {"p2", "P2", "", 0.0f, 1.0f, 0.5f, 0}, {"p3", "P3", "", 0.0f, 1.0f, 0.5f, 0},
    {"p4", "P4", "", 0.0f, 1.0f, 0.5f, 0}, {"p5", "P5", "", 0.0f, 1.0f, 0.5f, 0},
    {"p6", "P6", "", 0.0f, 1.0f, 0.5f, 0}, {"p7", "P7", "", 0.0f, 1.0f, 0.5f, 0},
};

static const float kLow[SONORE_NUM_PARAMS] = {0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f, 0.25f};
static const float kHigh[SONORE_NUM_PARAMS] = {0.75f, 0.75f, 0.75f, 0.75f, 0.75f, 0.75f, 0.75f, 0.75f};
static const sonore::Preset kPresets[] = {
    {"Low", kLow, SONORE_NUM_PARAMS},
    {"High", kHigh, SONORE_NUM_PARAMS},
};

static const sonore::PluginDescriptor kDesc = {
    "com.sonorie.test.vst3dirty",
    "Sonore VST3 Dirty Probe",
    "Sonorie",
    "1.0.0",
    "Reports a state change on every block.",
    "https://sonorie.com",
    false,
    kParamTable,
    SONORE_NUM_PARAMS,
    kPresets,
    2,
    "utility",
    nullptr,
    nullptr,
    1,
    2,
};

#include <sonore/clap_wrapper.h>
#include <sonore/vst3_wrapper.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

static int g_checks = 0;
static int g_failures = 0;

static void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

// A host's component handler that counts what the plugin tells it.
static int g_edits = 0, g_gestures = 0, g_valueRestarts = 0;
static Steinberg_tresult SMTG_STDMETHODCALLTYPE handlerQuery(void*, const Steinberg_TUID, void** obj) {
  if (obj) *obj = nullptr;
  return Steinberg_kNoInterface;
}
static Steinberg_uint32 SMTG_STDMETHODCALLTYPE handlerRef(void*) { return 1; }
static Steinberg_tresult SMTG_STDMETHODCALLTYPE handlerBegin(void*, Steinberg_Vst_ParamID) {
  ++g_gestures;
  return Steinberg_kResultOk;
}
static Steinberg_tresult SMTG_STDMETHODCALLTYPE handlerPerform(void*, Steinberg_Vst_ParamID,
                                                              Steinberg_Vst_ParamValue) {
  ++g_edits;
  return Steinberg_kResultOk;
}
static Steinberg_tresult SMTG_STDMETHODCALLTYPE handlerEnd(void*, Steinberg_Vst_ParamID) {
  return Steinberg_kResultOk;
}
static Steinberg_tresult SMTG_STDMETHODCALLTYPE handlerRestart(void*, Steinberg_int32 flags) {
  if (flags & Steinberg_Vst_RestartFlags_kParamValuesChanged) ++g_valueRestarts;
  return Steinberg_kResultOk;
}
static Steinberg_Vst_IComponentHandlerVtbl g_handlerVtbl = {handlerQuery,   handlerRef, handlerRef,
                                                            handlerBegin,   handlerPerform,
                                                            handlerEnd,     handlerRestart};
static struct {
  Steinberg_Vst_IComponentHandlerVtbl* lpVtbl = &g_handlerVtbl;
} g_handler;

template <typename Sample>
static void runBlock(Steinberg_Vst_IAudioProcessor* proc, Steinberg_int32 sampleSize) {
  std::vector<Sample> inL(64), inR(64), outL(64), outR(64);
  Sample* in[2] = {inL.data(), inR.data()};
  Sample* out[2] = {outL.data(), outR.data()};
  Steinberg_Vst_AudioBusBuffers inBus{}, outBus{};
  inBus.numChannels = outBus.numChannels = 2;
  if constexpr (sizeof(Sample) == 4) {
    inBus.Steinberg_Vst_AudioBusBuffers_channelBuffers32 = in;
    outBus.Steinberg_Vst_AudioBusBuffers_channelBuffers32 = out;
  } else {
    inBus.Steinberg_Vst_AudioBusBuffers_channelBuffers64 = in;
    outBus.Steinberg_Vst_AudioBusBuffers_channelBuffers64 = out;
  }
  Steinberg_Vst_ProcessData data{};
  data.processMode = Steinberg_Vst_ProcessModes_kRealtime;
  data.symbolicSampleSize = sampleSize;
  data.numSamples = 64;
  data.numInputs = data.numOutputs = 1;
  data.inputs = &inBus;
  data.outputs = &outBus;
  proc->lpVtbl->process(proc, &data);
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("── VST3, the end of a block in both precisions, and a preset the page loads ──\n");
  Steinberg_IPluginFactory* factory = GetPluginFactory();
  Steinberg_PClassInfo info{};
  bool found = false;
  for (Steinberg_int32 i = 0; i < factory->lpVtbl->countClasses(factory) && !found; ++i)
    found = factory->lpVtbl->getClassInfo(factory, i, &info) == Steinberg_kResultOk &&
            std::strcmp(info.category, "Audio Module Class") == 0;
  void* object = nullptr;
  check(found && factory->lpVtbl->createInstance(factory, (Steinberg_FIDString) info.cid,
                                                 (Steinberg_FIDString) Steinberg_Vst_IComponent_iid,
                                                 &object) == Steinberg_kResultOk,
        "the probe is created through its own factory");
  if (!object) return 1;
  auto* component = (Steinberg_Vst_IComponent*) object;
  component->lpVtbl->initialize(component, nullptr);
  Steinberg_Vst_IAudioProcessor* proc = nullptr;
  component->lpVtbl->queryInterface(component, Steinberg_Vst_IAudioProcessor_iid, (void**) &proc);
  sonore::vst3::Plugin* plugin = sonore::vst3::ownerOfComponent(component);

  const Steinberg_int32 sizes[2] = {Steinberg_Vst_SymbolicSampleSizes_kSample32,
                                    Steinberg_Vst_SymbolicSampleSizes_kSample64};
  for (Steinberg_int32 size : sizes) {
    const bool doubles = size == Steinberg_Vst_SymbolicSampleSizes_kSample64;
    check(proc->lpVtbl->canProcessSampleSize(proc, size) == Steinberg_kResultOk,
          doubles ? "the probe processes 64-bit samples" : "the probe processes 32-bit samples");
    Steinberg_Vst_ProcessSetup setup{};
    setup.processMode = Steinberg_Vst_ProcessModes_kRealtime;
    setup.symbolicSampleSize = size;
    setup.maxSamplesPerBlock = 64;
    setup.sampleRate = 48000.0;
    proc->lpVtbl->setupProcessing(proc, &setup);
    component->lpVtbl->setActive(component, 1);
    proc->lpVtbl->setProcessing(proc, 1);
    plugin->stateDirty = false;
    if (doubles) runBlock<double>(proc, size);
    else runBlock<float>(proc, size);
    check((bool) plugin->stateDirty,
          doubles ? "a 64-bit block whose DSP changed its state marks the session dirty"
                  : "a 32-bit block whose DSP changed its state marks the session dirty");
    proc->lpVtbl->setProcessing(proc, 0);
    component->lpVtbl->setActive(component, 0);
  }

  // A preset the page loads is a preset load to the host, not a set of edits.
  // It told the host through performEdit for every parameter, bracketed in
  // gestures, which a host with automation in write, latch or touch mode
  // records: a point on every parameter's lane for choosing a preset. Every
  // other load the plugin makes itself -- the preset bar, an undo, the CLAP
  // build's page -- asks the host to read the values again instead
  // (restartComponent with kParamValuesChanged), which records nothing.
  {
    Steinberg_Vst_IEditController* controller = nullptr;
    component->lpVtbl->queryInterface(component, Steinberg_Vst_IEditController_iid, (void**) &controller);
    if (controller) controller->lpVtbl->setComponentHandler(controller, (Steinberg_Vst_IComponentHandler*) &g_handler);
    g_edits = g_gestures = g_valueRestarts = 0;
    sonore::BridgeMessage message;
    message.kind = sonore::BridgeMessage::Kind::LoadPreset;
    message.index = 1;
    sonore::vst3::viewOnMessage(plugin, message);
    bool loaded = true;
    for (int i = 0; i < SONORE_NUM_PARAMS; ++i) loaded = loaded && (float) plugin->shared.params[i] == 0.75f;
    char line[220];
    std::snprintf(line, sizeof(line),
                  "a preset the page loads is applied (%s) and announced as one, not recorded as edits "
                  "(%d edits, %d gestures, %d re-reads)",
                  loaded ? "yes" : "no", g_edits, g_gestures, g_valueRestarts);
    check(loaded && g_edits == 0 && g_gestures == 0 && g_valueRestarts == 1, line);
    check(plugin->currentProgram() == 1, "...and it is the program the host shows");

    // The program's name, as the plugin prints it, read back as the program.
    // getParamValueByString had no case for the program parameter, so the
    // name it had just printed was refused as an unknown id.
    if (controller) {
      Steinberg_Vst_String128 text{};
      Steinberg_Vst_ParamValue back = -1.0;
      const bool printed = controller->lpVtbl->getParamStringByValue(controller, sonore::vst3::kProgramParamId,
                                                                      1.0, text) == Steinberg_kResultOk;
      const bool parsed = controller->lpVtbl->getParamValueByString(controller, sonore::vst3::kProgramParamId,
                                                                     text, &back) == Steinberg_kResultOk;
      std::snprintf(line, sizeof(line), "the program's name \"%s\" reads back as the program (%s, %.2f)",
                    sonore::vst3::utf16ToUtf8(text, 128).c_str(), parsed ? "read" : "refused", back);
      check(printed && parsed && back == 1.0, line);
    }
    if (controller) {
      controller->lpVtbl->setComponentHandler(controller, nullptr);
      controller->lpVtbl->release(controller);
    }
  }

  // The page's LoadPreset, in turn between the two presets on this thread,
  // while another runs blocks as an audio thread does.
  {
    Steinberg_Vst_ProcessSetup setup{};
    setup.processMode = Steinberg_Vst_ProcessModes_kRealtime;
    setup.symbolicSampleSize = Steinberg_Vst_SymbolicSampleSizes_kSample32;
    setup.maxSamplesPerBlock = 64;
    setup.sampleRate = 48000.0;
    proc->lpVtbl->setupProcessing(proc, &setup);
    component->lpVtbl->setActive(component, 1);
    proc->lpVtbl->setProcessing(proc, 1);
    g_blocks.store(0);
    g_torn.store(0);
    std::atomic<bool> stop{false};
    std::thread audio([&] {
      while (!stop.load(std::memory_order_acquire))
        runBlock<float>(proc, Steinberg_Vst_SymbolicSampleSizes_kSample32);
    });
    long long loads = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < until) {
      sonore::BridgeMessage message;
      message.kind = sonore::BridgeMessage::Kind::LoadPreset;
      message.index = (int) (loads & 1);
      sonore::vst3::viewOnMessage(plugin, message);
      ++loads;
    }
    stop.store(true, std::memory_order_release);
    audio.join();
    proc->lpVtbl->setProcessing(proc, 0);
    component->lpVtbl->setActive(component, 0);
    char line[200];
    std::snprintf(line, sizeof(line), "%lld blocks against %lld presets loaded by the page", g_blocks.load(),
                  loads);
    check(g_blocks.load() > 100 && loads > 100, line);
    std::snprintf(line, sizeof(line), "...and no block ran on a mixture of the two (%lld did)", g_torn.load());
    check(g_torn.load() == 0, line);
  }

  proc->lpVtbl->release(proc);
  component->lpVtbl->terminate(component);
  component->lpVtbl->release(component);
  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  if (g_failures == 0) std::printf("SONORE VST3 DIRTY PASSED\n");
  return g_failures == 0 ? 0 : 1;
}
