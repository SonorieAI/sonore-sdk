// SPDX-License-Identifier: Apache-2.0
// Sonore SDK test fixture: a VST3 plugin whose controller is a separate
// object, as most VST3 plugins are.
//
// Two things a host owes such a plugin, both of which sonore/host.h skipped:
//   * an edit made in the CONTROLLER -- a knob turned on the plugin's own
//     editor, reported through IComponentHandler::performEdit -- has to reach
//     the PROCESSOR, through the next process() call's parameter changes.
//     The host recorded it for automation and sent it nowhere, so the DSP
//     never heard its own editor.
//   * the two halves are connected through IConnectionPoint, each to the
//     other, and disconnected before they are terminated -- the channel a
//     processor sends meters and a controller sends anything else down.
//
// Written against the C API so it can watch both. The controller edits its
// visible parameter (100) and a hidden one (200) the moment it is given a
// component handler; the processor writes the last value it received for
// each into the process environment (shared with the test), and both halves
// count their live connections there.
//
// And the other way: the processor sets a read-only meter (300) through the
// block's output parameter changes, at 0.375 and then 0.625 in its first
// block, the way a split plugin reports a level to its own editor -- and on
// every block after, ending at (block % 997) / 1000, so the newest value
// after any number of blocks is known.
//
// It has an event input too, and counts every event whose sampleOffset
// falls outside the block it arrived in.
//
// And it talks between its halves the way a plugin built on Steinberg's SDK
// does: both halves take the IHostApplication from the context initialize()
// is given -- host.h passed null -- and make their messages with its
// createInstance(). The processor makes one in initialize(), with an int, a
// float, a string and a binary attribute, each from a buffer it scribbles
// over straight after, and sends it to the controller when activated; the
// controller sends one back when it is given its component handler. Each
// side records what arrived, and the processor what its last reference
// release left. With SONORE_SPLIT_RT_MSG set the processor also sends from
// process(), which the controller counts: that must never arrive. And with
// SONORE_SPLIT_REFUSE_INIT or SONORE_SPLIT_REFUSE_CTRL_INIT a half refuses
// initialize(), and counts any terminate() it is then sent anyway.

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

int readEnv(const char* name) {
  char text[32] = {0};
#if defined(_WIN32)
  GetEnvironmentVariableA(name, text, (DWORD) sizeof(text));
#else
  if (const char* v = std::getenv(name)) std::snprintf(text, sizeof(text), "%s", v);
#endif
  return std::atoi(text);
}

void writeEnv(const char* name, int value) {
  char text[32];
  std::snprintf(text, sizeof(text), "%d", value);
#if defined(_WIN32)
  SetEnvironmentVariableA(name, text);
#else
  setenv(name, text, 1);
#endif
}

void writeEnvText(const char* name, const char* text) {
#if defined(_WIN32)
  SetEnvironmentVariableA(name, text);
#else
  setenv(name, text, 1);
#endif
}

bool sameIid(const Steinberg_TUID a, const Steinberg_TUID b) { return std::memcmp(a, b, 16) == 0; }

/** The host application behind initialize()'s context, as Steinberg's
 *  ComponentBase finds it: queried, and kept until terminate(). */
Steinberg_Vst_IHostApplication* hostApplicationOf(Steinberg_FUnknown* context) {
  Steinberg_Vst_IHostApplication* app = nullptr;
  if (!context || context->lpVtbl->queryInterface(context, Steinberg_Vst_IHostApplication_iid,
                                                  (void**) &app) != Steinberg_kResultOk)
    return nullptr;
  return app;
}

/** allocateMessage(), as Steinberg's SDK writes it: IMessage's iid as both
 *  the class and the interface asked for. Null without a host to ask. */
Steinberg_Vst_IMessage* allocateMessage(Steinberg_Vst_IHostApplication* app, const char* id) {
  if (!app) return nullptr;
  Steinberg_TUID cid, iid;
  std::memcpy(cid, Steinberg_Vst_IMessage_iid, 16);
  std::memcpy(iid, Steinberg_Vst_IMessage_iid, 16);
  Steinberg_Vst_IMessage* m = nullptr;
  if (app->lpVtbl->createInstance(app, cid, iid, (void**) &m) != Steinberg_kResultOk || !m)
    return nullptr;
  m->lpVtbl->setMessageID(m, id);
  return m;
}

/** What the processor's message carries, checked by the controller. */
const Steinberg_int64 kHelloCount = -1234567890123LL;
const double kHelloLevel = 0.8125;
const char16_t kHelloName[] = u"Grüße aus dem Prozessor";
uint8_t helloByte(int i) { return (uint8_t) (i * 7 + 3); }
constexpr int kHelloBytes = 300;

/** Which of the message's four attributes arrived as sent, a bit each, and
 *  a fifth for a string read into too small a buffer coming back cut and
 *  terminated. 31 is all of them. */
