// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: a VST3 note-on's own tuning reaching an expressive DSP.
//
// VST3's NoteOnEvent carries a TUNING, in cents, with the note: how a
// microtonal host, or Cubase's per-note expression, says "this C, a quarter
// tone sharp" in one event rather than a note and then a correction. The
// wrapper read the pitch and the velocity and dropped the tuning, so every
// such note played at its twelve-tone pitch.
//
// The VST3 wrapper is compiled into this test, as in vst3_midiout_test, and a
// note-on with a tuning is sent through process().
#define SONORE_BUILD_VST3 1
#define SONORE_NUM_PARAMS 1

#include <sonore/dsp.h>

#include <vector>

namespace {
struct Seen {
  int key, channel, sample;
  float semitones;
};
std::vector<Seen> g_tunings;
int g_noteOns = 0;
} // namespace

struct SonoreDsp {
  void prepare(const sonore::ProcessSpec&) {}
  void process(sonore::ProcessContext& ctx, const float*) {
    for (const auto& e : ctx.midi)
      if (e.getMessage().isNoteOn()) ++g_noteOns;
    if (ctx.expression)
      for (const auto& x : *ctx.expression)
        if (x.expression == sonore::kExprTuning)
          g_tunings.push_back({x.key, x.channel, x.samplePosition, x.value});
    ctx.main.clear();
  }
};

#include <sonore/plugin.h>

static const sonore::ParamInfo kParamTable[SONORE_NUM_PARAMS] = {
    {"x", "X", "", 0.0f, 1.0f, 0.0f, 0},
};

static sonore::PluginDescriptor makeDesc() {
  sonore::PluginDescriptor d;
  d.id = "com.sonorie.test.vst3tuning";
  d.name = "Sonore VST3 Tuning Probe";
  d.description = "Records the tuning expressions it is given.";
  d.url = "https://sonorie.com";
  d.isInstrument = true;
  d.params = kParamTable;
  d.numParams = SONORE_NUM_PARAMS;
  d.category = "synth";
  d.supportsMpe = true;
  return d;
}
static const sonore::PluginDescriptor kDesc = makeDesc();

#include <sonore/clap_wrapper.h>
#include <sonore/vst3_wrapper.h>

#include <cmath>
#include <cstdio>
#include <cstring>

static int g_checks = 0;
static int g_failures = 0;

static void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

