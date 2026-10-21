// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: what a VST3 note effect emits reaches the host, in order.
//
// VST3 carries notes as note events, SysEx as data events, poly pressure as
// its own event, and controllers, channel pressure, pitch bend and program
// changes as kLegacyMIDICCOutEvent -- the type the API defines for exactly
// that. The wrapper sent notes and SysEx and dropped the rest, on the belief
// that they "have no VST3 event equivalent", and appended every SysEx after
// every short message whatever their times.
//
// The probe's DSP emits one of each, at rising times, and the test's own
// event list records what arrives.
#define SONORE_BUILD_VST3 1
#define SONORE_NUM_PARAMS 1

#include <sonore/dsp.h>

struct SonoreDsp {
  void prepare(const sonore::ProcessSpec&) {}
  void process(sonore::ProcessContext& ctx, const float*) {
    using sonore::MidiMessage;
    const uint8_t sysex[] = {0xF0, 0x7D, 0x01, 0x02, 0xF7};
    ctx.midiOut.addSysex(sysex, sizeof(sysex), 0);
    ctx.midiOut.addEvent(MidiMessage(0xB0, 7, 100), 10);  // controller 7
    ctx.midiOut.addEvent(MidiMessage(0xE0, 0x11, 0x40), 20); // pitch bend
    ctx.midiOut.addEvent(MidiMessage(0xD0, 64, 0), 30);   // channel pressure
    ctx.midiOut.addEvent(MidiMessage(0xC0, 5, 0), 40);    // program change
    ctx.midiOut.addEvent(MidiMessage(0xA0, 60, 90), 50);  // poly pressure
    ctx.midiOut.addEvent(MidiMessage(0x90, 60, 100), 60); // note on
    ctx.main.clear();
  }
};

#include <sonore/plugin.h>

static const sonore::ParamInfo kParamTable[SONORE_NUM_PARAMS] = {
    {"x", "X", "", 0.0f, 1.0f, 0.0f, 0},
};

static const sonore::PluginDescriptor kDesc = {
    "com.sonorie.test.vst3midiout",
    "Sonore VST3 MIDI Out Probe",
    "Sonorie",
    "1.0.0",
    "Emits one of every MIDI message kind.",
    "https://sonorie.com",
    true,
    kParamTable,
    SONORE_NUM_PARAMS,
    nullptr,
    0,
    "utility",
    nullptr,
    nullptr,
    2,
    2,
    nullptr,
    0,
    true, // produces MIDI
};

#include <sonore/clap_wrapper.h>
#include <sonore/vst3_wrapper.h>

#include <cstdio>
#include <cstring>
#include <vector>

static int g_checks = 0;
static int g_failures = 0;

static void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

namespace {
std::vector<Steinberg_Vst_Event> g_received;
Steinberg_tresult SMTG_STDMETHODCALLTYPE elQuery(void*, const Steinberg_TUID, void** o) {
  *o = nullptr;
  return Steinberg_kNoInterface;
}
Steinberg_uint32 SMTG_STDMETHODCALLTYPE elRef(void*) { return 1; }
Steinberg_int32 SMTG_STDMETHODCALLTYPE elCount(void*) { return (Steinberg_int32) g_received.size(); }
Steinberg_tresult SMTG_STDMETHODCALLTYPE elGet(void*, Steinberg_int32 i, Steinberg_Vst_Event* e) {
  if (i < 0 || i >= (Steinberg_int32) g_received.size()) return Steinberg_kInvalidArgument;
  *e = g_received[(size_t) i];
  return Steinberg_kResultOk;
}
Steinberg_tresult SMTG_STDMETHODCALLTYPE elAdd(void*, Steinberg_Vst_Event* e) {
  g_received.push_back(*e);
  return Steinberg_kResultOk;
}
Steinberg_Vst_IEventListVtbl kListVtbl = {elQuery, elRef, elRef, elCount, elGet, elAdd};
struct List {
  Steinberg_Vst_IEventListVtbl* lpVtbl = &kListVtbl;
} g_list;
} // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("── VST3 MIDI out ──\n");
  g_received.reserve(64);
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
  data.outputEvents = (Steinberg_Vst_IEventList*) &g_list;
  proc->lpVtbl->process(proc, &data);

  int legacy = 0, poly = 0, notes = 0, sysex = 0;
  bool ordered = true, legacyRight = true;
  Steinberg_int32 last = -1;
  for (const Steinberg_Vst_Event& e : g_received) {
    if (e.sampleOffset < last) ordered = false;
    last = e.sampleOffset;
    if (e.type == Steinberg_Vst_Event_EventTypes_kLegacyMIDICCOutEvent) {
      ++legacy;
      const auto& cc = e.Steinberg_Vst_Event_midiCCOut;
      if (e.sampleOffset == 10 && !(cc.controlNumber == 7 && cc.value == 100)) legacyRight = false;
      if (e.sampleOffset == 20 &&
          !(cc.controlNumber == Steinberg_Vst_ControllerNumbers_kPitchBend && cc.value == 0x11 &&
            cc.value2 == 0x40))
        legacyRight = false;
      if (e.sampleOffset == 30 &&
          !(cc.controlNumber == Steinberg_Vst_ControllerNumbers_kAfterTouch && cc.value == 64))
        legacyRight = false;
      if (e.sampleOffset == 40 &&
          !(cc.controlNumber == Steinberg_Vst_ControllerNumbers_kCtrlProgramChange && cc.value == 5))
        legacyRight = false;
    }
    if (e.type == Steinberg_Vst_Event_EventTypes_kPolyPressureEvent) ++poly;
    if (e.type == Steinberg_Vst_Event_EventTypes_kNoteOnEvent) ++notes;
    if (e.type == Steinberg_Vst_Event_EventTypes_kDataEvent) ++sysex;
  }
  char line[200];
  std::snprintf(line, sizeof(line), "controller, pitch bend, channel pressure and program change "
                "arrive as legacy MIDI CC events (%d of 4)", legacy);
  check(legacy == 4 && legacyRight, line);
  std::snprintf(line, sizeof(line), "poly pressure arrives as its own event (%d)", poly);
  check(poly == 1, line);
  check(notes == 1 && sysex == 1, "the note and the SysEx arrive as before");
  check(ordered, "...and everything arrives in time order, the SysEx at 0 first");

  proc->lpVtbl->setProcessing(proc, 0);
  component->lpVtbl->setActive(component, 0);
  proc->lpVtbl->release(proc);
  component->lpVtbl->terminate(component);
  component->lpVtbl->release(component);
  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  if (g_failures == 0) std::printf("SONORE VST3 MIDI OUT PASSED\n");
  return g_failures == 0 ? 0 : 1;
}