int checkHello(Steinberg_Vst_IMessage* m) {
  Steinberg_Vst_IAttributeList* a = m->lpVtbl->getAttributes(m);
  if (!a) return 0;
  int intact = 0;
  Steinberg_int64 count = 0;
  if (a->lpVtbl->getInt(a, "count", &count) == Steinberg_kResultOk && count == kHelloCount)
    intact |= 1;
  double level = 0.0;
  if (a->lpVtbl->getFloat(a, "level", &level) == Steinberg_kResultOk && level == kHelloLevel)
    intact |= 2;
  Steinberg_Vst_String128 name{};
  if (a->lpVtbl->getString(a, "name", name, (Steinberg_uint32) sizeof(name)) == Steinberg_kResultOk &&
      std::memcmp(name, kHelloName, sizeof(kHelloName)) == 0)
    intact |= 4;
  const void* data = nullptr;
  Steinberg_uint32 size = 0;
  if (a->lpVtbl->getBinary(a, "blob", &data, &size) == Steinberg_kResultOk && data &&
      size == (Steinberg_uint32) kHelloBytes) {
    bool same = true;
    for (int i = 0; i < kHelloBytes; ++i)
      if (static_cast<const uint8_t*>(data)[i] != helloByte(i)) same = false;
    if (same) intact |= 8;
  }
  Steinberg_Vst_TChar cut[3] = {1, 1, 1};
  if (a->lpVtbl->getString(a, "name", cut, (Steinberg_uint32) sizeof(cut)) == Steinberg_kResultOk &&
      cut[0] == u'G' && cut[1] == u'r' && cut[2] == 0)
    intact |= 16;
  // A value asked for as a type it is not is not there.
  double asFloat = 0.0;
  if (a->lpVtbl->getFloat(a, "count", &asFloat) == Steinberg_kResultOk) intact &= ~1;
  return intact;
}

/** The host's component handler, as the controller was last given it. The
 *  processor reaches it too when SONORE_SPLIT_RT_EDITS is set: an edit and a
 *  restart from inside process(), on the audio thread, which the format
 *  forbids and plugins do anyway. */
std::atomic<Steinberg_Vst_IComponentHandler*> g_handler{nullptr};

const Steinberg_TUID kProcessorCid = {'S', 'o', 'n', 'o', 'r', 'e', 'S', 'p',
                                      'l', 'i', 't', 'P', 'r', 'o', 'c', '1'};
const Steinberg_TUID kControllerCid = {'S', 'o', 'n', 'o', 'r', 'e', 'S', 'p',
                                       'l', 'i', 't', 'C', 't', 'r', 'l', '1'};

// ── Connection points: each half keeps the peer it is connected to ──────────

/** [main-thread] Connected: the peer kept, and counted. */
Steinberg_tresult connectTo(Steinberg_Vst_IConnectionPoint** peer, Steinberg_Vst_IConnectionPoint* other) {
  if (!other) return Steinberg_kInvalidArgument;
  *peer = other;
  writeEnv("SONORE_SPLIT_CONNECTED", readEnv("SONORE_SPLIT_CONNECTED") + 1);
  return Steinberg_kResultOk;
}
Steinberg_tresult disconnectFrom(Steinberg_Vst_IConnectionPoint** peer) {
  *peer = nullptr;
  writeEnv("SONORE_SPLIT_CONNECTED", readEnv("SONORE_SPLIT_CONNECTED") - 1);
  return Steinberg_kResultOk;
}

// ── The processor ───────────────────────────────────────────────────────────

struct Processor;
template <typename Vtbl>
struct Face {
  Vtbl* lpVtbl;
  Processor* self;
};
struct Processor {
  Face<Steinberg_Vst_IComponentVtbl> component;
  Face<Steinberg_Vst_IAudioProcessorVtbl> audio;
  Face<Steinberg_Vst_IConnectionPointVtbl> connection;
  std::atomic<int> refs{1};
  bool initialized = false;
  Steinberg_Vst_IHostApplication* host = nullptr;
  /** Made in initialize(), sent when activated. */
  Steinberg_Vst_IMessage* hello = nullptr;
  /** Set at load and cleared at unload, never while audio runs. */
  Steinberg_Vst_IConnectionPoint* peer = nullptr;
};

template <typename F>
Processor* proc(void* p) {
  return static_cast<F*>(p)->self;
}

Steinberg_tresult queryProcessor(Processor* p, const Steinberg_TUID iid, void** obj) {
  if (!obj) return Steinberg_kInvalidArgument;
  if (sameIid(iid, Steinberg_FUnknown_iid) || sameIid(iid, Steinberg_IPluginBase_iid) ||
      sameIid(iid, Steinberg_Vst_IComponent_iid))
    *obj = &p->component;
  else if (sameIid(iid, Steinberg_Vst_IAudioProcessor_iid))
    *obj = &p->audio;
  else if (sameIid(iid, Steinberg_Vst_IConnectionPoint_iid))
    *obj = &p->connection;
  else {
    *obj = nullptr;
    return Steinberg_kNoInterface;
  }
  p->refs.fetch_add(1);
  return Steinberg_kResultOk;
}
Steinberg_uint32 releaseProcessor(Processor* p) {
  const int left = p->refs.fetch_sub(1) - 1;
  if (left == 0) delete p;
  return (Steinberg_uint32) left;
}

using CFace = Face<Steinberg_Vst_IComponentVtbl>;
using AFace = Face<Steinberg_Vst_IAudioProcessorVtbl>;
using PFace = Face<Steinberg_Vst_IConnectionPointVtbl>;