namespace {
std::vector<Steinberg_Vst_Event> g_input;
Steinberg_tresult SMTG_STDMETHODCALLTYPE elQuery(void*, const Steinberg_TUID, void** o) {
  *o = nullptr;
  return Steinberg_kNoInterface;
}
Steinberg_uint32 SMTG_STDMETHODCALLTYPE elRef(void*) { return 1; }
Steinberg_int32 SMTG_STDMETHODCALLTYPE elCount(void*) { return (Steinberg_int32) g_input.size(); }
Steinberg_tresult SMTG_STDMETHODCALLTYPE elGet(void*, Steinberg_int32 i, Steinberg_Vst_Event* e) {
  if (i < 0 || i >= (Steinberg_int32) g_input.size()) return Steinberg_kInvalidArgument;
  *e = g_input[(size_t) i];
  return Steinberg_kResultOk;
}
Steinberg_tresult SMTG_STDMETHODCALLTYPE elAdd(void*, Steinberg_Vst_Event*) {
  return Steinberg_kResultOk;
}
Steinberg_Vst_IEventListVtbl kListVtbl = {elQuery, elRef, elRef, elCount, elGet, elAdd};
struct List {
  Steinberg_Vst_IEventListVtbl* lpVtbl = &kListVtbl;
} g_list;

Steinberg_Vst_Event noteOn(int offset, int pitch, int channel, float cents, int id) {
  Steinberg_Vst_Event e{};
  e.type = Steinberg_Vst_Event_EventTypes_kNoteOnEvent;
  e.sampleOffset = offset;
  e.Steinberg_Vst_Event_noteOn.channel = (Steinberg_int16) channel;
  e.Steinberg_Vst_Event_noteOn.pitch = (Steinberg_int16) pitch;
  e.Steinberg_Vst_Event_noteOn.velocity = 0.8f;
  e.Steinberg_Vst_Event_noteOn.tuning = cents;
  e.Steinberg_Vst_Event_noteOn.noteId = id;
  return e;
}
} // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("── VST3 note-on tuning ──\n");
  Steinberg_IPluginFactory* factory = GetPluginFactory();
  Steinberg_PClassInfo info{};
  bool found = false;
  for (Steinberg_int32 i = 0; i < factory->lpVtbl->countClasses(factory) && !found; ++i)
    found = factory->lpVtbl->getClassInfo(factory, i, &info) == Steinberg_kResultOk &&
            std::strcmp(info.category, "Audio Module Class") == 0;
  void* object = nullptr;
  factory->lpVtbl->createInstance(factory, (Steinberg_FIDString) info.cid,
                                  (Steinberg_FIDString) Steinberg_Vst_IComponent_iid, &object);
  check(found && object != nullptr, "the probe is created through its own factory");
  if (!object) return 1;
  auto* component = (Steinberg_Vst_IComponent*) object;
  component->lpVtbl->initialize(component, nullptr);
  Steinberg_Vst_IAudioProcessor* proc = nullptr;
  component->lpVtbl->queryInterface(component, Steinberg_Vst_IAudioProcessor_iid, (void**) &proc);
  Steinberg_Vst_ProcessSetup setup{};
  setup.processMode = Steinberg_Vst_ProcessModes_kRealtime;
  setup.symbolicSampleSize = Steinberg_Vst_SymbolicSampleSizes_kSample32;
  setup.maxSamplesPerBlock = 128;
  setup.sampleRate = 48000.0;
  proc->lpVtbl->setupProcessing(proc, &setup);
  component->lpVtbl->setActive(component, 1);
  proc->lpVtbl->setProcessing(proc, 1);

  std::vector<float> l(128), r(128);
  float* out[2] = {l.data(), r.data()};
  Steinberg_Vst_AudioBusBuffers outBus{};
  outBus.numChannels = 2;
  outBus.Steinberg_Vst_AudioBusBuffers_channelBuffers32 = out;
  Steinberg_Vst_ProcessData data{};
  data.processMode = Steinberg_Vst_ProcessModes_kRealtime;
  data.symbolicSampleSize = Steinberg_Vst_SymbolicSampleSizes_kSample32;
  data.numSamples = 128;
  data.numOutputs = 1;
  data.outputs = &outBus;
  data.inputEvents = (Steinberg_Vst_IEventList*) &g_list;

  g_input.push_back(noteOn(10, 60, 0, 50.0f, 1));   // a quarter tone sharp
  g_input.push_back(noteOn(20, 64, 1, -14.0f, 2));  // a just major third, near enough
  g_input.push_back(noteOn(30, 67, 2, 0.0f, 3));    // in tune: nothing to say
  proc->lpVtbl->process(proc, &data);

  std::printf("  ---- %d note-ons, %d tuning expressions ----\n", g_noteOns, (int) g_tunings.size());
  check(g_noteOns == 3, "the three notes arrive as note-ons");
  const bool first = g_tunings.size() >= 1 && g_tunings[0].key == 60 && g_tunings[0].channel == 0 &&
                     g_tunings[0].sample == 10 && std::fabs(g_tunings[0].semitones - 0.5f) < 1e-6f;
  check(first, "+50 cents on C4 arrives as +0.5 semitones of tuning, at the note's own frame");
  const bool second = g_tunings.size() >= 2 && g_tunings[1].key == 64 && g_tunings[1].channel == 1 &&
                      g_tunings[1].sample == 20 && std::fabs(g_tunings[1].semitones + 0.14f) < 1e-6f;
  check(second, "and -14 cents on E4, on its own channel, as -0.14");
  check(g_tunings.size() == 2, "a note in tune adds no expression");

  proc->lpVtbl->setProcessing(proc, 0);
  component->lpVtbl->setActive(component, 0);
  proc->lpVtbl->release(proc);
  component->lpVtbl->terminate(component);
  component->lpVtbl->release(component);
  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
