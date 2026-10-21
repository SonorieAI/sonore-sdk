// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: what the VST3 wrapper hands a DSP of the host's events.
//
// The wrapper compiled into the test, driven through the C API as a host
// drives it, with a probe instrument that records every MIDI message, where
// in the block it came, and how long each block it was given was.
//
// Event offsets. The host's were passed on as they came: a note-off at the
// end of the block or past it sat where a DSP that stops at its last frame
// never reached it, and the note hung.
//
// A zero-sample call. A host flushing parameters with the transport stopped
// may carry notes in it too, and they were cleared, unplayed, at the top of
// the next call.
//
// And a block longer than setupProcessing allowed, which went to the DSP
// whole: past the end of everything sized to the promised maximum.
#define SONORE_BUILD_VST3 1
#define SONORE_NUM_PARAMS 3

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <sonore/dsp.h>

struct Received {
  int status, data1, at; // at: in the host's block
  int data2;
};
static std::vector<Received> g_received;
static std::vector<size_t> g_blockSizes; // as the DSP was given them
static std::vector<float> g_blockParam;  // the parameter each block ran with
static std::vector<double> g_blockBeats; // and the song position it started at
static size_t g_offset = 0;              // where in the host's block this one starts
static float g_counter = 0.0f;
static sonore::TransportInfo g_transport;

struct SonoreDsp {
  void prepare(const sonore::ProcessSpec&) {}
  void setTransport(const sonore::TransportInfo& t) { g_transport = t; }
  void process(sonore::ProcessContext& ctx, const float* params) {
    for (const auto& e : ctx.midi)
      g_received.push_back({e.getMessage().getRawStatus(), e.getMessage().getRawData1(),
                            (int) g_offset + e.samplePosition, e.getMessage().getRawData2()});
    ctx.main.clear();
    // A count that runs on from one call to the next, so the host's buffer
    // shows whether each piece of it was written where it belongs.
    float* out = ctx.main.getChannelPointer(0);
    for (size_t i = 0; i < ctx.getNumSamples(); ++i) out[i] = g_counter++;
    g_blockSizes.push_back(ctx.getNumSamples());
    g_blockParam.push_back(params[0]);
    g_blockBeats.push_back(g_transport.positionBeats);
    g_offset += ctx.getNumSamples();
  }
};

#include <sonore/plugin.h>
#include <sonore/transport.h>

// Two whose text is not all ASCII: a unit of degrees, and a choice with a
// German name. Spelled as escapes so the test means the same bytes whatever
// the compiler takes the source to be.
static const char* const kSizeNames[2] = {"Gro\xC3\x9F", "Klein"};
static const sonore::ParamInfo kParamTable[SONORE_NUM_PARAMS] = {
    {"x", "X", "", 0.0f, 1.0f, 0.1f, 0},
    {"angle", "Angle", "\xC2\xB0", 0.0f, 360.0f, 0.0f, 0},
    {"size", "Size", "", 0.0f, 1.0f, 0.0f, 2, nullptr, kSizeNames, 2}};
static sonore::PluginDescriptor makeDesc() {
  sonore::PluginDescriptor d;
  d.id = "com.sonorie.test.vst3events";
  d.name = "Sonore VST3 Events Probe";
  d.vendor = "Sonorie";
  d.version = "1.0.0";
  d.isInstrument = true;
  d.params = kParamTable;
  d.numParams = SONORE_NUM_PARAMS;
  return d;
}
static const sonore::PluginDescriptor kDesc = makeDesc();

#include <sonore/clap_wrapper.h>
#include <sonore/vst3_wrapper.h>

static int g_checks = 0;
static int g_failures = 0;

static void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