Steinberg_tresult SMTG_STDMETHODCALLTYPE cQuery(void* p, const Steinberg_TUID iid, void** o) {
  return queryProcessor(proc<CFace>(p), iid, o);
}
Steinberg_uint32 SMTG_STDMETHODCALLTYPE cAddRef(void* p) {
  return (Steinberg_uint32) (proc<CFace>(p)->refs.fetch_add(1) + 1);
}
Steinberg_uint32 SMTG_STDMETHODCALLTYPE cRelease(void* p) { return releaseProcessor(proc<CFace>(p)); }
Steinberg_tresult SMTG_STDMETHODCALLTYPE cControllerId(void*, Steinberg_TUID cid) {
  std::memcpy(cid, kControllerCid, 16);
  return Steinberg_kResultOk;
}
Steinberg_tresult SMTG_STDMETHODCALLTYPE cIoMode(void*, Steinberg_Vst_IoMode) { return Steinberg_kResultOk; }
// An audio bus each way, and an event input, so the host hands it notes.
Steinberg_int32 SMTG_STDMETHODCALLTYPE cBusCount(void*, Steinberg_Vst_MediaType type,
                                                 Steinberg_Vst_BusDirection dir) {
  if (type == Steinberg_Vst_MediaTypes_kAudio) return 1;
  return type == Steinberg_Vst_MediaTypes_kEvent && dir == Steinberg_Vst_BusDirections_kInput ? 1 : 0;
}
Steinberg_tresult SMTG_STDMETHODCALLTYPE cBusInfo(void*, Steinberg_Vst_MediaType type,
                                                  Steinberg_Vst_BusDirection dir, Steinberg_int32 index,
                                                  struct Steinberg_Vst_BusInfo* bus) {
  const bool events = type == Steinberg_Vst_MediaTypes_kEvent && dir == Steinberg_Vst_BusDirections_kInput;
  if ((type != Steinberg_Vst_MediaTypes_kAudio && !events) || index != 0 || !bus)
    return Steinberg_kInvalidArgument;
  std::memset(bus, 0, sizeof(*bus));
  bus->mediaType = type;
  bus->direction = dir;
  bus->channelCount = events ? 16 : 2;
  bus->busType = Steinberg_Vst_BusTypes_kMain;
  bus->flags = Steinberg_Vst_BusInfo_BusFlags_kDefaultActive;
  return Steinberg_kResultOk;
}
Steinberg_tresult SMTG_STDMETHODCALLTYPE cRouting(void*, struct Steinberg_Vst_RoutingInfo*,
                                                  struct Steinberg_Vst_RoutingInfo*) {
  return Steinberg_kNotImplemented;
}
Steinberg_tresult SMTG_STDMETHODCALLTYPE cActivateBus(void*, Steinberg_Vst_MediaType, Steinberg_Vst_BusDirection,
                                                      Steinberg_int32, Steinberg_TBool) {
  return Steinberg_kResultOk;
}
Steinberg_tresult SMTG_STDMETHODCALLTYPE okBool(void*, Steinberg_TBool) { return Steinberg_kResultOk; }
Steinberg_tresult SMTG_STDMETHODCALLTYPE okStream(void*, struct Steinberg_IBStream*) {
  return Steinberg_kResultOk;
}

/** The host application, kept; and the message the processor will send,
 *  made now from buffers that are overwritten straight after -- the list
 *  must have taken copies. */
Steinberg_tresult SMTG_STDMETHODCALLTYPE cInitialize(void* p, struct Steinberg_FUnknown* context) {
  Processor* self = proc<CFace>(p);
  if (readEnv("SONORE_SPLIT_REFUSE_INIT")) return Steinberg_kResultFalse;
  self->initialized = true;
  self->host = hostApplicationOf(context);
  writeEnv("SONORE_SPLIT_HOST_APP", self->host ? 1 : 0);
  self->hello = allocateMessage(self->host, "SonoreHello");
  if (Steinberg_Vst_IAttributeList* a = self->hello ? self->hello->lpVtbl->getAttributes(self->hello) : nullptr) {
    a->lpVtbl->setInt(a, "count", kHelloCount);
    a->lpVtbl->setFloat(a, "level", kHelloLevel);
    Steinberg_Vst_TChar name[64] = {};
    std::memcpy(name, kHelloName, sizeof(kHelloName));
    a->lpVtbl->setString(a, "name", name);
    uint8_t blob[kHelloBytes];
    for (int i = 0; i < kHelloBytes; ++i) blob[i] = helloByte(i);
    a->lpVtbl->setBinary(a, "blob", blob, (Steinberg_uint32) sizeof(blob));
    // The sender's buffers are its own: a stack array, reused at once.
    std::memset(name, 0x55, sizeof(name));
    std::memset(blob, 0xAA, sizeof(blob));
    // A value set twice is the second one.
    a->lpVtbl->setInt(a, "count", 7);
    a->lpVtbl->setInt(a, "count", kHelloCount);
  }
  return Steinberg_kResultOk;
}
Steinberg_tresult SMTG_STDMETHODCALLTYPE cTerminate(void* p) {
  Processor* self = proc<CFace>(p);
  if (!self->initialized)
    writeEnv("SONORE_SPLIT_TERMINATED_UNINIT", readEnv("SONORE_SPLIT_TERMINATED_UNINIT") + 1);
  self->initialized = false;
  if (self->hello) self->hello->lpVtbl->release(self->hello);
  self->hello = nullptr;
  if (self->host) self->host->lpVtbl->release(self->host);
  self->host = nullptr;
  return Steinberg_kResultOk;
}
/** Activated, and connected by now: the message goes to the controller, and
 *  the processor lets go of it -- what that release leaves is what anyone
 *  else still holds. */
Steinberg_tresult SMTG_STDMETHODCALLTYPE cSetActive(void* p, Steinberg_TBool state) {
  Processor* self = proc<CFace>(p);
  if (state && self->hello && self->peer) {
    writeEnv("SONORE_SPLIT_HELLO_SENT", self->peer->lpVtbl->notify(self->peer, self->hello) == Steinberg_kResultOk);
    writeEnv("SONORE_SPLIT_HELLO_LEFT", (int) self->hello->lpVtbl->release(self->hello));
    self->hello = nullptr;
  }
  return Steinberg_kResultOk;
}

Steinberg_Vst_IComponentVtbl kComponentVtbl = {
    cQuery,    cAddRef,  cRelease, cInitialize,  cTerminate, cControllerId, cIoMode,
    cBusCount, cBusInfo, cRouting, cActivateBus, cSetActive, okStream,      okStream,
};

Steinberg_tresult SMTG_STDMETHODCALLTYPE aQuery(void* p, const Steinberg_TUID iid, void** o) {
  return queryProcessor(proc<AFace>(p), iid, o);
}
Steinberg_uint32 SMTG_STDMETHODCALLTYPE aAddRef(void* p) {
  return (Steinberg_uint32) (proc<AFace>(p)->refs.fetch_add(1) + 1);
}
Steinberg_uint32 SMTG_STDMETHODCALLTYPE aRelease(void* p) { return releaseProcessor(proc<AFace>(p)); }
Steinberg_tresult SMTG_STDMETHODCALLTYPE aSetArrangements(void*, Steinberg_Vst_SpeakerArrangement*, Steinberg_int32,
                                                          Steinberg_Vst_SpeakerArrangement*, Steinberg_int32) {
  return Steinberg_kResultOk;
}
Steinberg_tresult SMTG_STDMETHODCALLTYPE aGetArrangement(void*, Steinberg_Vst_BusDirection, Steinberg_int32 index,
                                                         Steinberg_Vst_SpeakerArrangement* arr) {
  if (index != 0 || !arr) return Steinberg_kInvalidArgument;
  *arr = Steinberg_Vst_SpeakerArr_kStereo;
  return Steinberg_kResultOk;
}
Steinberg_tresult SMTG_STDMETHODCALLTYPE aCanProcess(void*, Steinberg_int32 size) {
  return size == Steinberg_Vst_SymbolicSampleSizes_kSample32 ? Steinberg_kResultOk : Steinberg_kResultFalse;
}
Steinberg_uint32 SMTG_STDMETHODCALLTYPE zero(void*) { return 0; }
Steinberg_tresult SMTG_STDMETHODCALLTYPE aSetup(void*, struct Steinberg_Vst_ProcessSetup*) {
  return Steinberg_kResultOk;
}
Steinberg_tresult SMTG_STDMETHODCALLTYPE aProcess(void* p, struct Steinberg_Vst_ProcessData* data) {
  if (!data) return Steinberg_kResultOk;
  // A message from the audio thread, which the format forbids and plugins
  // send anyway: the controller's notify() is UI code, and must not run here.
  Processor* self = proc<AFace>(p);
  if (readEnv("SONORE_SPLIT_RT_MSG") && self->peer) {
    if (Steinberg_Vst_IMessage* m = allocateMessage(self->host, "SonoreFromAudio")) {
      if (self->peer->lpVtbl->notify(self->peer, m) == Steinberg_kResultOk)
        writeEnv("SONORE_SPLIT_RT_DELIVERED", readEnv("SONORE_SPLIT_RT_DELIVERED") + 1);
      m->lpVtbl->release(m);
    }
  }
  if (Steinberg_Vst_IComponentHandler* handler = g_handler.load()) {
    if (readEnv("SONORE_SPLIT_RT_EDITS")) {
      static int calls = 0;
      handler->lpVtbl->beginEdit(handler, 100);
      handler->lpVtbl->performEdit(handler, 100, (double) (++calls % 1000) / 1000.0);
      handler->lpVtbl->endEdit(handler, 100);
      if (calls % 8 == 0)
        handler->lpVtbl->restartComponent(handler, Steinberg_Vst_RestartFlags_kParamValuesChanged);
    }
  }
  // Every event must fall inside its block: a plugin rendering up to an
  // event's offset renders past its buffers for one that does not.
  if (Steinberg_Vst_IEventList* events = data->inputEvents) {
    const Steinberg_int32 n = events->lpVtbl->getEventCount(events);
    for (Steinberg_int32 i = 0; i < n; ++i) {
      Steinberg_Vst_Event e{};
      if (events->lpVtbl->getEvent(events, i, &e) != Steinberg_kResultOk) continue;
      if (e.sampleOffset < 0 || e.sampleOffset >= data->numSamples)
        writeEnv("SONORE_SPLIT_LATE_EVENTS", readEnv("SONORE_SPLIT_LATE_EVENTS") + 1);
    }
  }
  // The last value each parameter arrived with, in thousandths.
  if (Steinberg_Vst_IParameterChanges* changes = data->inputParameterChanges) {
    const Steinberg_int32 n = changes->lpVtbl->getParameterCount(changes);
    for (Steinberg_int32 i = 0; i < n; ++i) {
      Steinberg_Vst_IParamValueQueue* queue = changes->lpVtbl->getParameterData(changes, i);
      if (!queue) continue;
      const Steinberg_int32 points = queue->lpVtbl->getPointCount(queue);
      Steinberg_int32 offset = 0;
      Steinberg_Vst_ParamValue value = 0.0;
      if (points < 1 || queue->lpVtbl->getPoint(queue, points - 1, &offset, &value) != Steinberg_kResultOk)
        continue;
      char name[48];
      std::snprintf(name, sizeof(name), "SONORE_SPLIT_P%u", (unsigned) queue->lpVtbl->getParameterId(queue));
      writeEnv(name, (int) (value * 1000.0 + 0.5));
    }
  }
  if (Steinberg_Vst_IParameterChanges* out = data->outputParameterChanges) {
    static int blocks = 0;
    ++blocks;
    const Steinberg_Vst_ParamID meter = 300;
    Steinberg_int32 index = -1, again = -1, point = 0;
    if (Steinberg_Vst_IParamValueQueue* q = out->lpVtbl->addParameterData(out, &meter, &index))
      q->lpVtbl->addPoint(q, 0, 0.375, &point);
    // Asked for again in the same block, it must be the same queue.
    const double last = blocks == 1 ? 0.625 : (double) (blocks % 997) / 1000.0;
    if (Steinberg_Vst_IParamValueQueue* q = out->lpVtbl->addParameterData(out, &meter, &again))
      if (again == index) q->lpVtbl->addPoint(q, data->numSamples - 1, last, &point);
  }
  if (data->numInputs > 0 && data->numOutputs > 0)
    for (int c = 0; c < data->outputs[0].numChannels && c < data->inputs[0].numChannels; ++c)
      std::memcpy(data->outputs[0].Steinberg_Vst_AudioBusBuffers_channelBuffers32[c],
                  data->inputs[0].Steinberg_Vst_AudioBusBuffers_channelBuffers32[c],
                  (size_t) data->numSamples * sizeof(float));
  return Steinberg_kResultOk;
}
Steinberg_Vst_IAudioProcessorVtbl kAudioVtbl = {
    aQuery, aAddRef, aRelease, aSetArrangements, aGetArrangement, aCanProcess,
    zero,   aSetup,  okBool,   aProcess,         zero,
};