namespace {
std::vector<Steinberg_Vst_Event> g_input;
Steinberg_tresult SMTG_STDMETHODCALLTYPE noInterface(void*, const Steinberg_TUID, void** obj) {
  *obj = nullptr;
  return Steinberg_kNoInterface;
}
Steinberg_uint32 SMTG_STDMETHODCALLTYPE oneRef(void*) { return 1; }
Steinberg_int32 SMTG_STDMETHODCALLTYPE listCount(void*) { return (Steinberg_int32) g_input.size(); }
Steinberg_tresult SMTG_STDMETHODCALLTYPE listGet(void*, Steinberg_int32 i, Steinberg_Vst_Event* e) {
  if (i < 0 || i >= (Steinberg_int32) g_input.size()) return Steinberg_kInvalidArgument;
  *e = g_input[(size_t) i];
  return Steinberg_kResultOk;
}
Steinberg_tresult SMTG_STDMETHODCALLTYPE listAdd(void*, Steinberg_Vst_Event*) { return Steinberg_kResultOk; }
Steinberg_Vst_IEventListVtbl kListVtbl = {noInterface, oneRef, oneRef, listCount, listGet, listAdd};
struct List {
  Steinberg_Vst_IEventListVtbl* lpVtbl = &kListVtbl;
} g_list;

// One parameter queue, for parameter 0 unless a check names another.
std::vector<std::pair<int, double>> g_points;
Steinberg_Vst_ParamID g_queueId = 0;
Steinberg_Vst_ParamID SMTG_STDMETHODCALLTYPE queueId(void*) { return g_queueId; }
Steinberg_int32 SMTG_STDMETHODCALLTYPE queueCount(void*) { return (Steinberg_int32) g_points.size(); }
Steinberg_tresult SMTG_STDMETHODCALLTYPE queueGet(void*, Steinberg_int32 i, Steinberg_int32* offset,
                                                  Steinberg_Vst_ParamValue* value) {
  if (i < 0 || i >= (Steinberg_int32) g_points.size()) return Steinberg_kInvalidArgument;
  *offset = g_points[(size_t) i].first;
  *value = g_points[(size_t) i].second;
  return Steinberg_kResultOk;
}
Steinberg_tresult SMTG_STDMETHODCALLTYPE queueAdd(void*, Steinberg_int32, Steinberg_Vst_ParamValue,
                                                  Steinberg_int32*) {
  return Steinberg_kResultFalse;
}
Steinberg_Vst_IParamValueQueueVtbl kQueueVtbl = {noInterface, oneRef,     oneRef,  queueId,
                                                 queueCount,  queueGet,   queueAdd};
struct Queue {
  Steinberg_Vst_IParamValueQueueVtbl* lpVtbl = &kQueueVtbl;
} g_queue;
Steinberg_int32 SMTG_STDMETHODCALLTYPE changesCount(void*) { return g_points.empty() ? 0 : 1; }
Steinberg_Vst_IParamValueQueue* SMTG_STDMETHODCALLTYPE changesGet(void*, Steinberg_int32) {
  return (Steinberg_Vst_IParamValueQueue*) &g_queue;
}
Steinberg_Vst_IParamValueQueue* SMTG_STDMETHODCALLTYPE changesAdd(void*, const Steinberg_Vst_ParamID*,
                                                                  Steinberg_int32*) {
  return nullptr;
}
Steinberg_Vst_IParameterChangesVtbl kChangesVtbl = {noInterface,  oneRef,     oneRef,
                                                    changesCount, changesGet, changesAdd};
struct Changes {
  Steinberg_Vst_IParameterChangesVtbl* lpVtbl = &kChangesVtbl;
} g_changes;

Steinberg_Vst_Event noteEvent(bool on, int offset, int pitch) {
  Steinberg_Vst_Event e{};
  e.sampleOffset = offset;
  if (on) {
    e.type = Steinberg_Vst_Event_EventTypes_kNoteOnEvent;
    e.Steinberg_Vst_Event_noteOn.pitch = (Steinberg_int16) pitch;
    e.Steinberg_Vst_Event_noteOn.velocity = 0.8f;
    e.Steinberg_Vst_Event_noteOn.noteId = -1;
  } else {
    e.type = Steinberg_Vst_Event_EventTypes_kNoteOffEvent;
    e.Steinberg_Vst_Event_noteOff.pitch = (Steinberg_int16) pitch;
    e.Steinberg_Vst_Event_noteOff.noteId = -1;
  }
  return e;
}

Steinberg_Vst_IAudioProcessor* g_processor = nullptr;
float g_left[1024], g_right[1024];

void run(int frames, bool withBuses = true, Steinberg_Vst_ProcessContext* context = nullptr) {
  float* out[2] = {g_left, g_right};
  Steinberg_Vst_AudioBusBuffers outBus{};
  outBus.numChannels = 2;
  outBus.Steinberg_Vst_AudioBusBuffers_channelBuffers32 = out;
  Steinberg_Vst_ProcessData data{};
  data.processMode = Steinberg_Vst_ProcessModes_kRealtime;
  data.symbolicSampleSize = Steinberg_Vst_SymbolicSampleSizes_kSample32;
  data.numSamples = frames;
  data.numOutputs = withBuses ? 1 : 0;
  data.outputs = withBuses ? &outBus : nullptr;
  data.inputEvents = (Steinberg_Vst_IEventList*) &g_list;
  data.inputParameterChanges = (Steinberg_Vst_IParameterChanges*) &g_changes;
  data.processContext = context;
  g_received.clear();
  g_blockSizes.clear();
  g_blockParam.clear();
  g_blockBeats.clear();
  g_offset = 0;
  g_processor->lpVtbl->process(g_processor, &data);
  g_input.clear();
  g_points.clear();
  g_queueId = 0;
}

/** Every controller message the DSP was given, as "value@frame" in order. */
std::string controllerTrail(int status, int controller) {
  std::string trail;
  for (const Received& r : g_received) {
    if (r.status != status || r.data1 != controller) continue;
    char item[32];
    std::snprintf(item, sizeof(item), "%s%d@%d", trail.empty() ? "" : " ", r.data2, r.at);
    trail += item;
  }
  return trail;
}

int receivedAt(int status, int data1) {
  for (const Received& r : g_received)
    if (r.status == status && r.data1 == data1) return r.at;
  return -1000;
}
} // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("── VST3 events into the DSP ──\n");
  g_received.reserve(256);
  Steinberg_IPluginFactory* factory = GetPluginFactory();
  Steinberg_PClassInfo info{};
  for (Steinberg_int32 i = 0; i < factory->lpVtbl->countClasses(factory); ++i)
    if (factory->lpVtbl->getClassInfo(factory, i, &info) == Steinberg_kResultOk &&
        std::strcmp(info.category, "Audio Module Class") == 0)
      break;
  void* object = nullptr;
  factory->lpVtbl->createInstance(factory, (Steinberg_FIDString) info.cid,
                                  (Steinberg_FIDString) Steinberg_Vst_IComponent_iid, &object);
  auto* component = (Steinberg_Vst_IComponent*) object;
  check(component && component->lpVtbl->initialize(component, nullptr) == Steinberg_kResultOk,
        "the probe instrument initialises");
  if (!component) return 1;
  component->lpVtbl->queryInterface(component, Steinberg_Vst_IAudioProcessor_iid, (void**) &g_processor);
  Steinberg_Vst_ProcessSetup setup{};
  setup.processMode = Steinberg_Vst_ProcessModes_kRealtime;
  setup.symbolicSampleSize = Steinberg_Vst_SymbolicSampleSizes_kSample32;
  setup.maxSamplesPerBlock = 64;
  setup.sampleRate = 48000.0;
  g_processor->lpVtbl->setupProcessing(g_processor, &setup);
  component->lpVtbl->setActive(component, 1);
  g_processor->lpVtbl->setProcessing(g_processor, 1);

  // Note-offs timed at the end of a 64-frame block and well past it.
  g_input = {noteEvent(true, 0, 50), noteEvent(true, 0, 51)};
  run(64);
  g_input = {noteEvent(false, 64, 50), noteEvent(false, 200, 51)};
  run(64);
  {
    char line[160];
    std::snprintf(line, sizeof(line),
                  "note-offs at offsets 64 and 200 of a 64-frame block land on its last frame (%d, %d)",
                  receivedAt(0x80, 50), receivedAt(0x80, 51));
    check(receivedAt(0x80, 50) == 63 && receivedAt(0x80, 51) == 63, line);
  }

  // The bar a VST3 host is in, which it gives as a position only, and its
  // cycle, which the DSP never saw.
  {
    Steinberg_Vst_ProcessContext context{};
    context.state = Steinberg_Vst_ProcessContext_StatesAndFlags_kPlaying |
                    Steinberg_Vst_ProcessContext_StatesAndFlags_kTempoValid |
                    Steinberg_Vst_ProcessContext_StatesAndFlags_kProjectTimeMusicValid |
                    Steinberg_Vst_ProcessContext_StatesAndFlags_kBarPositionValid |
                    Steinberg_Vst_ProcessContext_StatesAndFlags_kTimeSigValid |
                    Steinberg_Vst_ProcessContext_StatesAndFlags_kCycleActive |
                    Steinberg_Vst_ProcessContext_StatesAndFlags_kCycleValid;
    context.sampleRate = 48000.0;
    context.tempo = 100.0;
    context.projectTimeMusic = 13.5;
    context.barPositionMusic = 12.0;
    context.timeSigNumerator = 3;
    context.timeSigDenominator = 4;
    context.cycleStartMusic = 6.0;
    context.cycleEndMusic = 18.0;
    run(64, true, &context);
    char line[200];
    std::snprintf(line, sizeof(line), "a VST3 host's bar and cycle reach the DSP (bar %d, loop %.1f to %.1f)",
                  (int) g_transport.barNumber, g_transport.loopStartBeats, g_transport.loopEndBeats);
    check(g_transport.barNumber == 4 && g_transport.hasLoop && g_transport.isLooping &&
              g_transport.loopStartBeats == 6.0 && g_transport.loopEndBeats == 18.0,
          line);
  }

  // The same cycle from a host that fills only what the plugin asked for
  // through IProcessContextRequirements, which is what the interface is for:
  // Cubase leaves out what was not requested. The wrapper read the cycle and
  // never asked for it, so in such a host a DSP never saw a loop.
  {
    Steinberg_Vst_IProcessContextRequirements* requirements = nullptr;
    component->lpVtbl->queryInterface(component, Steinberg_Vst_IProcessContextRequirements_iid,
                                      (void**) &requirements);
    const Steinberg_uint32 asked =
        requirements ? requirements->lpVtbl->getProcessContextRequirements(requirements) : 0;
    if (requirements) requirements->lpVtbl->release(requirements);
    Steinberg_Vst_ProcessContext context{};
    context.sampleRate = 48000.0;
    if (asked & Steinberg_Vst_IProcessContextRequirements_Flags_kNeedTransportState)
      context.state |= Steinberg_Vst_ProcessContext_StatesAndFlags_kPlaying |
                       Steinberg_Vst_ProcessContext_StatesAndFlags_kCycleActive;
    if (asked & Steinberg_Vst_IProcessContextRequirements_Flags_kNeedCycleMusic) {
      context.state |= Steinberg_Vst_ProcessContext_StatesAndFlags_kCycleValid;
      context.cycleStartMusic = 6.0;
      context.cycleEndMusic = 18.0;
    }
    run(64, true, &context);
    char line[200];
    std::snprintf(line, sizeof(line),
                  "a host that fills only what was asked for still delivers the loop "
                  "(requirements 0x%x, loop %.1f to %.1f)",
                  (unsigned) asked, g_transport.loopStartBeats, g_transport.loopEndBeats);
    check(g_transport.isLooping && g_transport.hasLoop && g_transport.loopStartBeats == 6.0 &&
              g_transport.loopEndBeats == 18.0,
          line);
  }

  // A note-off in a zero-sample call with no buses, as a parameter flush is.
  g_input = {noteEvent(true, 0, 52)};
  run(64);
  g_input = {noteEvent(false, 0, 52)};
  run(0, false);
  run(64);
  check(receivedAt(0x80, 52) == 0, "a note-off sent in a zero-sample call reaches the next block");

  // A pedal change: the sustain pedal lifted and pressed again a frame later,
  // which a piano part does to let go of what it held and catch only what is
  // still down. VST3 delivers a controller as a parameter whose queue has a
  // point per message, and only the last point reached the DSP: it saw the
  // pedal go down while already down, never up, and every note the pedal
  // held rang on. The CLAP, LV2 and AU builds are handed both messages.
  {
    const Steinberg_Vst_ParamID sustain = sonore::vst3::midiCcParamId(0, 64);
    g_points = {{40, 1.0}};
    g_queueId = sustain;
    run(64);
    g_points = {{20, 0.0}, {21, 1.0}};
    g_queueId = sustain;
    run(64);
    std::string trail = controllerTrail(0xB0, 64);
    std::string line = "a pedal lifted and pressed a frame apart reaches the DSP both ways (" + trail + ")";
    check(trail == "0@20 127@21", line.c_str());

    // And through a block longer than the promised maximum, where each piece
    // is handed the points in it and none from an earlier piece again.
    g_points = {{30, 0.0}, {100, 1.0}, {130, 0.0}, {131, 1.0}};
    g_queueId = sustain;
    run(200);
    trail = controllerTrail(0xB0, 64);
    line = "...and through a 200-frame block cut into pieces, each message once (" + trail + ")";
    check(trail == "0@30 127@100 0@130 127@131", line.c_str());
  }

  // Four output channels for a stereo instrument: the two it does not write
  // are silence, not what the host's buffer held.
  {
    float c2[64], c3[64];
    for (int i = 0; i < 64; ++i) c2[i] = c3[i] = -1.0f;
    float* out[4] = {g_left, g_right, c2, c3};
    Steinberg_Vst_AudioBusBuffers outBus{};
    outBus.numChannels = 4;
    outBus.Steinberg_Vst_AudioBusBuffers_channelBuffers32 = out;
    Steinberg_Vst_ProcessData data{};
    data.processMode = Steinberg_Vst_ProcessModes_kRealtime;
    data.symbolicSampleSize = Steinberg_Vst_SymbolicSampleSizes_kSample32;
    data.numSamples = 64;
    data.numOutputs = 1;
    data.outputs = &outBus;
    g_processor->lpVtbl->process(g_processor, &data);
    bool silent = true;
    for (int i = 0; i < 64; ++i) silent = silent && c2[i] == 0.0f && c3[i] == 0.0f;
    char line[120];
    std::snprintf(line, sizeof(line), "the two channels past a stereo DSP are silence (%.0f, %.0f)", c2[0], c3[0]);
    check(silent, line);
  }

  // 200 frames after setting up for at most 64, with notes, automation and a
  // playing transport (120 bpm: 48000 frames a 2-beat second, so 64 frames
  // are 1/375 of a beat).
  g_input = {noteEvent(true, 10, 70), noteEvent(false, 130, 70), noteEvent(true, 199, 71)};
  g_points = {{10, 0.2}, {100, 0.6}, {190, 0.9}};
  Steinberg_Vst_ProcessContext context{};
  context.state = Steinberg_Vst_ProcessContext_StatesAndFlags_kPlaying |
                  Steinberg_Vst_ProcessContext_StatesAndFlags_kTempoValid |
                  Steinberg_Vst_ProcessContext_StatesAndFlags_kProjectTimeMusicValid;
  context.sampleRate = 48000.0;
  context.tempo = 120.0;
  context.projectTimeMusic = 8.0;
  run(200, true, &context);
  {
    size_t longest = 0, total = 0;
    for (size_t n : g_blockSizes) {
      longest = n > longest ? n : longest;
      total += n;
    }
    bool inPlace = true;
    for (int i = 1; i < 200; ++i) inPlace = inPlace && g_left[i] == g_left[i - 1] + 1.0f;
    char line[220];
    std::snprintf(line, sizeof(line),
                  "a 200-frame block reaches a DSP set up for 64 as %zu blocks, the longest %zu",
                  g_blockSizes.size(), longest);
    check(longest <= 64 && total == 200, line);
    check(inPlace, "...each written where it belongs in the host's buffer");
    std::snprintf(line, sizeof(line), "...every note at its own time in it (%d, %d, %d)",
                  receivedAt(0x90, 70), receivedAt(0x80, 70), receivedAt(0x90, 71));
    check(receivedAt(0x90, 70) == 10 && receivedAt(0x80, 70) == 130 && receivedAt(0x90, 71) == 199,
          line);
    const bool fourBlocks = g_blockParam.size() == 4;
    std::snprintf(line, sizeof(line),
                  "...the automation as it stands in each piece (%.2f %.2f %.2f %.2f)",
                  fourBlocks ? g_blockParam[0] : -1.0f, fourBlocks ? g_blockParam[1] : -1.0f,
                  fourBlocks ? g_blockParam[2] : -1.0f, fourBlocks ? g_blockParam[3] : -1.0f);
    check(fourBlocks && std::fabs(g_blockParam[0] - 0.2f) < 1e-6f &&
              std::fabs(g_blockParam[1] - 0.6f) < 1e-6f && std::fabs(g_blockParam[2] - 0.9f) < 1e-6f &&
              std::fabs(g_blockParam[3] - 0.9f) < 1e-6f,
          line);
    bool moved = fourBlocks;
    for (size_t b = 0; moved && b < 4; ++b)
      moved = std::fabs(g_blockBeats[b] - (8.0 + 64.0 * (double) b / 24000.0)) < 1e-9;
    std::snprintf(line, sizeof(line), "...and the song position moved on to where each starts (%.6f .. %.6f)",
                  fourBlocks ? g_blockBeats[0] : -1.0, fourBlocks ? g_blockBeats[3] : -1.0);
    check(moved, line);
  }

  // A value's text read back as the value: what a host does with what a user
  // types into its own box, and with the strings this plugin printed. The
  // text came in as UTF-16 and was narrowed to ASCII, everything else to
  // '?', on the belief that only a number was read out of it -- but the unit
  // and a step's name are read too, so "90 °" and "Groß", the plugin's own
  // words, were refused. The CLAP build, given UTF-8, takes both.
  {
    Steinberg_Vst_IEditController* controller = nullptr;
    component->lpVtbl->queryInterface(component, Steinberg_Vst_IEditController_iid, (void**) &controller);
    std::string texts;
    bool roundTrips = controller != nullptr;
    const std::pair<Steinberg_Vst_ParamID, double> cases[] = {{1, 0.25}, {2, 0.0}, {2, 1.0}};
    for (const auto& c : cases) {
      if (!controller) break;
      Steinberg_Vst_String128 text{};
      Steinberg_Vst_ParamValue back = -1.0;
      const bool printed =
          controller->lpVtbl->getParamStringByValue(controller, c.first, c.second, text) == Steinberg_kResultOk;
      const bool parsed =
          controller->lpVtbl->getParamValueByString(controller, c.first, text, &back) == Steinberg_kResultOk;
      texts += (texts.empty() ? "" : ", ") + sonore::vst3::utf16ToUtf8(text, 128) + (parsed ? "" : " refused");
      roundTrips = roundTrips && printed && parsed && std::fabs(back - c.second) < 1e-6;
    }
    const std::string line = "the plugin's own text with a non-ASCII unit or name reads back as its value (" + texts + ")";
    check(roundTrips, line.c_str());
    if (controller) controller->lpVtbl->release(controller);
  }

  g_processor->lpVtbl->setProcessing(g_processor, 0);
  component->lpVtbl->setActive(component, 0);
  g_processor->lpVtbl->release(g_processor);
  component->lpVtbl->terminate(component);
  component->lpVtbl->release(component);
  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  if (g_failures == 0) std::printf("SONORE VST3 EVENTS PASSED\n");
  return g_failures == 0 ? 0 : 1;
}