Steinberg_tresult SMTG_STDMETHODCALLTYPE pQuery(void* p, const Steinberg_TUID iid, void** o) {
  return queryProcessor(proc<PFace>(p), iid, o);
}
Steinberg_uint32 SMTG_STDMETHODCALLTYPE pAddRef(void* p) {
  return (Steinberg_uint32) (proc<PFace>(p)->refs.fetch_add(1) + 1);
}
Steinberg_uint32 SMTG_STDMETHODCALLTYPE pRelease(void* p) { return releaseProcessor(proc<PFace>(p)); }
Steinberg_tresult SMTG_STDMETHODCALLTYPE pConnect(void* p, struct Steinberg_Vst_IConnectionPoint* other) {
  return connectTo(&proc<PFace>(p)->peer, other);
}
Steinberg_tresult SMTG_STDMETHODCALLTYPE pDisconnect(void* p, struct Steinberg_Vst_IConnectionPoint*) {
  return disconnectFrom(&proc<PFace>(p)->peer);
}
/** What the controller sends, counted. */
Steinberg_tresult SMTG_STDMETHODCALLTYPE pNotify(void*, struct Steinberg_Vst_IMessage* m) {
  if (!m) return Steinberg_kInvalidArgument;
  const char* id = m->lpVtbl->getMessageID(m);
  if (id && std::strcmp(id, "SonoreFromController") == 0)
    writeEnv("SONORE_SPLIT_CTRL_HEARD", readEnv("SONORE_SPLIT_CTRL_HEARD") + 1);
  return Steinberg_kResultOk;
}
Steinberg_Vst_IConnectionPointVtbl kProcessorConnectionVtbl = {pQuery,   pAddRef,     pRelease,
                                                               pConnect, pDisconnect, pNotify};

// ── The controller ──────────────────────────────────────────────────────────

struct Controller;
template <typename Vtbl>
struct CtrlFace {
  Vtbl* lpVtbl;
  Controller* self;
};
struct Controller {
  CtrlFace<Steinberg_Vst_IEditControllerVtbl> edit;
  CtrlFace<Steinberg_Vst_IConnectionPointVtbl> connection;
  std::atomic<int> refs{1};
  double values[3] = {0.5, 0.5, 0.0};
  bool initialized = false;
  Steinberg_Vst_IHostApplication* host = nullptr;
  Steinberg_Vst_IConnectionPoint* peer = nullptr;
  /** State only the controller has, as an editor's open tab is: taken from
   *  SONORE_SPLIT_TAB at initialize(), saved by getState(), and what
   *  setState() is given is recorded as SONORE_SPLIT_RESTORED_TAB. */
  int32_t tab = 0;
};
using EFace = CtrlFace<Steinberg_Vst_IEditControllerVtbl>;
using KFace = CtrlFace<Steinberg_Vst_IConnectionPointVtbl>;
template <typename F>
Controller* ctrl(void* p) {
  return static_cast<F*>(p)->self;
}

Steinberg_tresult queryController(Controller* c, const Steinberg_TUID iid, void** obj) {
  if (!obj) return Steinberg_kInvalidArgument;
  if (sameIid(iid, Steinberg_FUnknown_iid) || sameIid(iid, Steinberg_IPluginBase_iid) ||
      sameIid(iid, Steinberg_Vst_IEditController_iid))
    *obj = &c->edit;
  else if (sameIid(iid, Steinberg_Vst_IConnectionPoint_iid))
    *obj = &c->connection;
  else {
    *obj = nullptr;
    return Steinberg_kNoInterface;
  }
  c->refs.fetch_add(1);
  return Steinberg_kResultOk;
}
Steinberg_uint32 releaseController(Controller* c) {
  const int left = c->refs.fetch_sub(1) - 1;
  if (left == 0) delete c;
  return (Steinberg_uint32) left;
}

int slotOf(Steinberg_Vst_ParamID id) {
  return id == 100 ? 0 : (id == 200 ? 1 : (id == 300 ? 2 : -1));
}

Steinberg_tresult SMTG_STDMETHODCALLTYPE eQuery(void* p, const Steinberg_TUID iid, void** o) {
  return queryController(ctrl<EFace>(p), iid, o);
}
Steinberg_uint32 SMTG_STDMETHODCALLTYPE eAddRef(void* p) {
  return (Steinberg_uint32) (ctrl<EFace>(p)->refs.fetch_add(1) + 1);
}
Steinberg_uint32 SMTG_STDMETHODCALLTYPE eRelease(void* p) { return releaseController(ctrl<EFace>(p)); }
Steinberg_int32 SMTG_STDMETHODCALLTYPE eCount(void*) { return 3; }
Steinberg_tresult SMTG_STDMETHODCALLTYPE eInfo(void*, Steinberg_int32 index, struct Steinberg_Vst_ParameterInfo* info) {
  if (!info || index < 0 || index > 2) return Steinberg_kInvalidArgument;
  std::memset(info, 0, sizeof(*info));
  const Steinberg_Vst_ParamID ids[3] = {100, 200, 300};
  const char* const titles[3] = {"Gain", "Hidden", "Meter"};
  info->id = ids[index];
  for (int i = 0; titles[index][i]; ++i) info->title[i] = (Steinberg_char16) titles[index][i];
  info->defaultNormalizedValue = index == 2 ? 0.0 : 0.5;
  info->flags = index == 0   ? Steinberg_Vst_ParameterInfo_ParameterFlags_kCanAutomate
                : index == 1 ? Steinberg_Vst_ParameterInfo_ParameterFlags_kIsHidden
                             : Steinberg_Vst_ParameterInfo_ParameterFlags_kIsReadOnly;
  return Steinberg_kResultOk;
}
Steinberg_tresult SMTG_STDMETHODCALLTYPE eString(void*, Steinberg_Vst_ParamID, Steinberg_Vst_ParamValue,
                                                 Steinberg_Vst_String128) {
  return Steinberg_kResultFalse;
}
Steinberg_tresult SMTG_STDMETHODCALLTYPE eValue(void*, Steinberg_Vst_ParamID, Steinberg_Vst_TChar*,
                                                Steinberg_Vst_ParamValue*) {
  return Steinberg_kResultFalse;
}
Steinberg_Vst_ParamValue SMTG_STDMETHODCALLTYPE eIdentity(void*, Steinberg_Vst_ParamID, Steinberg_Vst_ParamValue v) {
  return v;
}
Steinberg_Vst_ParamValue SMTG_STDMETHODCALLTYPE eGet(void* p, Steinberg_Vst_ParamID id) {
  const int slot = slotOf(id);
  return slot < 0 ? 0.0 : ctrl<EFace>(p)->values[slot];
}
Steinberg_tresult SMTG_STDMETHODCALLTYPE eSet(void* p, Steinberg_Vst_ParamID id, Steinberg_Vst_ParamValue v) {
  const int slot = slotOf(id);
  if (slot < 0) return Steinberg_kInvalidArgument;
  ctrl<EFace>(p)->values[slot] = v;
  return Steinberg_kResultOk;
}
Steinberg_tresult SMTG_STDMETHODCALLTYPE eHandler(void* p, struct Steinberg_Vst_IComponentHandler* handler) {
  g_handler.store(handler);
  if (!handler) return Steinberg_kResultOk;
  // What turning a knob on the editor does: a gesture on the visible
  // parameter, and a hidden one moved with it.
  Controller* c = ctrl<EFace>(p);
  c->values[0] = 0.75;
  c->values[1] = 0.25;
  handler->lpVtbl->beginEdit(handler, 100);
  handler->lpVtbl->performEdit(handler, 100, 0.75);
  handler->lpVtbl->endEdit(handler, 100);
  handler->lpVtbl->performEdit(handler, 200, 0.25);
  // And the session has changed where no parameter shows it, as after a
  // file picked in the editor.
  Steinberg_Vst_IComponentHandler2* second = nullptr;
  if (handler->lpVtbl->queryInterface(handler, Steinberg_Vst_IComponentHandler2_iid,
                                      (void**) &second) == Steinberg_kResultOk &&
      second) {
    second->lpVtbl->setDirty(second, 1);
    second->lpVtbl->release(second);
  }
  // And a word to the processor, on the main thread, where the format has it.
  if (c->peer) {
    if (Steinberg_Vst_IMessage* m = allocateMessage(c->host, "SonoreFromController")) {
      c->peer->lpVtbl->notify(c->peer, m);
      m->lpVtbl->release(m);
    }
  }
  return Steinberg_kResultOk;
}
// ── Its editor, which resizes itself ────────────────────────────────────────
//
// A view that asks its frame for 640x480 as soon as it is attached -- as an
// editor does when its corner is dragged or its zoom changed -- and changes
// size only when onSize() gives it one, as Steinberg's own views do. It
// records the frame's answer as SONORE_SPLIT_RESIZE_ANSWER (1 for
// kResultTrue) and the size onSize() gave it as SONORE_SPLIT_RESIZE_ONSIZE.

struct View {
  Steinberg_IPlugViewVtbl* lpVtbl = nullptr;
  std::atomic<int> refs{1};
  Steinberg_IPlugFrame* frame = nullptr;
  Steinberg_int32 width = 320, height = 200;
};
View* view(void* p) { return static_cast<View*>(p); }

Steinberg_uint32 SMTG_STDMETHODCALLTYPE vAddRef(void* p) {
  return (Steinberg_uint32) (view(p)->refs.fetch_add(1) + 1);
}
Steinberg_uint32 SMTG_STDMETHODCALLTYPE vRelease(void* p) {
  const int left = view(p)->refs.fetch_sub(1) - 1;
  if (left == 0) delete view(p);
  return (Steinberg_uint32) left;
}
Steinberg_tresult SMTG_STDMETHODCALLTYPE vQuery(void* p, const Steinberg_TUID iid, void** o) {
  if (!o) return Steinberg_kInvalidArgument;
  if (sameIid(iid, Steinberg_FUnknown_iid) || sameIid(iid, Steinberg_IPlugView_iid)) {
    vAddRef(p);
    *o = p;
    return Steinberg_kResultOk;
  }
  *o = nullptr;
  return Steinberg_kNoInterface;
}
Steinberg_tresult SMTG_STDMETHODCALLTYPE vSupported(void*, Steinberg_FIDString) { return Steinberg_kResultOk; }
Steinberg_tresult SMTG_STDMETHODCALLTYPE vAttached(void* p, void* parent, Steinberg_FIDString) {
  if (!parent) return Steinberg_kResultFalse;
  View* v = view(p);
  if (v->frame) {
    Steinberg_ViewRect wanted{0, 0, 640, 480};
    const Steinberg_tresult answer =
        v->frame->lpVtbl->resizeView(v->frame, (Steinberg_IPlugView*) v, &wanted);
    writeEnv("SONORE_SPLIT_RESIZE_ANSWER", answer == Steinberg_kResultTrue ? 1 : 0);
  }
  return Steinberg_kResultOk;
}
Steinberg_tresult SMTG_STDMETHODCALLTYPE vNothing(void*) { return Steinberg_kResultOk; }
Steinberg_tresult SMTG_STDMETHODCALLTYPE vWheel(void*, float) { return Steinberg_kResultFalse; }
Steinberg_tresult SMTG_STDMETHODCALLTYPE vKey(void*, Steinberg_char16, Steinberg_int16, Steinberg_int16) {
  return Steinberg_kResultFalse;
}
Steinberg_tresult SMTG_STDMETHODCALLTYPE vGetSize(void* p, struct Steinberg_ViewRect* rect) {
  if (!rect) return Steinberg_kInvalidArgument;
  *rect = Steinberg_ViewRect{0, 0, view(p)->width, view(p)->height};
  return Steinberg_kResultOk;
}
Steinberg_tresult SMTG_STDMETHODCALLTYPE vOnSize(void* p, struct Steinberg_ViewRect* rect) {
  if (!rect) return Steinberg_kInvalidArgument;
  view(p)->width = rect->right - rect->left;
  view(p)->height = rect->bottom - rect->top;
  char text[32];
  std::snprintf(text, sizeof(text), "%dx%d", (int) view(p)->width, (int) view(p)->height);
  writeEnvText("SONORE_SPLIT_RESIZE_ONSIZE", text);
  return Steinberg_kResultOk;
}
Steinberg_tresult SMTG_STDMETHODCALLTYPE vFocus(void*, Steinberg_TBool) { return Steinberg_kResultOk; }
Steinberg_tresult SMTG_STDMETHODCALLTYPE vSetFrame(void* p, struct Steinberg_IPlugFrame* frame) {
  view(p)->frame = frame;
  return Steinberg_kResultOk;
}
Steinberg_tresult SMTG_STDMETHODCALLTYPE vConstraint(void*, struct Steinberg_ViewRect*) {
  return Steinberg_kResultOk;
}
Steinberg_IPlugViewVtbl kViewVtbl = {
    vQuery, vAddRef,  vRelease, vSupported, vAttached, vNothing,  vWheel,
    vKey,   vKey,     vGetSize, vOnSize,    vFocus,    vSetFrame, vNothing,
    vConstraint,
};

struct Steinberg_IPlugView* SMTG_STDMETHODCALLTYPE eView(void*, Steinberg_FIDString name) {
  if (!name || std::strcmp(name, "editor") != 0) return nullptr;
  auto* v = new View();
  v->lpVtbl = &kViewVtbl;
  return (Steinberg_IPlugView*) v;
}

/** The host's name and what it says it supports, from the context; the
 *  application kept for the messages this half sends. */
Steinberg_tresult SMTG_STDMETHODCALLTYPE eInitialize(void* p, struct Steinberg_FUnknown* context) {
  Controller* c = ctrl<EFace>(p);
  if (readEnv("SONORE_SPLIT_REFUSE_CTRL_INIT")) return Steinberg_kResultFalse;
  c->initialized = true;
  c->host = hostApplicationOf(context);
  c->tab = readEnv("SONORE_SPLIT_TAB");
  char name[129] = {0};
  Steinberg_Vst_String128 wide{};
  if (c->host && c->host->lpVtbl->getName(c->host, wide) == Steinberg_kResultOk)
    for (int i = 0; i < 128 && wide[i]; ++i) name[i] = wide[i] < 128 ? (char) wide[i] : '?';
  writeEnvText("SONORE_SPLIT_HOST_NAME", name[0] ? name : "(none)");
  Steinberg_Vst_IPlugInterfaceSupport* support = nullptr;
  int supports = 0;
  if (context &&
      context->lpVtbl->queryInterface(context, Steinberg_Vst_IPlugInterfaceSupport_iid,
                                      (void**) &support) == Steinberg_kResultOk &&
      support) {
    // What it uses, yes; what it never asks a plugin for, no.
    if (support->lpVtbl->isPlugInterfaceSupported(support, Steinberg_Vst_IConnectionPoint_iid) ==
        Steinberg_kResultOk)
      supports |= 1;
    if (support->lpVtbl->isPlugInterfaceSupported(support, Steinberg_Vst_IMidiMapping_iid) ==
        Steinberg_kResultOk)
      supports |= 2;
    if (support->lpVtbl->isPlugInterfaceSupported(support, Steinberg_Vst_IPrefetchableSupport_iid) !=
        Steinberg_kResultOk)
      supports |= 4;
    support->lpVtbl->release(support);
  }
  writeEnv("SONORE_SPLIT_SUPPORT", supports);
  return Steinberg_kResultOk;
}
Steinberg_tresult SMTG_STDMETHODCALLTYPE eTerminate(void* p) {
  Controller* c = ctrl<EFace>(p);
  if (!c->initialized)
    writeEnv("SONORE_SPLIT_TERMINATED_UNINIT", readEnv("SONORE_SPLIT_TERMINATED_UNINIT") + 1);
  c->initialized = false;
  if (c->host) c->host->lpVtbl->release(c->host);
  c->host = nullptr;
  return Steinberg_kResultOk;
}
Steinberg_tresult SMTG_STDMETHODCALLTYPE eSetState(void* p, struct Steinberg_IBStream* stream) {
  Controller* c = ctrl<EFace>(p);
  int32_t tab = 0;
  Steinberg_int32 got = 0;
  if (!stream || stream->lpVtbl->read(stream, &tab, (Steinberg_int32) sizeof(tab), &got) != Steinberg_kResultOk ||
      got != (Steinberg_int32) sizeof(tab))
    return Steinberg_kResultFalse;
  c->tab = tab;
  writeEnv("SONORE_SPLIT_RESTORED_TAB", tab);
  return Steinberg_kResultOk;
}
Steinberg_tresult SMTG_STDMETHODCALLTYPE eGetState(void* p, struct Steinberg_IBStream* stream) {
  Controller* c = ctrl<EFace>(p);
  Steinberg_int32 put = 0;
  if (!stream || stream->lpVtbl->write(stream, &c->tab, (Steinberg_int32) sizeof(c->tab), &put) != Steinberg_kResultOk ||
      put != (Steinberg_int32) sizeof(c->tab))
    return Steinberg_kResultFalse;
  return Steinberg_kResultOk;
}
Steinberg_Vst_IEditControllerVtbl kEditVtbl = {
    eQuery, eAddRef, eRelease, eInitialize, eTerminate, okStream, eSetState, eGetState, eCount,
    eInfo,  eString, eValue,   eIdentity,   eIdentity,  eGet,     eSet,     eHandler, eView,
};

Steinberg_tresult SMTG_STDMETHODCALLTYPE kQuery(void* p, const Steinberg_TUID iid, void** o) {
  return queryController(ctrl<KFace>(p), iid, o);
}
Steinberg_uint32 SMTG_STDMETHODCALLTYPE kAddRef(void* p) {
  return (Steinberg_uint32) (ctrl<KFace>(p)->refs.fetch_add(1) + 1);
}
Steinberg_uint32 SMTG_STDMETHODCALLTYPE kRelease(void* p) { return releaseController(ctrl<KFace>(p)); }
Steinberg_tresult SMTG_STDMETHODCALLTYPE kConnect(void* p, struct Steinberg_Vst_IConnectionPoint* other) {
  return connectTo(&ctrl<KFace>(p)->peer, other);
}
Steinberg_tresult SMTG_STDMETHODCALLTYPE kDisconnect(void* p, struct Steinberg_Vst_IConnectionPoint*) {
  return disconnectFrom(&ctrl<KFace>(p)->peer);
}
/** What the processor sends: its hello, checked attribute by attribute, and
 *  anything it sent from process(), which must never get here. */
Steinberg_tresult SMTG_STDMETHODCALLTYPE kNotify(void*, struct Steinberg_Vst_IMessage* m) {
  if (!m) return Steinberg_kInvalidArgument;
  const char* id = m->lpVtbl->getMessageID(m);
  if (id && std::strcmp(id, "SonoreHello") == 0) {
    writeEnv("SONORE_SPLIT_HELLO_HEARD", readEnv("SONORE_SPLIT_HELLO_HEARD") + 1);
    writeEnv("SONORE_SPLIT_HELLO_INTACT", checkHello(m));
  }
  if (id && std::strcmp(id, "SonoreFromAudio") == 0)
    writeEnv("SONORE_SPLIT_RT_HEARD", readEnv("SONORE_SPLIT_RT_HEARD") + 1);
  return Steinberg_kResultOk;
}
Steinberg_Vst_IConnectionPointVtbl kControllerConnectionVtbl = {kQuery,   kAddRef,     kRelease,
                                                                kConnect, kDisconnect, kNotify};

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
Steinberg_tresult SMTG_STDMETHODCALLTYPE fInfo(void*, struct Steinberg_PFactoryInfo* info) {
  if (!info) return Steinberg_kInvalidArgument;
  std::memset(info, 0, sizeof(*info));
  std::snprintf(info->vendor, sizeof(info->vendor), "Sonorie");
  return Steinberg_kResultOk;
}
Steinberg_int32 SMTG_STDMETHODCALLTYPE fCount(void*) { return 2; }
Steinberg_tresult SMTG_STDMETHODCALLTYPE fClassInfo(void*, Steinberg_int32 index, struct Steinberg_PClassInfo* info) {
  if (!info || index < 0 || index > 1) return Steinberg_kInvalidArgument;
  std::memset(info, 0, sizeof(*info));
  std::memcpy(info->cid, index == 0 ? kProcessorCid : kControllerCid, 16);
  info->cardinality = 0x7FFFFFFF;
  std::snprintf(info->category, sizeof(info->category), "%s",
                index == 0 ? "Audio Module Class" : "Component Controller Class");
  std::snprintf(info->name, sizeof(info->name), "Sonore Split Fixture");
  return Steinberg_kResultOk;
}
Steinberg_tresult SMTG_STDMETHODCALLTYPE fCreate(void*, Steinberg_FIDString cid, Steinberg_FIDString iid, void** obj) {
  if (!obj) return Steinberg_kInvalidArgument;
  *obj = nullptr;
  if (std::memcmp(cid, kProcessorCid, 16) == 0) {
    auto* p = new Processor();
    p->component = {&kComponentVtbl, p};
    p->audio = {&kAudioVtbl, p};
    p->connection = {&kProcessorConnectionVtbl, p};
    const Steinberg_tresult r = queryProcessor(p, iid, obj);
    releaseProcessor(p);
    return r;
  }
  if (std::memcmp(cid, kControllerCid, 16) == 0) {
    auto* c = new Controller();
    c->edit = {&kEditVtbl, c};
    c->connection = {&kControllerConnectionVtbl, c};
    const Steinberg_tresult r = queryController(c, iid, obj);
    releaseController(c);
    return r;
  }
  return Steinberg_kInvalidArgument;
}

Steinberg_IPluginFactoryVtbl kFactoryVtbl = {fQuery, fRef, fRef, fInfo, fCount, fClassInfo, fCreate};
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
