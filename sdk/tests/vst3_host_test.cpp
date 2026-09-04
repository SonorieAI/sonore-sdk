// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the VST3 wrapper driven exactly as a DAW drives it.
//
// Loads a BUILT .vst3 the way a host does (module entry -> factory -> class ->
// createInstance -> queryInterface -> initialize -> buses -> setup -> active ->
// process), pushes real parameter automation and note events through the VST3
// structures, round-trips state, and creates the editor view.
//
// The parallel of clap_host_test, and for the same reason: what is tested is the
// shipped artifact, not a second build of the same source. VST3's own quirks:
// normalised parameters, stepCount being steps-BETWEEN-values, COM refcounting:
// are exactly where a wrapper goes silently wrong, so each gets an assertion.
#include <sonore/midi_ci.h>
#include <vst3_c_api.h>

#include <cmath>
#include <limits>
#include <random>
#include <cstdio>
#include <cstring>
#include <string>
#include <array>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
using LibHandle = HMODULE;
static LibHandle openLib(const char* p) { return LoadLibraryA(p); }
static void* symbolOf(LibHandle h, const char* n) { return (void*) GetProcAddress(h, n); }
static void closeLib(LibHandle h) { FreeLibrary(h); }
#else
#include <dlfcn.h>
using LibHandle = void*;
static LibHandle openLib(const char* p) { return dlopen(p, RTLD_LOCAL | RTLD_NOW); }
static void* symbolOf(LibHandle h, const char* n) { return dlsym(h, n); }
static void closeLib(LibHandle h) { dlclose(h); }
#endif

static int g_failures = 0;
static int g_checks = 0;

static void check(bool ok, const char* what) {
  ++g_checks;
  std::printf(ok ? "  ok   %s\n" : "  FAIL %s\n", what);
  if (!ok) ++g_failures;
}

static void checkNear(double got, double want, double tol, const char* what) {
  ++g_checks;
  if (std::fabs(got - want) <= tol) {
    std::printf("  ok   %s (%.4f)\n", what, got);
  } else {
    ++g_failures;
    std::printf("  FAIL %s: got %.4f, want %.4f +/- %.4f\n", what, got, want, tol);
  }
}

// ── A memory IBStream, for the state round-trip ──────────────────────────────

struct MemStream {
  Steinberg_IBStreamVtbl* lpVtbl = nullptr;
  std::vector<uint8_t> bytes;
  size_t pos = 0;
};

static Steinberg_tresult SMTG_STDMETHODCALLTYPE streamQuery(void*, const Steinberg_TUID, void** o) {
  if (o) *o = nullptr;
  return Steinberg_kNoInterface;
}
static Steinberg_uint32 SMTG_STDMETHODCALLTYPE streamAddRef(void*) { return 1; }
static Steinberg_uint32 SMTG_STDMETHODCALLTYPE streamRelease(void*) { return 1; }

static Steinberg_tresult SMTG_STDMETHODCALLTYPE streamRead(void* self, void* buffer,
                                                           Steinberg_int32 numBytes,
                                                           Steinberg_int32* read) {
  auto* s = (MemStream*) self;
  const size_t left = s->bytes.size() - s->pos;
  const size_t take = (size_t) numBytes < left ? (size_t) numBytes : left;
  if (take) std::memcpy(buffer, s->bytes.data() + s->pos, take);
  s->pos += take;
  if (read) *read = (Steinberg_int32) take;
  return Steinberg_kResultOk;
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE streamWrite(void* self, void* buffer,
                                                            Steinberg_int32 numBytes,
                                                            Steinberg_int32* written) {
  auto* s = (MemStream*) self;
  const auto* p = (const uint8_t*) buffer;
  s->bytes.insert(s->bytes.end(), p, p + numBytes);
  if (written) *written = numBytes;
  return Steinberg_kResultOk;
}

static Steinberg_tresult SMTG_STDMETHODCALLTYPE streamSeek(void*, Steinberg_int64, Steinberg_int32,
                                                           Steinberg_int64*) {
  return Steinberg_kResultOk;
}
static Steinberg_tresult SMTG_STDMETHODCALLTYPE streamTell(void* self, Steinberg_int64* pos) {
  if (pos) *pos = (Steinberg_int64) ((MemStream*) self)->pos;
  return Steinberg_kResultOk;
}

static Steinberg_IBStreamVtbl g_streamVtbl = {streamQuery, streamAddRef, streamRelease,
                                              streamRead,  streamWrite, streamSeek, streamTell};

// ── A parameter-change queue the plugin can read ─────────────────────────────

struct ValueQueue {
  Steinberg_Vst_IParamValueQueueVtbl* lpVtbl = nullptr;
  Steinberg_Vst_ParamID id = 0;
  double value = 0.0;
};

static Steinberg_tresult SMTG_STDMETHODCALLTYPE vqQuery(void*, const Steinberg_TUID, void** o) {
  if (o) *o = nullptr;
  return Steinberg_kNoInterface;
}
static Steinberg_uint32 SMTG_STDMETHODCALLTYPE vqAddRef(void*) { return 1; }
static Steinberg_uint32 SMTG_STDMETHODCALLTYPE vqRelease(void*) { return 1; }
static Steinberg_Vst_ParamID SMTG_STDMETHODCALLTYPE vqGetId(void* self) {
  return ((ValueQueue*) self)->id;
}
static Steinberg_int32 SMTG_STDMETHODCALLTYPE vqCount(void*) { return 1; }
static Steinberg_tresult SMTG_STDMETHODCALLTYPE vqGetPoint(void* self, Steinberg_int32 index,
                                                           Steinberg_int32* offset,
                                                           Steinberg_Vst_ParamValue* value) {
  if (index != 0) return Steinberg_kInvalidArgument;
  if (offset) *offset = 0;
  if (value) *value = ((ValueQueue*) self)->value;
  return Steinberg_kResultOk;
}
static Steinberg_tresult SMTG_STDMETHODCALLTYPE vqAddPoint(void*, Steinberg_int32,
                                                           Steinberg_Vst_ParamValue,
                                                           Steinberg_int32*) {
  return Steinberg_kResultOk;
}
static Steinberg_Vst_IParamValueQueueVtbl g_vqVtbl = {vqQuery,    vqAddRef,  vqRelease, vqGetId,
                                                      vqCount,    vqGetPoint, vqAddPoint};

struct ParamChanges {
  Steinberg_Vst_IParameterChangesVtbl* lpVtbl = nullptr;
  std::vector<ValueQueue>* queues = nullptr;
};

static Steinberg_tresult SMTG_STDMETHODCALLTYPE pcQuery(void*, const Steinberg_TUID, void** o) {
  if (o) *o = nullptr;
  return Steinberg_kNoInterface;
}
static Steinberg_uint32 SMTG_STDMETHODCALLTYPE pcAddRef(void*) { return 1; }
static Steinberg_uint32 SMTG_STDMETHODCALLTYPE pcRelease(void*) { return 1; }
static Steinberg_int32 SMTG_STDMETHODCALLTYPE pcCount(void* self) {
  return (Steinberg_int32) ((ParamChanges*) self)->queues->size();
}
static Steinberg_Vst_IParamValueQueue* SMTG_STDMETHODCALLTYPE pcGetData(void* self,
                                                                       Steinberg_int32 index) {
  auto* pc = (ParamChanges*) self;
  if (index < 0 || index >= (Steinberg_int32) pc->queues->size()) return nullptr;
  return (Steinberg_Vst_IParamValueQueue*) &(*pc->queues)[index];
}
static Steinberg_Vst_IParamValueQueue* SMTG_STDMETHODCALLTYPE pcAddData(
    void*, const Steinberg_Vst_ParamID*, Steinberg_int32*) {
  return nullptr;
}
static Steinberg_Vst_IParameterChangesVtbl g_pcVtbl = {pcQuery,  pcAddRef,  pcRelease,
                                                       pcCount,  pcGetData, pcAddData};

// ── An event list ────────────────────────────────────────────────────────────

struct EventList {
  Steinberg_Vst_IEventListVtbl* lpVtbl = nullptr;
  std::vector<Steinberg_Vst_Event>* events = nullptr;
};

static Steinberg_tresult SMTG_STDMETHODCALLTYPE elQuery(void*, const Steinberg_TUID, void** o) {
  if (o) *o = nullptr;
  return Steinberg_kNoInterface;
}
static Steinberg_uint32 SMTG_STDMETHODCALLTYPE elAddRef(void*) { return 1; }
static Steinberg_uint32 SMTG_STDMETHODCALLTYPE elRelease(void*) { return 1; }
static Steinberg_int32 SMTG_STDMETHODCALLTYPE elCount(void* self) {
  return (Steinberg_int32) ((EventList*) self)->events->size();
}
static Steinberg_tresult SMTG_STDMETHODCALLTYPE elGet(void* self, Steinberg_int32 index,
                                                      Steinberg_Vst_Event* e) {
  auto* el = (EventList*) self;
  if (index < 0 || index >= (Steinberg_int32) el->events->size()) return Steinberg_kInvalidArgument;
  *e = (*el->events)[index];
  return Steinberg_kResultOk;
}
/** Emitted events, captured rather than dropped: a note effect's product. */
static std::vector<Steinberg_Vst_Event> g_emitted;

/** SysEx the plugin emitted, COPIED out.
 *
 *  A kDataEvent carries a POINTER, and the event struct above is captured by
 *  value -- so keeping it would leave the test holding an address the plugin
 *  is free to reuse on the next block. The CLAP host test makes the same copy
 *  for the same reason. A test that kept the pointer would be measuring its
 *  own bug. */
static std::vector<std::vector<uint8_t>> g_emittedSysex;

static Steinberg_tresult SMTG_STDMETHODCALLTYPE elAdd(void*, Steinberg_Vst_Event* e) {
  if (e) {
    g_emitted.push_back(*e);
    if (e->type == Steinberg_Vst_Event_EventTypes_kDataEvent &&
        e->Steinberg_Vst_Event_data.type == Steinberg_Vst_DataEvent_DataTypes_kMidiSysEx &&
        e->Steinberg_Vst_Event_data.bytes && e->Steinberg_Vst_Event_data.size > 0) {
      const auto* from = e->Steinberg_Vst_Event_data.bytes;
      g_emittedSysex.push_back(
          std::vector<uint8_t>(from, from + e->Steinberg_Vst_Event_data.size));
    }
  }
  return Steinberg_kResultOk;
}

static int countEventType(Steinberg_uint16 type) {
  int n = 0;
  for (const auto& e : g_emitted)
    if (e.type == type) ++n;
  return n;
}
static Steinberg_Vst_IEventListVtbl g_elVtbl = {elQuery, elAddRef, elRelease,
                                                elCount, elGet,    elAdd};

// ── An attribute list, which is how VST3 hands over the track ──────────
//
// Pushed, not pulled: the host calls setChannelContextInfos with a bag of
// keys and the plugin reads out the ones it knows. This one carries a name
// and a colour and nothing else, which is also what a real host sends -- so a
// plugin that assumes every key is present reads garbage from a real session
// too.

struct AttrList {
  Steinberg_Vst_IAttributeListVtbl* lpVtbl = nullptr;
};
static AttrList g_attrs;

static Steinberg_tresult SMTG_STDMETHODCALLTYPE attrQuery(void*, const Steinberg_TUID, void** o) {
  if (o) *o = nullptr;
  return Steinberg_kNoInterface;
}
static Steinberg_uint32 SMTG_STDMETHODCALLTYPE attrAddRef(void*) { return 1; }
static Steinberg_uint32 SMTG_STDMETHODCALLTYPE attrRelease(void*) { return 1; }
static Steinberg_tresult SMTG_STDMETHODCALLTYPE attrSetInt(void*,
                                                           Steinberg_Vst_IAttributeList_AttrID,
                                                           Steinberg_int64) {
  return Steinberg_kNotImplemented;
}
static Steinberg_tresult SMTG_STDMETHODCALLTYPE attrGetInt(void*,
                                                           Steinberg_Vst_IAttributeList_AttrID id,
                                                           Steinberg_int64* value) {
  if (!value || !id) return Steinberg_kInvalidArgument;
  if (std::strcmp(id, Steinberg_Vst_ChannelContext_kChannelColorKey) == 0) {
    *value = (Steinberg_int64) 0xFF2AB17Cu; // ARGB, opaque
    return Steinberg_kResultOk;
  }
  if (std::strcmp(id, Steinberg_Vst_ChannelContext_kChannelNameLengthKey) == 0) {
    *value = 11;
    return Steinberg_kResultOk;
  }
  return Steinberg_kResultFalse;
}
static Steinberg_tresult SMTG_STDMETHODCALLTYPE attrSetFloat(void*,
                                                             Steinberg_Vst_IAttributeList_AttrID,
                                                             double) {
  return Steinberg_kNotImplemented;
}
static Steinberg_tresult SMTG_STDMETHODCALLTYPE attrGetFloat(void*,
                                                             Steinberg_Vst_IAttributeList_AttrID,
                                                             double*) {
  return Steinberg_kResultFalse;
}
static Steinberg_tresult SMTG_STDMETHODCALLTYPE attrSetString(void*,
                                                              Steinberg_Vst_IAttributeList_AttrID,
                                                              const Steinberg_Vst_TChar*) {
  return Steinberg_kNotImplemented;
}
static Steinberg_tresult SMTG_STDMETHODCALLTYPE
attrGetString(void*, Steinberg_Vst_IAttributeList_AttrID id, Steinberg_Vst_TChar* string,
              Steinberg_uint32 sizeInBytes) {
  if (!string || !id) return Steinberg_kInvalidArgument;
  if (std::strcmp(id, Steinberg_Vst_ChannelContext_kChannelNameKey) != 0)
    return Steinberg_kResultFalse;
  const char* text = "Verb Return";
  const size_t room = sizeInBytes / sizeof(Steinberg_Vst_TChar);
  size_t i = 0;
  for (; text[i] && i + 1 < room; ++i) string[i] = (Steinberg_Vst_TChar) text[i];
  string[i] = 0;
  return Steinberg_kResultOk;
}
static Steinberg_tresult SMTG_STDMETHODCALLTYPE attrSetBinary(void*,
                                                              Steinberg_Vst_IAttributeList_AttrID,
                                                              const void*, Steinberg_uint32) {
  return Steinberg_kNotImplemented;
}
static Steinberg_tresult SMTG_STDMETHODCALLTYPE attrGetBinary(void*,
                                                              Steinberg_Vst_IAttributeList_AttrID,
                                                              const void**, Steinberg_uint32*) {
  return Steinberg_kResultFalse;
}

// ── A host application, so the plugin can find out whose house it is in ──
//
// VST3 makes a plugin ASK: the context handed to initialize may or may not be
// an IHostApplication, and that is the only place a host names itself. A test
// host that passes nullptr -- which this one did -- proves only that the
// plugin survives not being told.

struct HostApp {
  Steinberg_Vst_IHostApplicationVtbl* lpVtbl = nullptr;
};
static HostApp g_hostApp;

static Steinberg_tresult SMTG_STDMETHODCALLTYPE appQuery(void* self, const Steinberg_TUID iid,
                                                         void** obj) {
  if (!obj) return Steinberg_kInvalidArgument;
  if (std::memcmp(iid, Steinberg_Vst_IHostApplication_iid, sizeof(Steinberg_TUID)) == 0 ||
      std::memcmp(iid, Steinberg_FUnknown_iid, sizeof(Steinberg_TUID)) == 0) {
    *obj = self;
    return Steinberg_kResultOk;
  }
  *obj = nullptr;
  return Steinberg_kNoInterface;
}
static Steinberg_uint32 SMTG_STDMETHODCALLTYPE appAddRef(void*) { return 1; }
static Steinberg_uint32 SMTG_STDMETHODCALLTYPE appRelease(void*) { return 1; }
static Steinberg_tresult SMTG_STDMETHODCALLTYPE appGetName(void*, Steinberg_Vst_String128 name) {
  const char* text = "Sonore VST3 Test Host";
  int i = 0;
  for (; text[i] && i < 127; ++i) name[i] = (Steinberg_Vst_TChar) text[i];
  name[i] = 0;
  return Steinberg_kResultOk;
}
static Steinberg_tresult SMTG_STDMETHODCALLTYPE appCreateInstance(void*, Steinberg_TUID,
                                                                  Steinberg_TUID, void** obj) {
  if (obj) *obj = nullptr;
  return Steinberg_kNotImplemented;
}

// ── A component handler, so UI edits have somewhere to land ──────────────────

struct Handler {
  Steinberg_Vst_IComponentHandlerVtbl* lpVtbl = nullptr;
  int begins = 0, edits = 0, ends = 0;
  /** How many times the plugin said its LATENCY moved. A host that is never
   *  told keeps compensating by the old number. */
  int latencyRestarts = 0;
  /** How many times the plugin said the SESSION changed underneath the host.
   *  A host never told closes without asking to save. */
  int dirtyMarks = 0;
  double lastValue = -1.0;
  Steinberg_Vst_ParamID lastId = 0xffffffff;
};

/** The handler's second interface, which is where setDirty lives. Offering it
 *  is optional for a host -- so a plugin has to QUERY, and a test host that
 *  refuses every query proves only that the query happened. */
struct Handler2 {
  Steinberg_Vst_IComponentHandler2Vtbl* lpVtbl = nullptr;
  Handler* owner = nullptr;
};
static Handler2 g_handler2;

/** The host's right-click menu, and the third handler interface that creates
 *  it. A real one arrives already carrying MIDI learn and "remove automation"
 *  -- things only a host can offer, and which disappear if the plugin handles
 *  the right-click itself. Recorded rather than shown: a modal menu inside a
 *  test would never come back. */
struct ContextMenu {
  Steinberg_Vst_IContextMenuVtbl* lpVtbl = nullptr;
  int refs = 1;
};
static ContextMenu g_contextMenu;
static int g_menuCreated = 0, g_menuPopups = 0, g_menuReleases = 0;
static Steinberg_Vst_ParamID g_menuParamId = 0xffffffff;
static int g_menuX = -1, g_menuY = -1;

static Steinberg_tresult SMTG_STDMETHODCALLTYPE cmQuery(void*, const Steinberg_TUID, void** o) {
  if (o) *o = nullptr;
  return Steinberg_kNoInterface;
}
static Steinberg_uint32 SMTG_STDMETHODCALLTYPE cmAddRef(void* self) {
  return (Steinberg_uint32) ++((ContextMenu*) self)->refs;
}
static Steinberg_uint32 SMTG_STDMETHODCALLTYPE cmRelease(void* self) {
  ++g_menuReleases;
  return (Steinberg_uint32) --((ContextMenu*) self)->refs;
}
static Steinberg_int32 SMTG_STDMETHODCALLTYPE cmGetItemCount(void*) { return 0; }
static Steinberg_tresult SMTG_STDMETHODCALLTYPE
cmGetItem(void*, Steinberg_int32, Steinberg_Vst_IContextMenu_Item*,
          struct Steinberg_Vst_IContextMenuTarget**) {
  return Steinberg_kResultFalse;
}
static Steinberg_tresult SMTG_STDMETHODCALLTYPE
cmAddItem(void*, const Steinberg_Vst_IContextMenu_Item*,
          struct Steinberg_Vst_IContextMenuTarget*) {
  return Steinberg_kResultOk;
}
static Steinberg_tresult SMTG_STDMETHODCALLTYPE
cmRemoveItem(void*, const Steinberg_Vst_IContextMenu_Item*,
             struct Steinberg_Vst_IContextMenuTarget*) {
  return Steinberg_kResultOk;
}
static Steinberg_tresult SMTG_STDMETHODCALLTYPE cmPopup(void*, Steinberg_UCoord x,
                                                        Steinberg_UCoord y) {
  ++g_menuPopups;
  g_menuX = (int) x;
  g_menuY = (int) y;
  return Steinberg_kResultOk;
}
static Steinberg_Vst_IContextMenuVtbl g_cmVtbl = {cmQuery,      cmAddRef,     cmRelease,
                                                  cmGetItemCount, cmGetItem,  cmAddItem,
                                                  cmRemoveItem, cmPopup};

struct Handler3 {
  Steinberg_Vst_IComponentHandler3Vtbl* lpVtbl = nullptr;
};
static Handler3 g_handler3;

static Steinberg_tresult SMTG_STDMETHODCALLTYPE h3Query(void*, const Steinberg_TUID, void** o) {
  if (o) *o = nullptr;
  return Steinberg_kNoInterface;
}
static Steinberg_uint32 SMTG_STDMETHODCALLTYPE h3AddRef(void*) { return 1; }
static Steinberg_uint32 SMTG_STDMETHODCALLTYPE h3Release(void*) { return 1; }
static struct Steinberg_Vst_IContextMenu* SMTG_STDMETHODCALLTYPE
h3CreateContextMenu(void*, struct Steinberg_IPlugView* view,
                    const Steinberg_Vst_ParamID* paramId) {
  ++g_menuCreated;
  // A plugin that passes no view is asking for a menu that cannot be placed,
  // and one that passes no parameter is asking about itself rather than a
  // control -- both are worth telling apart from the case this test is for.
  if (!view) return nullptr;
  if (paramId) g_menuParamId = *paramId;
  g_contextMenu.lpVtbl = &g_cmVtbl;
  g_contextMenu.refs = 1;
  return (struct Steinberg_Vst_IContextMenu*) &g_contextMenu;
}
static Steinberg_Vst_IComponentHandler3Vtbl g_h3Vtbl = {h3Query, h3AddRef, h3Release,
                                                        h3CreateContextMenu};

static Steinberg_tresult SMTG_STDMETHODCALLTYPE hQuery(void*, const Steinberg_TUID iid,
                                                       void** o) {
  if (!o) return Steinberg_kInvalidArgument;
  if (std::memcmp(iid, Steinberg_Vst_IComponentHandler2_iid, sizeof(Steinberg_TUID)) == 0) {
    *o = &g_handler2;
    return Steinberg_kResultOk;
  }
  if (std::memcmp(iid, Steinberg_Vst_IComponentHandler3_iid, sizeof(Steinberg_TUID)) == 0) {
    g_handler3.lpVtbl = &g_h3Vtbl;
    *o = &g_handler3;
    return Steinberg_kResultOk;
  }
  *o = nullptr;
  return Steinberg_kNoInterface;
}
static Steinberg_tresult SMTG_STDMETHODCALLTYPE h2Query(void*, const Steinberg_TUID, void** o) {
  if (o) *o = nullptr;
  return Steinberg_kNoInterface;
}
static Steinberg_uint32 SMTG_STDMETHODCALLTYPE h2AddRef(void*) { return 1; }
static Steinberg_uint32 SMTG_STDMETHODCALLTYPE h2Release(void*) { return 1; }
static Steinberg_tresult SMTG_STDMETHODCALLTYPE h2SetDirty(void* self, Steinberg_TBool state) {
  auto* h2 = (Handler2*) self;
  if (state && h2->owner) h2->owner->dirtyMarks++;
  return Steinberg_kResultOk;
}
static Steinberg_tresult SMTG_STDMETHODCALLTYPE h2RequestOpenEditor(void*, Steinberg_FIDString) {
  return Steinberg_kNotImplemented;
}
static Steinberg_tresult SMTG_STDMETHODCALLTYPE h2StartGroupEdit(void*) {
  return Steinberg_kNotImplemented;
}
static Steinberg_tresult SMTG_STDMETHODCALLTYPE h2FinishGroupEdit(void*) {
  return Steinberg_kNotImplemented;
}
static Steinberg_uint32 SMTG_STDMETHODCALLTYPE hAddRef(void*) { return 1; }
static Steinberg_uint32 SMTG_STDMETHODCALLTYPE hRelease(void*) { return 1; }
static Steinberg_tresult SMTG_STDMETHODCALLTYPE hBegin(void* self, Steinberg_Vst_ParamID) {
  ((Handler*) self)->begins++;
  return Steinberg_kResultOk;
}
static Steinberg_tresult SMTG_STDMETHODCALLTYPE hPerform(void* self, Steinberg_Vst_ParamID id,
                                                         Steinberg_Vst_ParamValue v) {
  auto* h = (Handler*) self;
  h->edits++;
  h->lastId = id;
  h->lastValue = v;
  return Steinberg_kResultOk;
}
static Steinberg_tresult SMTG_STDMETHODCALLTYPE hEnd(void* self, Steinberg_Vst_ParamID) {
  ((Handler*) self)->ends++;
  return Steinberg_kResultOk;
}
static Steinberg_tresult SMTG_STDMETHODCALLTYPE hRestart(void* self, Steinberg_int32 flags) {
  if (flags & Steinberg_Vst_RestartFlags_kLatencyChanged) ((Handler*) self)->latencyRestarts++;
  return Steinberg_kResultOk;
}
static Steinberg_Vst_IComponentHandlerVtbl g_hVtbl = {hQuery, hAddRef, hRelease, hBegin,
                                                      hPerform, hEnd,  hRestart};
static Steinberg_Vst_IComponentHandler2Vtbl g_h2Vtbl = {
    h2Query, h2AddRef, h2Release, h2SetDirty, h2RequestOpenEditor, h2StartGroupEdit,
    h2FinishGroupEdit};
static Steinberg_Vst_IHostApplicationVtbl g_appVtbl = {appQuery, appAddRef, appRelease,
                                                       appGetName, appCreateInstance};
static Steinberg_Vst_IAttributeListVtbl g_attrVtbl = {
    attrQuery,     attrAddRef,    attrRelease,   attrSetInt,    attrGetInt,   attrSetFloat,
    attrGetFloat,  attrSetString, attrGetString, attrSetBinary, attrGetBinary};

// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
  // Unbuffered, so a crash under ctest still shows the line it crashed after.
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 2) {
    std::printf("usage: vst3_host_test <path-to.vst3> [--expect-bridge]\n");
    return 2;
  }
  const char* path = argv[1];
  // 0 = no expectation, 1 = native, 2 = web. See the CLAP host test: a
  // fallback that happened silently would otherwise pass.
  int expectEditor = 0;
  bool expectBridge = false;
  bool expectSidechain = false;
  int expectChanMin = 0, expectChanMax = 0;
  int expectAuxOuts = -1;
  bool expectMidiOut = false;
  /** Does this DSP declare supportsMpe? The wrapper offers
   *  INoteExpressionController only when it does, so the test has to be told
   *  which answer is the correct one -- inferring it from the query would make
   *  the check agree with whatever the plugin happened to do. */
  bool expectMpe = false;
  for (int i = 2; i < argc; ++i) {
    if (std::strcmp(argv[i], "--expect-native-editor") == 0) expectEditor = 1;
    if (std::strcmp(argv[i], "--expect-web-editor") == 0) expectEditor = 2;
    if (std::strcmp(argv[i], "--expect-bridge") == 0) expectBridge = true;
    if (std::strcmp(argv[i], "--expect-sidechain") == 0) expectSidechain = true;
    if (std::strcmp(argv[i], "--expect-midi-out") == 0) expectMidiOut = true;
    if (std::strcmp(argv[i], "--expect-mpe") == 0) expectMpe = true;
    if (std::strcmp(argv[i], "--expect-aux-outs") == 0 && i + 1 < argc) {
      expectAuxOuts = std::atoi(argv[i + 1]);
      ++i;
    }
    if (std::strcmp(argv[i], "--expect-channels") == 0 && i + 2 < argc) {
      expectChanMin = std::atoi(argv[i + 1]);
      expectChanMax = std::atoi(argv[i + 2]);
      i += 2;
    }
  }
  std::printf("Sonore VST3 host test\n  plugin: %s\n\n", path);

  LibHandle lib = openLib(path);
  if (!lib) {
    std::printf("  FAIL could not load the module\n");
    return 1;
  }

  // Module entry: a host calls the platform's init symbol before anything else,
  // and a module missing it is simply refused, usually without an explanation.
#if defined(_WIN32)
  auto init = (bool (*)()) symbolOf(lib, "InitDll");
  check(init != nullptr, "the module exports the platform's init symbol (InitDll)");
  if (init) check(init(), "…and it succeeds");
#elif defined(__APPLE__)
  auto init = (bool (*)(void*)) symbolOf(lib, "bundleEntry");
  check(init != nullptr, "the module exports bundleEntry");
  if (init) check(init(nullptr), "…and it succeeds");
#else
  auto init = (bool (*)(void*)) symbolOf(lib, "ModuleEntry");
  check(init != nullptr, "the module exports ModuleEntry");
  if (init) check(init(nullptr), "…and it succeeds");
#endif

  auto getFactory = (Steinberg_IPluginFactory * (*) ()) symbolOf(lib, "GetPluginFactory");
  check(getFactory != nullptr, "the module exports GetPluginFactory");
  if (!getFactory) return 1;

  Steinberg_IPluginFactory* factory = getFactory();
  check(factory != nullptr, "a factory is returned");
  if (!factory) return 1;

  Steinberg_PFactoryInfo finfo{};
  check(factory->lpVtbl->getFactoryInfo(factory, &finfo) == Steinberg_kResultOk,
        "the factory reports its info");
  check(finfo.vendor[0] != '\0', "…including a vendor");

  check(factory->lpVtbl->countClasses(factory) == 1, "the factory advertises one class");

  Steinberg_PClassInfo cinfo{};
  check(factory->lpVtbl->getClassInfo(factory, 0, &cinfo) == Steinberg_kResultOk,
        "the class info reads back");
  check(std::strcmp(cinfo.category, "Audio Module Class") == 0,
        "…with the audio-module category hosts scan for");
  check(cinfo.name[0] != '\0', "…and a name");

  // IPluginFactory2 carries the subcategory that decides which folder a host
  // files the plugin under: an instrument that says "Fx" is lost forever.
  Steinberg_IPluginFactory2* factory2 = nullptr;
  if (factory->lpVtbl->queryInterface(factory, Steinberg_IPluginFactory2_iid,
                                      (void**) &factory2) == Steinberg_kResultOk &&
      factory2) {
    Steinberg_PClassInfo2 c2{};
    check(factory2->lpVtbl->getClassInfo2(factory2, 0, &c2) == Steinberg_kResultOk,
          "IPluginFactory2 class info reads back");
    const bool instrument = std::strstr(c2.subCategories, "Instrument") != nullptr;
    const bool fx = std::strstr(c2.subCategories, "Fx") != nullptr;
    check(instrument || fx, "…declaring a subcategory (Fx or Instrument)");
    std::printf("  ---- graded as %s ----\n", instrument ? "an INSTRUMENT" : "an EFFECT");
    check(c2.vendor[0] != '\0' && c2.version[0] != '\0', "…with vendor and version");
    factory2->lpVtbl->release(factory2);
  }

  // ── Create ────────────────────────────────────────────────────────────────
  Steinberg_Vst_IComponent* component = nullptr;
  check(factory->lpVtbl->createInstance(factory, (Steinberg_FIDString) cinfo.cid,
                                        (Steinberg_FIDString) Steinberg_Vst_IComponent_iid,
                                        (void**) &component) == Steinberg_kResultOk &&
            component,
        "the factory creates a component");
  if (!component) return 1;

  Steinberg_TUID bogus = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  void* nothing = nullptr;
  check(factory->lpVtbl->createInstance(factory, (Steinberg_FIDString) bogus,
                                        (Steinberg_FIDString) Steinberg_Vst_IComponent_iid,
                                        &nothing) != Steinberg_kResultOk,
        "an unknown class id is refused");

  g_hostApp.lpVtbl = &g_appVtbl;
  check(component->lpVtbl->initialize(component, (Steinberg_FUnknown*) &g_hostApp) ==
            Steinberg_kResultOk,
        "the component initialises");

  Steinberg_Vst_IAudioProcessor* processor = nullptr;
  check(component->lpVtbl->queryInterface(component, Steinberg_Vst_IAudioProcessor_iid,
                                          (void**) &processor) == Steinberg_kResultOk &&
            processor,
        "IAudioProcessor is reachable from the component");

  Steinberg_Vst_IEditController* controller = nullptr;
  check(component->lpVtbl->queryInterface(component, Steinberg_Vst_IEditController_iid,
                                          (void**) &controller) == Steinberg_kResultOk &&
            controller,
        "IEditController is reachable too (single-component effect)");
  if (!processor || !controller) return 1;

  Steinberg_TUID controllerCid{};
  component->lpVtbl->getControllerClassId(component, controllerCid);
  check(std::memcmp(controllerCid, cinfo.cid, sizeof(Steinberg_TUID)) == 0,
        "…and it reports the same class id, as a single component must");

  // ── Buses ─────────────────────────────────────────────────────────────────
  const Steinberg_int32 numIn =
      component->lpVtbl->getBusCount(component, Steinberg_Vst_MediaTypes_kAudio,
                                     Steinberg_Vst_BusDirections_kInput);
  const Steinberg_int32 numOut =
      component->lpVtbl->getBusCount(component, Steinberg_Vst_MediaTypes_kAudio,
                                     Steinberg_Vst_BusDirections_kOutput);
  const Steinberg_int32 numEvent =
      component->lpVtbl->getBusCount(component, Steinberg_Vst_MediaTypes_kEvent,
                                     Steinberg_Vst_BusDirections_kInput);
  const bool isInstrument = numEvent > 0;
  if (expectMidiOut) {
    const Steinberg_int32 evOut = component->lpVtbl->getBusCount(
        component, Steinberg_Vst_MediaTypes_kEvent, Steinberg_Vst_BusDirections_kOutput);
    check(evOut == 1, "a MIDI-emitting plugin declares an event OUTPUT bus");
    Steinberg_Vst_BusInfo ei{};
    check(component->lpVtbl->getBusInfo(component, Steinberg_Vst_MediaTypes_kEvent,
                                        Steinberg_Vst_BusDirections_kOutput, 0,
                                        &ei) == Steinberg_kResultOk,
          "…and it reports its info");
  }

  if (expectAuxOuts >= 0) {
    check(numOut == 1 + expectAuxOuts, "the plugin declares its aux output buses");
    for (int b = 0; b < expectAuxOuts; ++b) {
      Steinberg_Vst_BusInfo ai{};
      check(component->lpVtbl->getBusInfo(component, Steinberg_Vst_MediaTypes_kAudio,
                                          Steinberg_Vst_BusDirections_kOutput,
                                          (Steinberg_int32) (1 + b),
                                          &ai) == Steinberg_kResultOk,
            "each aux output bus reports its info");
      check(ai.busType == Steinberg_Vst_BusTypes_kAux, "…and it is an AUX bus");
    }
  } else {
    check(numOut == 1, "there is exactly one audio output bus");
  }
  if (isInstrument) check(numIn == 0, "an instrument declares no audio input bus");
  else if (expectSidechain) check(numIn == 2, "a sidechain effect declares TWO input buses");
  else check(numIn == 1, "an effect declares an audio input bus");

  if (expectSidechain) {
    Steinberg_Vst_BusInfo scInfo{};
    check(component->lpVtbl->getBusInfo(component, Steinberg_Vst_MediaTypes_kAudio,
                                        Steinberg_Vst_BusDirections_kInput, 1,
                                        &scInfo) == Steinberg_kResultOk,
          "the sidechain bus reports its info");
    check(scInfo.busType == Steinberg_Vst_BusTypes_kAux, "…and it is an AUX bus");
    check(scInfo.channelCount == 2, "…stereo");
  }

  Steinberg_Vst_BusInfo binfo{};
  check(component->lpVtbl->getBusInfo(component, Steinberg_Vst_MediaTypes_kAudio,
                                      Steinberg_Vst_BusDirections_kOutput, 0,
                                      &binfo) == Steinberg_kResultOk,
        "the output bus reports its info");
  check(binfo.channelCount == 2, "…and it is stereo");

  Steinberg_Vst_SpeakerArrangement stereo = Steinberg_Vst_kSpeakerL | Steinberg_Vst_kSpeakerR;
  Steinberg_Vst_SpeakerArrangement stereoPair[2] = {stereo, stereo};
  const Steinberg_int32 wantIns = isInstrument ? 0 : (expectSidechain ? 2 : 1);
  const Steinberg_int32 wantOuts = expectAuxOuts > 0 ? 1 + expectAuxOuts : 1;
  Steinberg_Vst_SpeakerArrangement outArrs[8];
  for (int i = 0; i < 8; ++i) outArrs[i] = stereo;
  check(processor->lpVtbl->setBusArrangements(processor, isInstrument ? nullptr : stereoPair,
                                              wantIns, outArrs,
                                              wantOuts) == Steinberg_kResultOk,
        "a stereo arrangement is accepted");
  Steinberg_Vst_SpeakerArrangement mono = Steinberg_Vst_kSpeakerL;
  Steinberg_Vst_SpeakerArrangement monoPair[2] = {mono, mono};
  // As many OUTPUT arrangements as the plugin has buses: main mono, every aux
  // at its declared (stereo) width. The first version passed `&mono` with
  // numOuts = 3 for the splitter, and the wrapper -- correctly, per the
  // contract -- read outputs[1] and outputs[2] off the end of a one-element
  // stack variable. ASan found it; the test had lied about its own array.
  Steinberg_Vst_SpeakerArrangement monoOuts[8];
  monoOuts[0] = mono;
  for (int i = 1; i < 8; ++i) monoOuts[i] = stereo;
  if (expectChanMin > 0 && expectChanMin <= 1) {
    check(processor->lpVtbl->setBusArrangements(processor, isInstrument ? nullptr : monoPair,
                                                wantIns, monoOuts,
                                                wantOuts) == Steinberg_kResultOk,
          "a channel-flexible plugin accepts mono");
    Steinberg_Vst_BusInfo mi{};
    component->lpVtbl->getBusInfo(component, Steinberg_Vst_MediaTypes_kAudio,
                                  Steinberg_Vst_BusDirections_kOutput, 0, &mi);
    check(mi.channelCount == 1, "…and the bus now reports one channel");
    // Back to stereo for the rest of the run.
    check(processor->lpVtbl->setBusArrangements(processor, isInstrument ? nullptr : stereoPair,
                                                wantIns, outArrs,
                                                wantOuts) == Steinberg_kResultOk,
          "…and back to stereo");
  } else {
    check(processor->lpVtbl->setBusArrangements(processor, isInstrument ? nullptr : monoPair,
                                                wantIns, monoOuts,
                                                wantOuts) != Steinberg_kResultOk,
          "a mono arrangement is refused rather than silently mis-handled");
  }

  if (expectChanMax >= 8) {
    Steinberg_Vst_SpeakerArrangement wide = 0;
    for (int b = 0; b < 8; ++b) wide |= (Steinberg_Vst_SpeakerArrangement) 1 << b;
    Steinberg_Vst_SpeakerArrangement widePair[2] = {wide, wide};
    check(processor->lpVtbl->setBusArrangements(processor, isInstrument ? nullptr : widePair,
                                                wantIns, &wide,
                                                wantOuts) == Steinberg_kResultOk,
          "an 8-channel (7.1) arrangement is accepted");
    Steinberg_Vst_SpeakerArrangement got = 0;
    processor->lpVtbl->getBusArrangement(processor, Steinberg_Vst_BusDirections_kOutput, 0,
                                         &got);
    check(got == wide, "…and echoes back exactly the mask the host proposed");
    check(processor->lpVtbl->setBusArrangements(processor, isInstrument ? nullptr : stereoPair,
                                                wantIns, outArrs,
                                                wantOuts) == Steinberg_kResultOk,
          "…and back to stereo again");
  }

  check(processor->lpVtbl->canProcessSampleSize(
            processor, Steinberg_Vst_SymbolicSampleSizes_kSample32) == Steinberg_kResultTrue,
        "32-bit processing is supported");
  // 64-bit is a CLAIM about the DSP, not about the wrapper: a plugin either
  // genuinely processes doubles or must decline them rather than downcasting
  // behind the host's back. Both answers are correct; a plugin that accepts
  // 64-bit and then loses precision would be the wrong one, and the CLAP host
  // test measures exactly that.
  const bool accepts64 =
      processor->lpVtbl->canProcessSampleSize(
          processor, Steinberg_Vst_SymbolicSampleSizes_kSample64) == Steinberg_kResultTrue;
  std::printf("  ---- 64-bit processing: %s ----\n", accepts64 ? "offered" : "declined");

  // ── Parameters ────────────────────────────────────────────────────────────
  const Steinberg_int32 numParams = controller->lpVtbl->getParameterCount(controller);
  check(numParams > 0, "the controller declares parameters");

  bool infoOk = true, normalisedOk = true, textOk = true;
  for (Steinberg_int32 i = 0; i < numParams; ++i) {
    Steinberg_Vst_ParameterInfo pinfo{};
    if (controller->lpVtbl->getParameterInfo(controller, i, &pinfo) != Steinberg_kResultOk) {
      infoOk = false;
      break;
    }
    if (pinfo.title[0] == 0) infoOk = false;
    if (pinfo.defaultNormalizedValue < 0.0 || pinfo.defaultNormalizedValue > 1.0) infoOk = false;

    // The conversion that silently ruins a plugin: normalised 0..1 must survive
    // a round trip through plain values.
    for (double n : {0.0, 0.25, 0.5, 0.75, 1.0}) {
      const double plain = controller->lpVtbl->normalizedParamToPlain(controller, pinfo.id, n);
      const double back = controller->lpVtbl->plainParamToNormalized(controller, pinfo.id, plain);
      // A stepped control quantises, so only continuous ones round-trip exactly.
      if (pinfo.stepCount == 0 && std::fabs(back - n) > 1e-6) normalisedOk = false;
      if (back < -1e-9 || back > 1.0 + 1e-9) normalisedOk = false;
    }

    Steinberg_Vst_String128 text{};
    if (controller->lpVtbl->getParamStringByValue(controller, pinfo.id, 0.5, text) !=
            Steinberg_kResultOk ||
        text[0] == 0)
      textOk = false;
  }
  check(infoOk, "every parameter reports valid, named info");

  // Parameter groups become VST3 UNITS. A plugin that declares none must not
  // offer IUnitInfo at all; one that does must describe a consistent tree,
  // because a parameter pointing at a missing unit is how a host's editor
  // ends up with orphaned controls.
  {
    Steinberg_Vst_IUnitInfo* unitInfo = nullptr;
    controller->lpVtbl->queryInterface(controller, Steinberg_Vst_IUnitInfo_iid,
                                       (void**) &unitInfo);
    if (unitInfo) {
      const Steinberg_int32 nUnits = unitInfo->lpVtbl->getUnitCount(unitInfo);
      // At LEAST a root. More than one means groups; exactly one means the
      // interface is here to carry a program list instead, which is a plugin
      // with presets and no parameter groups -- the saturator, as it happens.
      // The old check demanded more than one and started failing the moment
      // factory presets became reachable in this format.
      check(nUnits >= 1, "a plugin offering IUnitInfo declares at least a root unit");
      const Steinberg_int32 programLists = unitInfo->lpVtbl->getProgramListCount(unitInfo);
      check(nUnits > 1 || programLists > 0,
            "…and offers the interface for a reason: groups, programs, or both");
      bool treeOk = true;
      for (Steinberg_int32 u = 0; u < nUnits; ++u) {
        Steinberg_Vst_UnitInfo ui{};
        if (unitInfo->lpVtbl->getUnitInfo(unitInfo, u, &ui) != Steinberg_kResultOk) {
          treeOk = false;
          break;
        }
        if (ui.name[0] == 0) treeOk = false;
        if (u == 0 && ui.id != 0) treeOk = false;              // root is id 0
        if (u > 0 && ui.parentUnitId != 0) treeOk = false;      // children hang off it
      }
      check(treeOk, "…each unit named, rooted and parented");

      bool unitsResolve = true;
      for (Steinberg_int32 i = 0; i < numParams; ++i) {
        Steinberg_Vst_ParameterInfo pi{};
        if (controller->lpVtbl->getParameterInfo(controller, i, &pi) != Steinberg_kResultOk)
          continue;
        if (pi.unitId < 0 || pi.unitId >= nUnits) unitsResolve = false;
      }
      check(unitsResolve, "…and every parameter points at a unit that exists");
      unitInfo->lpVtbl->release(unitInfo);
    } else {
      std::printf("  ---- no parameter groups declared ----\n");
    }
  }
  check(normalisedOk, "normalised values round-trip through plain and back");
  check(textOk, "every parameter renders a displayable string");

  check(controller->lpVtbl->getParameterInfo(controller, numParams, nullptr) !=
            Steinberg_kResultOk,
        "an out-of-range parameter index is refused");

  // Setting through the controller must be readable back at the same value.
  check(controller->lpVtbl->setParamNormalized(controller, 0, 0.25) == Steinberg_kResultOk,
        "a normalised value can be set");
  checkNear(controller->lpVtbl->getParamNormalized(controller, 0), 0.25, 1e-6,
            "…and reads back unchanged");

  Handler handler;
  handler.lpVtbl = &g_hVtbl;
  g_handler2.lpVtbl = &g_h2Vtbl;
  g_handler2.owner = &handler;
  check(controller->lpVtbl->setComponentHandler(
            controller, (Steinberg_Vst_IComponentHandler*) &handler) == Steinberg_kResultOk,
        "the component handler is accepted");

  // ── Activate and process ──────────────────────────────────────────────────
  const Steinberg_int32 blockSize = 128;
  Steinberg_Vst_ProcessSetup setup{};
  setup.processMode = 0;
  setup.symbolicSampleSize = Steinberg_Vst_SymbolicSampleSizes_kSample32;
  setup.maxSamplesPerBlock = blockSize;
  setup.sampleRate = 48000.0;
  check(processor->lpVtbl->setupProcessing(processor, &setup) == Steinberg_kResultOk,
        "processing is set up");
  check(component->lpVtbl->setActive(component, 1) == Steinberg_kResultOk,
        "the component activates");
  processor->lpVtbl->setProcessing(processor, 1);

  std::vector<float> inL(blockSize), inR(blockSize), outL(blockSize), outR(blockSize);
  float* inPtrs[2] = {inL.data(), inR.data()};
  float* outPtrs[2] = {outL.data(), outR.data()};

  std::vector<float> scL(blockSize, 0.0f), scR(blockSize, 0.0f);
  float* scPtrs[2] = {scL.data(), scR.data()};

  Steinberg_Vst_AudioBusBuffers inBuses[2] = {};
  Steinberg_Vst_AudioBusBuffers& inBus = inBuses[0];
  inBus.numChannels = 2;
  inBus.Steinberg_Vst_AudioBusBuffers_channelBuffers32 = inPtrs;
  inBuses[1].numChannels = 2;
  inBuses[1].Steinberg_Vst_AudioBusBuffers_channelBuffers32 = scPtrs;
  constexpr int kMaxTestAux = 4;
  std::vector<std::vector<float>> auxCh((size_t) (kMaxTestAux * 2));
  std::vector<std::array<float*, 2>> auxPtrs((size_t) kMaxTestAux);
  Steinberg_Vst_AudioBusBuffers outBuses[1 + kMaxTestAux] = {};
  for (int b = 0; b < kMaxTestAux; ++b) {
    auxCh[(size_t) (b * 2)].assign((size_t) blockSize, 0.0f);
    auxCh[(size_t) (b * 2 + 1)].assign((size_t) blockSize, 0.0f);
    auxPtrs[(size_t) b] = {auxCh[(size_t) (b * 2)].data(), auxCh[(size_t) (b * 2 + 1)].data()};
    outBuses[1 + b].numChannels = 2;
    outBuses[1 + b].Steinberg_Vst_AudioBusBuffers_channelBuffers32 =
        auxPtrs[(size_t) b].data();
  }
  Steinberg_Vst_AudioBusBuffers& outBus = outBuses[0];
  outBus.numChannels = 2;
  outBus.Steinberg_Vst_AudioBusBuffers_channelBuffers32 = outPtrs;

  std::vector<ValueQueue> queues;
  ParamChanges changes;
  changes.lpVtbl = &g_pcVtbl;
  changes.queues = &queues;

  std::vector<Steinberg_Vst_Event> events;
  EventList eventList;
  eventList.lpVtbl = &g_elVtbl;
  eventList.events = &events;

  Steinberg_Vst_ProcessData data{};
  data.processMode = 0;
  data.symbolicSampleSize = Steinberg_Vst_SymbolicSampleSizes_kSample32;
  data.numSamples = blockSize;
  data.numInputs = isInstrument ? 0 : (expectSidechain ? 2 : 1);
  data.numOutputs = expectAuxOuts > 0 ? 1 + expectAuxOuts : 1;
  data.inputs = isInstrument ? nullptr : inBuses;
  data.outputs = outBuses;
  data.inputParameterChanges = (Steinberg_Vst_IParameterChanges*) &changes;
  data.inputEvents = (Steinberg_Vst_IEventList*) &eventList;
  std::vector<Steinberg_Vst_Event> outEventStore;
  EventList outEventList;
  outEventList.lpVtbl = &g_elVtbl;
  outEventList.events = &outEventStore;
  data.outputEvents = (Steinberg_Vst_IEventList*) &outEventList;

  struct Result {
    double peak = 0.0, energy = 0.0;
    bool finite = true;
  };
  auto run = [&](int blocks, bool silent) -> Result {
    Result r;
    g_emitted.clear();
    static int phase = 0;
    for (int b = 0; b < blocks; ++b) {
      for (Steinberg_int32 i = 0; i < blockSize; ++i) {
        const float s = silent ? 0.0f
                               : 0.25f * (float) std::sin(2.0 * 3.14159265358979 * 1000.0 * phase /
                                                          48000.0);
        inL[i] = s;
        inR[i] = s;
        ++phase;
      }
      processor->lpVtbl->process(processor, &data);
      queues.clear();
      events.clear();
      for (Steinberg_int32 i = 0; i < blockSize; ++i) {
        if (!std::isfinite(outL[i]) || !std::isfinite(outR[i])) r.finite = false;
        const double a = std::fabs(outL[i]);
        if (a > r.peak) r.peak = a;
        r.energy += (double) outL[i] * outL[i];
      }
    }
    return r;
  };

  const int oneSecond = 48000 / blockSize;

  if (isInstrument) {
    const Result rest = run(oneSecond / 4, true);
    check(rest.finite, "an idle instrument stays finite");
    check(rest.peak < 1e-4, "an instrument is SILENT at rest");

    Steinberg_Vst_Event noteOn{};
    noteOn.type = Steinberg_Vst_Event_EventTypes_kNoteOnEvent;
    noteOn.sampleOffset = 0;
    noteOn.Steinberg_Vst_Event_noteOn.channel = 0;
    noteOn.Steinberg_Vst_Event_noteOn.pitch = 69; // A4
    noteOn.Steinberg_Vst_Event_noteOn.velocity = 1.0f;
    noteOn.Steinberg_Vst_Event_noteOn.noteId = -1;
    events.push_back(noteOn);

    const Result playing = run(oneSecond / 2, true);
    check(playing.finite, "a held note stays finite");
    if (expectMidiOut) {
      // A note effect is silent on purpose; what it produces is events.
      check(playing.energy == 0.0, "a note effect stays audibly silent");
      check(!g_emitted.empty(), "…and a held note makes it EMIT events");
      check(countEventType(Steinberg_Vst_Event_EventTypes_kNoteOnEvent) > 1,
            "…several note-ons, one per arpeggiator step");
      check(countEventType(Steinberg_Vst_Event_EventTypes_kNoteOffEvent) > 0,
            "…each of them released again");
      bool sane = true;
      for (const auto& e : g_emitted) {
        const auto& on = e.Steinberg_Vst_Event_noteOn;
        if (e.type == Steinberg_Vst_Event_EventTypes_kNoteOnEvent &&
            (on.pitch < 0 || on.pitch > 127 || on.velocity < 0.0f || on.velocity > 1.0f))
          sane = false;
        if (e.sampleOffset < 0 || e.sampleOffset >= blockSize) sane = false;
      }
      check(sane, "…with pitches, velocities and offsets a host can act on");

      // ── SysEx, in and out ────────────────────────────────────────────────
      //
      // VST3 has no SysEx event: it is a generic kDataEvent whose type field
      // says what the bytes are. Both directions are exercised here because
      // CLAP and LV2 already were, and "one format has it, the others do not"
      // is the defect this project has found more often than any other.
      //
      // TWO messages, the first seven bytes long. That length is not
      // decoration -- the equivalent LV2 test passed twice against
      // deliberately broken framing because the only message it sent was 32
      // bytes, which is already a multiple of the alignment it was supposed
      // to be testing.
      {
        const std::vector<uint8_t> odd = {0xF0, 0x7D, 0x11, 0x22, 0x33, 0x44, 0xF7};
        sonore::midici::DeviceIdentity identity;
        identity.muid = 0x0ABCDEF;
        identity.manufacturer[0] = 0x7D;
        const std::vector<uint8_t> discovery = sonore::midici::encodeDiscovery(identity);

        g_emitted.clear();
        g_emittedSysex.clear();
        events.clear();
        Steinberg_Vst_Event sx{};
        sx.type = Steinberg_Vst_Event_EventTypes_kDataEvent;
        sx.sampleOffset = 0;
        sx.Steinberg_Vst_Event_data.type = Steinberg_Vst_DataEvent_DataTypes_kMidiSysEx;
        sx.Steinberg_Vst_Event_data.size = (Steinberg_uint32) odd.size();
        sx.Steinberg_Vst_Event_data.bytes = odd.data();
        events.push_back(sx);
        sx.sampleOffset = 1;
        sx.Steinberg_Vst_Event_data.size = (Steinberg_uint32) discovery.size();
        sx.Steinberg_Vst_Event_data.bytes = discovery.data();
        events.push_back(sx);

        run(4, true);

        bool oddBack = false, discoveryBack = false, parses = false;
        for (const auto& got : g_emittedSysex) {
          if (got.size() == odd.size() && std::equal(got.begin(), got.end(), odd.begin()))
            oddBack = true;
          if (got.size() == discovery.size() &&
              std::equal(got.begin(), got.end(), discovery.begin())) {
            discoveryBack = true;
            sonore::midici::Message decoded;
            if (sonore::midici::decode(got.data(), got.size(), &decoded) &&
                decoded.type == sonore::midici::MessageType::Discovery)
              parses = true;
          }
        }
        check(oddBack, "…a seven-byte SysEx goes in as a kDataEvent and comes back out");
        check(discoveryBack, "…and so does a MIDI-CI discovery message behind it");
        check(parses, "…which still decodes as a Discovery, framing intact");
        events.clear();
        g_emittedSysex.clear();
      }
    } else {
      check(playing.energy > 0.0, "a VST3 note-on makes the instrument sound");
    }
    check(playing.peak < 10.0, "…without blowing up");

    Steinberg_Vst_Event noteOff{};
    noteOff.type = Steinberg_Vst_Event_EventTypes_kNoteOffEvent;
    noteOff.Steinberg_Vst_Event_noteOff.channel = 0;
    noteOff.Steinberg_Vst_Event_noteOff.pitch = 69;
    events.push_back(noteOff);
    run(oneSecond * 3, true);
    const Result released = run(oneSecond / 4, true);
    check(released.peak < 1e-3, "note-off releases back to silence");
  } else {
    const Result r = run(oneSecond, false);
    check(r.finite, "a second of audio stays finite (no NaN/Inf)");
    check(r.energy > 0.0, "the plugin produces sound, not silence");
    check(r.peak < 10.0, "the output does not blow up");
  }

  // Automation through the parameter queue, in NORMALISED units.
  {
    ValueQueue q;
    q.lpVtbl = &g_vqVtbl;
    q.id = 0;
    q.value = 1.0; // maximum
    queues.push_back(q);
    run(1, true);
    checkNear(controller->lpVtbl->getParamNormalized(controller, 0), 1.0, 1e-6,
              "automation at 1.0 lands as the parameter's maximum");

    queues.clear();
    ValueQueue q2 = q;
    q2.value = 0.0;
    queues.push_back(q2);
    run(1, true);
    checkNear(controller->lpVtbl->getParamNormalized(controller, 0), 0.0, 1e-6,
              "…and 0.0 lands as its minimum");
    queues.clear();
  }

  // ── Sidechain ─────────────────────────────────────────────────────────────
  if (expectSidechain) {
    // The automation test above parked parameter 0 (Duck) at its MINIMUM;
    // a ducker with 0% duck ignores any key. Full depth for the proof.
    ValueQueue depth;
    depth.lpVtbl = &g_vqVtbl;
    depth.id = 0;
    depth.value = 1.0;
    queues.push_back(depth);
    std::fill(scL.begin(), scL.end(), 0.0f);
    std::fill(scR.begin(), scR.end(), 0.0f);
    run(20, false);
    const Result quietKey = run(20, false);
    for (Steinberg_int32 i = 0; i < blockSize; ++i) scL[i] = scR[i] = 0.9f;
    run(20, false);
    const Result hotKey = run(20, false);
    std::fill(scL.begin(), scL.end(), 0.0f);
    std::fill(scR.begin(), scR.end(), 0.0f);
    check(quietKey.energy > 0.0, "with a silent key the main signal passes");
    check(hotKey.energy < quietKey.energy * 0.5,
          "a hot key ducks the main signal (the sidechain reaches the DSP)");
  }

  if (accepts64) {
    // Then it must survive being driven that way.
    Steinberg_Vst_ProcessData d64 = data;
    d64.symbolicSampleSize = Steinberg_Vst_SymbolicSampleSizes_kSample64;
    std::vector<double> dinL((size_t) blockSize, 0.25), dinR((size_t) blockSize, 0.25);
    std::vector<double> doutL((size_t) blockSize, -1.0), doutR((size_t) blockSize, -1.0);
    double* dinP[2] = {dinL.data(), dinR.data()};
    double* doutP[2] = {doutL.data(), doutR.data()};
    Steinberg_Vst_AudioBusBuffers din{}, dout{};
    din.numChannels = 2;
    din.Steinberg_Vst_AudioBusBuffers_channelBuffers64 = dinP;
    dout.numChannels = 2;
    dout.Steinberg_Vst_AudioBusBuffers_channelBuffers64 = doutP;
    d64.numInputs = isInstrument ? 0 : 1;
    d64.numOutputs = 1;
    d64.inputs = isInstrument ? nullptr : &din;
    d64.outputs = &dout;
    check(processor->lpVtbl->process(processor, &d64) == Steinberg_kResultOk,
          "…and a 64-bit block really processes");
    bool finite64 = true;
    for (Steinberg_int32 i = 0; i < blockSize; ++i)
      if (!std::isfinite(doutL[(size_t) i])) finite64 = false;
    check(finite64, "…producing finite doubles");
  }

  // ── Aux output buses ──────────────────────────────────────────────────────
  // Same physics proof as the CLAP host: low tone to the main bus, high tone
  // to the last aux bus, each measured on the buffers the host owns.
  if (expectAuxOuts > 0) {
    auto energies = [&](double hz) {
      static int ph = 0;
      double mainE = 0.0, lastAuxE = 0.0;
      for (int b = 0; b < 30; ++b) {
        for (Steinberg_int32 i = 0; i < blockSize; ++i) {
          const float v =
              0.5f * (float) std::sin(2.0 * 3.14159265358979 * hz * ph / 48000.0);
          inL[i] = inR[i] = v;
          ++ph;
        }
        processor->lpVtbl->process(processor, &data);
        queues.clear();
        events.clear();
        if (b >= 15) {
          for (Steinberg_int32 i = 0; i < blockSize; ++i) {
            mainE += (double) outL[i] * outL[i];
            const float a = auxCh[(size_t) ((expectAuxOuts - 1) * 2)][(size_t) i];
            lastAuxE += (double) a * a;
          }
        }
      }
      return std::pair<double, double>(mainE, lastAuxE);
    };

    const auto low = energies(60.0);
    check(low.first > 1e-3, "a low tone reaches the MAIN bus");
    check(low.second < low.first * 0.01, "…and stays off the high aux bus");

    const auto high = energies(12000.0);
    check(high.second > 1e-3, "a high tone reaches the AUX bus");
    check(high.first < high.second * 0.01, "…and stays off the main bus");
  }

  // ── The rates and block sizes a host really uses ──────────────────────────
  //
  // Everything above ran at 48 kHz with 128-sample blocks. The CLAP host test
  // sweeps the rates that break filters designed around that number -- 8 kHz,
  // where Nyquist is below a tone control's top end, and 192 kHz, where every
  // time expressed in samples is four times longer -- and this wrapper has its
  // own buffer plumbing, so passing there says nothing about passing here.
  //
  // Block sizes are capped at the buffers this file allocated. A 512-sample
  // block against a 128-sample vector is a heap overflow, which is what the
  // first version of the CLAP sweep did.
  {
    const double rates[] = {8000.0, 22050.0, 96000.0, 192000.0};
    const Steinberg_int32 blocks[] = {1, 7, blockSize};
    bool allFinite = true, allSane = true, allSetup = true;
    double worst = 0.0;

    for (double rate : rates) {
      component->lpVtbl->setActive(component, 0);
      Steinberg_Vst_ProcessSetup rateSetup{};
      rateSetup.processMode = Steinberg_Vst_ProcessModes_kRealtime;
      rateSetup.symbolicSampleSize = Steinberg_Vst_SymbolicSampleSizes_kSample32;
      rateSetup.maxSamplesPerBlock = blockSize;
      rateSetup.sampleRate = rate;
      if (processor->lpVtbl->setupProcessing(processor, &rateSetup) != Steinberg_kResultOk) {
        allSetup = false;
        continue;
      }
      component->lpVtbl->setActive(component, 1);
      processor->lpVtbl->setProcessing(processor, 1);

      for (Steinberg_int32 frames : blocks) {
        data.numSamples = frames;
        queues.clear();
        events.clear();
        if (isInstrument) {
          Steinberg_Vst_Event on{};
          on.type = Steinberg_Vst_Event_EventTypes_kNoteOnEvent;
          on.Steinberg_Vst_Event_noteOn.channel = 0;
          on.Steinberg_Vst_Event_noteOn.pitch = 60;
          on.Steinberg_Vst_Event_noteOn.velocity = 1.0f;
          on.Steinberg_Vst_Event_noteOn.noteId = -1;
          events.push_back(on);
        }
        unsigned lcg = 99u;
        for (int b = 0; b < 40; ++b) {
          for (Steinberg_int32 i = 0; i < frames; ++i) {
            lcg = lcg * 1664525u + 1013904223u;
            inL[(size_t) i] = inR[(size_t) i] =
                (float) ((int) (lcg >> 8) % 20001 - 10000) / 40000.0f;
          }
          processor->lpVtbl->process(processor, &data);
          queues.clear();
          events.clear();
          for (Steinberg_int32 i = 0; i < frames; ++i) {
            if (!std::isfinite(outL[(size_t) i]) || !std::isfinite(outR[(size_t) i]))
              allFinite = false;
            const double a = std::fabs((double) outL[(size_t) i]);
            if (a > worst) worst = a;
            // Not merely finite: a huge but representable value reaches the
            // user as a burst loud enough to damage monitors, where a NaN
            // would at least be muted.
            if (a > 100.0) allSane = false;
          }
        }
      }
      processor->lpVtbl->setProcessing(processor, 0);
      component->lpVtbl->setActive(component, 0);
    }
    data.numSamples = blockSize;

    std::printf("  ---- 8 k to 192 kHz, blocks of 1, 7 and %d: worst |out| %.3g ----\n",
                (int) blockSize, worst);
    check(allSetup, "the plugin accepts every rate a host might set up");
    check(allFinite, "…and produces finite audio at all of them");
    check(allSane, "…that never blows past sanity, whatever the rate");

    // Put it back the way the sections below expect to find it.
    Steinberg_Vst_ProcessSetup restore{};
    restore.processMode = Steinberg_Vst_ProcessModes_kRealtime;
    restore.symbolicSampleSize = Steinberg_Vst_SymbolicSampleSizes_kSample32;
    restore.maxSamplesPerBlock = blockSize;
    restore.sampleRate = 48000.0;
    processor->lpVtbl->setupProcessing(processor, &restore);
    component->lpVtbl->setActive(component, 1);
    processor->lpVtbl->setProcessing(processor, 1);
  }

  // ── Factory presets, which VST3 calls programs ────────────────────────────
  //
  // A plugin's presets appeared in CLAP through preset discovery and in LV2 as
  // pset:Preset, and in VST3 as nothing at all: the IUnitInfo program half was
  // a row of stubs returning "none". A saturator shipping three presets
  // offered none of them in Cubase or Reaper.
  //
  // Two halves have to line up. The LIST is metadata a host reads to build a
  // menu; the PARAMETER flagged kIsProgramChange is how it actually selects
  // one. Either without the other is a menu that does nothing or a control
  // with no names on it.
  {
    Steinberg_Vst_IUnitInfo* units = nullptr;
    const bool haveUnits = component->lpVtbl->queryInterface(component, Steinberg_Vst_IUnitInfo_iid,
                                                             (void**) &units) ==
                               Steinberg_kResultOk &&
                           units != nullptr;
    if (haveUnits && units) {
      const Steinberg_int32 lists = units->lpVtbl->getProgramListCount(units);
      if (lists > 0) {
        Steinberg_Vst_ProgramListInfo listInfo{};
        check(units->lpVtbl->getProgramListInfo(units, 0, &listInfo) == Steinberg_kResultOk,
              "a plugin with factory presets publishes a program list");
        check(listInfo.programCount > 0, "…with programs in it");

        // Every program must have a name a user can read, and they must be
        // the plugin's own rather than "Program 1".
        bool named = true;
        std::string firstName;
        for (Steinberg_int32 i = 0; i < listInfo.programCount; ++i) {
          Steinberg_Vst_String128 name{};
          if (units->lpVtbl->getProgramName(units, listInfo.id, i, name) != Steinberg_kResultOk) {
            named = false;
            break;
          }
          std::string narrow;
          for (int k = 0; k < 128 && name[k]; ++k) narrow.push_back((char) name[k]);
          if (narrow.empty()) named = false;
          if (i == 0) firstName = narrow;
        }
        check(named, "…each with a name of its own");
        std::printf("  ---- %d factory programs, first is \"%s\" ----\n",
                    (int) listInfo.programCount, firstName.c_str());

        // The root unit has to POINT at the list, which is where a host looks.
        Steinberg_Vst_UnitInfo root{};
        check(units->lpVtbl->getUnitInfo(units, 0, &root) == Steinberg_kResultOk,
              "the root unit is describable");
        check(root.programListId == listInfo.id,
              "…and owns the list, which is where a host goes looking for it");

        // …and the parameter that selects one exists and is flagged.
        Steinberg_Vst_ParamID programId = 0;
        bool foundProgramParam = false;
        for (Steinberg_int32 i = 0; i < numParams; ++i) {
          Steinberg_Vst_ParameterInfo info{};
          if (controller->lpVtbl->getParameterInfo(controller, i, &info) != Steinberg_kResultOk)
            continue;
          if ((info.flags & Steinberg_Vst_ParameterInfo_ParameterFlags_kIsProgramChange) == 0)
            continue;
          foundProgramParam = true;
          programId = info.id;
          check(info.stepCount == listInfo.programCount - 1,
                "…the program parameter has one step per gap, so the last one is reachable");
        }
        check(foundProgramParam, "…and a parameter flagged as THE program change");

        if (foundProgramParam) {
          // Selecting a program must MOVE THE CONTROLS. A list that names
          // presets and changes nothing is worse than no list, because the
          // user believes the preset loaded.
          Steinberg_Vst_ParameterInfo first{};
          controller->lpVtbl->getParameterInfo(controller, 0, &first);
          controller->lpVtbl->setParamNormalized(controller, first.id, 0.5);
          const double before =
              controller->lpVtbl->getParamNormalized(controller, first.id);

          controller->lpVtbl->setParamNormalized(controller, programId, 1.0); // the last one
          const double after = controller->lpVtbl->getParamNormalized(controller, first.id);
          check(std::fabs(after - before) > 1e-6,
                "…and selecting a program actually moves the controls");

          // The name the host shows for the selected program comes back from
          // the parameter, not only from the list.
          Steinberg_Vst_String128 shown{};
          check(controller->lpVtbl->getParamStringByValue(controller, programId, 1.0, shown) ==
                    Steinberg_kResultOk,
                "…and the parameter prints the preset's name");
          std::string narrow;
          for (int k = 0; k < 128 && shown[k]; ++k) narrow.push_back((char) shown[k]);
          check(!narrow.empty() && narrow != "1", "…as a name rather than a number");

          controller->lpVtbl->setParamNormalized(controller, programId, 0.0);
        }
      }
      units->lpVtbl->release(units);
    }
  }

  // ── IProcessContextRequirements ───────────────────────────────────────────
  //
  // VST3 3.7 made the process context something a plugin has to ASK for. A
  // plugin that does not implement this is treated as wanting everything,
  // which Cubase logs as a fault and which defeats the point: the host cannot
  // skip work nobody wants.
  //
  // The check is that we ask for exactly what readTransport() reads. Asking
  // for less means the host may stop filling a field the wrapper still
  // dereferences; asking for more is the fault the interface exists to fix.
  {
    Steinberg_Vst_IProcessContextRequirements* req = nullptr;
    const bool got = component->lpVtbl->queryInterface(
                         component, Steinberg_Vst_IProcessContextRequirements_iid,
                         (void**) &req) == Steinberg_kResultOk &&
                     req != nullptr;
    check(got, "IProcessContextRequirements is offered (VST3 3.7 expects it)");
    if (got && req) {
      const Steinberg_uint32 flags = req->lpVtbl->getProcessContextRequirements(req);
      const Steinberg_uint32 wanted =
          Steinberg_Vst_IProcessContextRequirements_Flags_kNeedTransportState |
          Steinberg_Vst_IProcessContextRequirements_Flags_kNeedTempo |
          Steinberg_Vst_IProcessContextRequirements_Flags_kNeedProjectTimeMusic |
          Steinberg_Vst_IProcessContextRequirements_Flags_kNeedBarPositionMusic |
          Steinberg_Vst_IProcessContextRequirements_Flags_kNeedTimeSignature;
      std::printf("  ---- context requirements: 0x%03x ----\n", (unsigned) flags);
      check(flags == wanted,
            "…and asks for exactly the fields the wrapper reads, no more");
      check((flags & Steinberg_Vst_IProcessContextRequirements_Flags_kNeedChord) == 0,
            "…not the chord track, which nothing here looks at");
      check((flags & Steinberg_Vst_IProcessContextRequirements_Flags_kNeedFrameRate) == 0,
            "…nor the video frame rate");
      req->lpVtbl->release(req);
    }
  }

  // ── INoteExpressionController ─────────────────────────────────────────────
  //
  // The wrapper has decoded VST3's native per-note expression events for a
  // long time, and none of that code could ever run: a host does not send note
  // expression to a plugin that has not declared which types it accepts. So
  // this checks the declaration AND then sends a real event through it, which
  // is the first time that path has executed at all.
  {
    Steinberg_Vst_INoteExpressionController* ne = nullptr;
    const bool got = component->lpVtbl->queryInterface(
                         component, Steinberg_Vst_INoteExpressionController_iid,
                         (void**) &ne) == Steinberg_kResultOk &&
                     ne != nullptr;
    if (expectMpe) {
      check(got, "an expressive instrument declares its note expression types");
    } else {
      check(!got, "a DSP that does not play expressively declares none");
    }

    if (got && ne) {
      const Steinberg_int32 count = ne->lpVtbl->getNoteExpressionCount(ne, 0, 0);
      check(count > 0, "…and there is at least one of them");
      check(ne->lpVtbl->getNoteExpressionCount(ne, 0, 99) == 0,
            "a channel that does not exist has none");

      bool sawTuning = false;
      for (Steinberg_int32 i = 0; i < count; ++i) {
        struct Steinberg_Vst_NoteExpressionTypeInfo info{};
        if (ne->lpVtbl->getNoteExpressionInfo(ne, 0, 0, i, &info) != Steinberg_kResultOk) continue;
        if (info.typeId == Steinberg_Vst_NoteExpressionTypeIDs_kTuningTypeID) {
          sawTuning = true;
          checkNear(info.valueDesc.defaultValue, 0.5, 1e-9,
                    "tuning rests at the NO-DETUNE point, not at the bottom of its range");
          check((info.flags &
                 Steinberg_Vst_NoteExpressionTypeInfo_NoteExpressionTypeFlags_kIsBipolar) != 0,
                "…and is declared bipolar, because it bends both ways");
        }
        if (info.typeId == Steinberg_Vst_NoteExpressionTypeIDs_kVolumeTypeID) {
          // 0.25 is VST3's 0 dB, and times four is the SDK's unity gain. This
          // is the check that would have caught the missing conversion.
          checkNear(info.valueDesc.defaultValue, 0.25, 1e-9,
                    "volume rests at UNITY once converted, not at a quarter of it");
          Steinberg_Vst_String128 shown{};
          ne->lpVtbl->getNoteExpressionStringByValue(
              ne, 0, 0, Steinberg_Vst_NoteExpressionTypeIDs_kVolumeTypeID, 0.25, shown);
          // String128 is UTF-16; the value is ASCII digits, so a narrowing
          // copy is enough and avoids dragging a converter into the test.
          char text[64];
          size_t k = 0;
          for (; k < sizeof(text) - 1 && shown[k]; ++k) text[k] = (char) shown[k];
          text[k] = 0;
          check(std::atof(text) > 0.99 && std::atof(text) < 1.01,
                "…and it says so: the default displays as a gain of 1");
        }
      }
      check(sawTuning, "tuning is among the declared types (MPE is unusable without it)");

      // ── and now make the previously unreachable path actually run ─────────
      if (isInstrument && !expectMidiOut) {
        std::vector<float> captured;
        auto capture = [&](int blocks) {
          captured.clear();
          for (int b = 0; b < blocks; ++b) {
            for (Steinberg_int32 i = 0; i < blockSize; ++i) inL[i] = inR[i] = 0.0f;
            processor->lpVtbl->process(processor, &data);
            queues.clear();
            events.clear();
            for (Steinberg_int32 i = 0; i < blockSize; ++i) captured.push_back(outL[i]);
          }
        };
        auto pitchOf = [&](const std::vector<float>& sig) {
          const size_t window = sig.size() / 2;
          if (window < 1000) return 0.0;
          double best = 0.0;
          size_t bestLag = 0;
          for (size_t lag = 48; lag < 800 && lag + window < sig.size(); ++lag) {
            double num = 0.0, a = 0.0, b = 0.0;
            for (size_t i = 0; i < window; ++i) {
              const double x = sig[i], y = sig[i + lag];
              num += x * y;
              a += x * x;
              b += y * y;
            }
            const double d = std::sqrt(a * b);
            const double r = d > 1e-12 ? num / d : 0.0;
            if (r > best) { best = r; bestLag = lag; }
          }
          return bestLag > 0 ? 48000.0 / (double) bestLag : 0.0;
        };

        queues.clear();
        events.clear();
        for (int key : {45, 60, 64, 67, 69, 72}) {
          Steinberg_Vst_Event off{};
          off.type = Steinberg_Vst_Event_EventTypes_kNoteOffEvent;
          off.Steinberg_Vst_Event_noteOff.channel = 0;
          off.Steinberg_Vst_Event_noteOff.pitch = key;
          off.Steinberg_Vst_Event_noteOff.noteId = -1;
          events.push_back(off);
        }
        capture(oneSecond * 2);

        Steinberg_Vst_Event on{};
        on.type = Steinberg_Vst_Event_EventTypes_kNoteOnEvent;
        on.Steinberg_Vst_Event_noteOn.channel = 0;
        on.Steinberg_Vst_Event_noteOn.pitch = 45; // A2, 110 Hz
        on.Steinberg_Vst_Event_noteOn.velocity = 1.0f;
        // A REAL note id, because that is the only thing a note expression
        // event carries: no key, no channel. A host that means to express a
        // note assigns it one, and -1 here would be testing a case no
        // expressive host produces.
        const Steinberg_int32 kNoteId = 4242;
        on.Steinberg_Vst_Event_noteOn.noteId = kNoteId;
        events.push_back(on);
        capture(oneSecond / 4);
        capture(oneSecond / 4);
        const double plain = pitchOf(captured);

        // A whole tone up, expressed the way VST3 does it: normalised around
        // 0.5 over a +/-120 semitone span.
        Steinberg_Vst_Event bend{};
        bend.type = Steinberg_Vst_Event_EventTypes_kNoteExpressionValueEvent;
        bend.Steinberg_Vst_Event_noteExpressionValue.typeId =
            Steinberg_Vst_NoteExpressionTypeIDs_kTuningTypeID;
        bend.Steinberg_Vst_Event_noteExpressionValue.noteId = kNoteId;
        bend.Steinberg_Vst_Event_noteExpressionValue.value = 0.5 + 2.0 / 240.0;
        events.push_back(bend);
        capture(oneSecond / 4);
        capture(oneSecond / 4);
        const double bent = pitchOf(captured);

        const double ratio = plain > 0.0 ? bent / plain : 0.0;
        std::printf("  ---- native note expression tuning: %.1f Hz -> %.1f Hz ----\n", plain,
                    bent);
        check(plain > 0.0, "the instrument sounds a held note");
        checkNear(ratio, 1.1225, 0.02,
                  "a NATIVE note expression event bends the note by a whole tone");

        // Release it, then play the SAME note again with no expression at all.
        // It must come back unbent.
        //
        // This is not a hypothetical. The sampler kept the bend on the voice
        // it reused, so the next note started a whole tone sharp: audible,
        // permanent until the Tune control was touched, and invisible to
        // every test that plays one note at a time.
        queues.clear();
        events.clear();
        Steinberg_Vst_Event off{};
        off.type = Steinberg_Vst_Event_EventTypes_kNoteOffEvent;
        off.Steinberg_Vst_Event_noteOff.channel = 0;
        off.Steinberg_Vst_Event_noteOff.pitch = 45;
        off.Steinberg_Vst_Event_noteOff.noteId = kNoteId;
        events.push_back(off);
        capture(oneSecond * 2);

        Steinberg_Vst_Event again{};
        again.type = Steinberg_Vst_Event_EventTypes_kNoteOnEvent;
        again.Steinberg_Vst_Event_noteOn.channel = 0;
        again.Steinberg_Vst_Event_noteOn.pitch = 45;
        again.Steinberg_Vst_Event_noteOn.velocity = 1.0f;
        again.Steinberg_Vst_Event_noteOn.noteId = kNoteId + 1;
        events.push_back(again);
        capture(oneSecond / 4);
        capture(oneSecond / 4);
        const double fresh = pitchOf(captured);
        std::printf("  ---- the next note, unexpressed: %.1f Hz ----\n", fresh);
        checkNear(fresh / (plain > 0.0 ? plain : 1.0), 1.0, 0.02,
                  "a NEW note is not still carrying the last one's bend");

        queues.clear();
        events.clear();
        Steinberg_Vst_Event off2{};
        off2.type = Steinberg_Vst_Event_EventTypes_kNoteOffEvent;
        off2.Steinberg_Vst_Event_noteOff.channel = 0;
        off2.Steinberg_Vst_Event_noteOff.pitch = 45;
        off2.Steinberg_Vst_Event_noteOff.noteId = kNoteId + 1;
        events.push_back(off2);
        capture(oneSecond);
      }
      ne->lpVtbl->release(ne);
    }
  }

  // ── IMidiMapping: the interface without which VST3 delivers no MIDI CC ────
  //
  // This is the part of VST3 that catches everyone out. Note on and note off
  // arrive as events; control change, pitch bend and aftertouch DO NOT. The
  // host converts them into parameter changes on ids the plugin publishes
  // here, and a plugin without this interface never receives one of them --
  // no mod wheel, no sustain pedal, no pitch bend, no aftertouch, silently.
  //
  // So this walks the whole chain rather than just checking the interface is
  // present: ask for the pitch bend id, send a parameter change on it, and
  // MEASURE whether the note actually bent.
  {
    Steinberg_Vst_IMidiMapping* mapping = nullptr;
    const bool got = component->lpVtbl->queryInterface(component, Steinberg_Vst_IMidiMapping_iid,
                                                       (void**) &mapping) == Steinberg_kResultOk &&
                     mapping != nullptr;
    if (isInstrument || expectMidiOut) {
      check(got, "an instrument offers IMidiMapping (without it VST3 sends no CC at all)");
    } else {
      check(!got, "an effect does not offer it, and does not publish 2080 hidden parameters");
    }

    if (got && mapping) {
      Steinberg_Vst_ParamID bendId = 0, ccId = 0, outOfRange = 0;
      check(mapping->lpVtbl->getMidiControllerAssignment(
                mapping, 0, 0, Steinberg_Vst_ControllerNumbers_kPitchBend, &bendId) ==
                Steinberg_kResultOk,
            "pitch bend on channel 1 has a parameter id");
      check(mapping->lpVtbl->getMidiControllerAssignment(mapping, 0, 3, 1, &ccId) ==
                Steinberg_kResultOk,
            "the mod wheel on channel 4 has its OWN id (MPE is channel-per-note)");
      check(bendId != ccId, "different controllers on different channels are different ids");
      check(mapping->lpVtbl->getMidiControllerAssignment(mapping, 0, 99, 1, &outOfRange) !=
                Steinberg_kResultOk,
            "a channel that does not exist is refused rather than given an id");

      // Every mapped id must be a parameter the controller actually declares,
      // or the host writes to something that is not there.
      bool bendDeclared = false;
      const Steinberg_int32 total = controller->lpVtbl->getParameterCount(controller);
      for (Steinberg_int32 i = 0; i < total; ++i) {
        Steinberg_Vst_ParameterInfo pi{};
        if (controller->lpVtbl->getParameterInfo(controller, i, &pi) != Steinberg_kResultOk)
          continue;
        if (pi.id == bendId) {
          bendDeclared = true;
          check((pi.flags & Steinberg_Vst_ParameterInfo_ParameterFlags_kIsHidden) != 0,
                "…and it is HIDDEN, so it does not clutter the user's parameter list");
          check((pi.flags & Steinberg_Vst_ParameterInfo_ParameterFlags_kCanAutomate) != 0,
                "…but automatable, or the host would never send it anything");
          checkNear(pi.defaultNormalizedValue, 0.5, 1e-9,
                    "…and pitch bend rests at CENTRE, not at the bottom of its range");
        }
      }
      check(bendDeclared, "the mapped id is a parameter the controller really declares");

      // ── and now the part that proves it is wired to the audio ────────────
      if (isInstrument && !expectMidiOut) {
        std::vector<float> captured;
        auto capture = [&](int blocks) {
          captured.clear();
          for (int b = 0; b < blocks; ++b) {
            for (Steinberg_int32 i = 0; i < blockSize; ++i) inL[i] = inR[i] = 0.0f;
            processor->lpVtbl->process(processor, &data);
            queues.clear();
            events.clear();
            for (Steinberg_int32 i = 0; i < blockSize; ++i) captured.push_back(outL[i]);
          }
        };
        // Autocorrelation, so no assumption about what the instrument is: any
        // periodic signal gives up its period this way, harmonics and all.
        auto pitchOf = [&](const std::vector<float>& sig) {
          const size_t window = sig.size() / 2;
          if (window < 1000) return 0.0;
          double best = 0.0;
          size_t bestLag = 0;
          for (size_t lag = 48; lag < 800 && lag + window < sig.size(); ++lag) {
            double num = 0.0, a = 0.0, b = 0.0;
            for (size_t i = 0; i < window; ++i) {
              const double x = sig[i], y = sig[i + lag];
              num += x * y;
              a += x * x;
              b += y * y;
            }
            const double d = std::sqrt(a * b);
            const double r = d > 1e-12 ? num / d : 0.0;
            if (r > best) { best = r; bestLag = lag; }
          }
          return bestLag > 0 ? 48000.0 / (double) bestLag : 0.0;
        };

        // A clean start: defaults, every earlier note released.
        queues.clear();
        for (Steinberg_int32 i = 0; i < numParams; ++i) {
          Steinberg_Vst_ParameterInfo pi{};
          if (controller->lpVtbl->getParameterInfo(controller, i, &pi) != Steinberg_kResultOk)
            continue;
          if (pi.id >= 0x10000000u) continue; // leave the MIDI ids alone here
          ValueQueue q;
          q.lpVtbl = &g_vqVtbl;
          q.id = pi.id;
          q.value = pi.defaultNormalizedValue;
          queues.push_back(q);
        }
        events.clear();
        for (int key : {60, 64, 67, 69, 72}) {
          Steinberg_Vst_Event off{};
          off.type = Steinberg_Vst_Event_EventTypes_kNoteOffEvent;
          off.Steinberg_Vst_Event_noteOff.channel = 0;
          off.Steinberg_Vst_Event_noteOff.pitch = key;
          off.Steinberg_Vst_Event_noteOff.noteId = -1;
          events.push_back(off);
        }
        capture(oneSecond * 2);

        Steinberg_Vst_Event on{};
        on.type = Steinberg_Vst_Event_EventTypes_kNoteOnEvent;
        on.Steinberg_Vst_Event_noteOn.channel = 0;
        on.Steinberg_Vst_Event_noteOn.pitch = 45; // A2, 110 Hz: a low note makes
        on.Steinberg_Vst_Event_noteOn.velocity = 1.0f; //  a whole-tone bend obvious
        on.Steinberg_Vst_Event_noteOn.noteId = -1;
        events.push_back(on);
        capture(oneSecond / 4);
        capture(oneSecond / 4);
        const double plain = pitchOf(captured);

        // Bend up, the only way a VST3 host can: a parameter change.
        queues.clear();
        ValueQueue bend;
        bend.lpVtbl = &g_vqVtbl;
        bend.id = bendId;
        // A WHOLE TONE, not the maximum.
        //
        // The first version sent 1.0 and printed 110 Hz -> 251 Hz, which looks
        // like a pass and is not one: MPE's default range is 48 semitones, so
        // full deflection is 1760 Hz, and the autocorrelation below searches
        // lags 48..800: it cannot see anything above 1000 Hz and locked onto
        // a sub-multiple instead. The bend was right and the measurement was
        // lying. A modest bend keeps the answer inside the instrument's range
        // and turns "it moved" into an exact ratio worth checking.
        //
        // 0.5 is centre; +2 of 48 semitones is +2/48 of the half-range.
        bend.value = 0.5 + (2.0 / 48.0) * 0.5;
        queues.push_back(bend);
        capture(oneSecond / 4);
        capture(oneSecond / 4);
        const double bent = pitchOf(captured);

        const double ratio = plain > 0.0 ? bent / plain : 0.0;
        std::printf("  ---- pitch bend through IMidiMapping: %.1f Hz -> %.1f Hz ----\n", plain,
                    bent);
        check(plain > 0.0, "the instrument sounds a held note");
        // 2^(2/12) = 1.1225. Checked as an interval rather than as "it went
        // up", because a chain that applies the bend twice, or halves it, or
        // rounds the 14-bit value to 7 bits would all still go up.
        checkNear(ratio, 1.1225, 0.02,
                  "a pitch bend parameter change bends the note by exactly a WHOLE TONE");

        queues.clear();
        events.clear();
        Steinberg_Vst_Event off{};
        off.type = Steinberg_Vst_Event_EventTypes_kNoteOffEvent;
        off.Steinberg_Vst_Event_noteOff.channel = 0;
        off.Steinberg_Vst_Event_noteOff.pitch = 45;
        off.Steinberg_Vst_Event_noteOff.noteId = -1;
        events.push_back(off);
        capture(oneSecond);
      }
      mapping->lpVtbl->release(mapping);
    }
  }

  // ── Latency ───────────────────────────────────────────────────────────────
  {
    const Steinberg_uint32 latency = processor->lpVtbl->getLatencySamples(processor);
    check(latency < 100000, "the plugin reports a sane latency");
    std::printf("  ---- reported latency: %u samples ----\n", (unsigned) latency);

    // Sane is not the same as true. The host shifts this plugin's output back
    // by `latency`; if the signal is not actually that late, the host has just
    // pushed it EARLY against every other track. Measured by cross-correlating
    // a broadband burst: noise rather than a tone, because a periodic input
    // correlates equally well at every multiple of its period.
    //
    // Only plugins declaring a latency are measured: a minimum-phase filter
    // has real group delay and correctly declares none, and nothing here can
    // tell that apart from an undeclared delay. The probe is skipped because
    // it declares 64 to prove the number travels, and delays nothing.
    if (latency > 0 && !isInstrument && !expectBridge) {
      queues.clear();
      for (Steinberg_int32 i = 0; i < numParams; ++i) {
        Steinberg_Vst_ParameterInfo pi{};
        if (controller->lpVtbl->getParameterInfo(controller, i, &pi) != Steinberg_kResultOk)
          continue;
        ValueQueue q;
        q.lpVtbl = &g_vqVtbl;
        q.id = pi.id;
        q.value = pi.defaultNormalizedValue;
        queues.push_back(q);
      }
      run(1, true);
      queues.clear();
      run(oneSecond / 4, true); // settle at defaults

      const size_t captureLen = 16384;
      const size_t maxLag = (size_t) latency * 2 + 512;
      std::vector<float> sent, got;
      unsigned lcg = 12345u;
      while (got.size() < captureLen + maxLag) {
        for (Steinberg_int32 i = 0; i < blockSize; ++i) {
          lcg = lcg * 1664525u + 1013904223u;
          const float v = (float) ((int) (lcg >> 8) % 20001 - 10000) / 40000.0f;
          inL[i] = inR[i] = v;
          sent.push_back(v);
        }
        processor->lpVtbl->process(processor, &data);
        queues.clear();
        events.clear();
        for (Steinberg_int32 i = 0; i < blockSize; ++i) got.push_back(outL[i]);
      }

      double best = -1.0;
      size_t bestLag = 0;
      for (size_t lag = 0; lag <= maxLag && lag + captureLen <= got.size(); ++lag) {
        double num = 0.0, a = 0.0, b = 0.0;
        for (size_t i = 0; i < captureLen; ++i) {
          const double x = sent[i], y = got[i + lag];
          num += x * y;
          a += x * x;
          b += y * y;
        }
        const double denom = std::sqrt(a * b);
        const double r = denom > 1e-12 ? std::fabs(num / denom) : 0.0;
        if (r > best) {
          best = r;
          bestLag = lag;
        }
      }

      std::printf("  ---- latency: declared %u samples, measured %u (r=%.3f) ----\n",
                  (unsigned) latency, (unsigned) bestLag, best);
      check(best > 0.2, "the output correlates with the input at SOME lag");
      const size_t error = bestLag > latency ? bestLag - latency : (size_t) latency - bestLag;
      check(error <= 32, "the declared latency is the delay the plugin ACTUALLY has");
    }
  }

  // ── Tail ──────────────────────────────────────────────────────────────────
  {
    const Steinberg_uint32 tail = processor->lpVtbl->getTailSamples(processor);
    if (expectBridge) check(tail == 4321, "a DSP's declared tail reaches the host");
    check(tail <= 10 * 48000u, "the declared tail is plausible (<10 s)");

    // Measured, not assumed: the same correction the CLAP host test needed.
    // This read `tail == 0` for everything that was not the GUI probe, keyed
    // off a flag about the JavaScript bridge, and it held only because no
    // plugin here had a tail yet. The convolution reverb broke it, and the
    // saturator turned out to have been wrong all along: it declared none and
    // its DC blocker was still ringing thousands of samples later.
    //
    // The probe is excluded because its right channel carries transport
    // telemetry as constant DC, so it never decays by design. Instruments are
    // excluded because they have no audio input to stop.
    if (!isInstrument && !expectBridge) {
      queues.clear();
      for (Steinberg_int32 i = 0; i < numParams; ++i) {
        Steinberg_Vst_ParameterInfo pi{};
        if (controller->lpVtbl->getParameterInfo(controller, i, &pi) != Steinberg_kResultOk)
          continue;
        ValueQueue q;
        q.lpVtbl = &g_vqVtbl;
        q.id = pi.id;
        q.value = pi.defaultNormalizedValue;
        queues.push_back(q);
      }
      run(1, true);
      queues.clear();
      run(oneSecond / 4, true);  // settle at defaults
      run(oneSecond / 4, false); // excite

      const double audible = 1e-4;
      Steinberg_uint32 decaySamples = 0, elapsed = 0;
      for (int b = 0; b < oneSecond * 2; ++b) {
        for (Steinberg_int32 i = 0; i < blockSize; ++i) inL[i] = inR[i] = 0.0f;
        processor->lpVtbl->process(processor, &data);
        queues.clear();
        events.clear();
        for (Steinberg_int32 i = 0; i < blockSize; ++i, ++elapsed)
          if (std::fabs(outL[i]) > audible || std::fabs(outR[i]) > audible)
            decaySamples = elapsed + 1;
      }

      std::printf("  ---- tail: declared %u samples, measured %u ----\n", (unsigned) tail,
                  (unsigned) decaySamples);
      // Small on purpose: a generous margin here hides exactly the
      // under-declarations this check exists to find. See the CLAP host test.
      check(decaySamples <= tail + 256u,
            "the declared tail COVERS the decay the plugin actually produces");
    }
  }

  // ── Host bypass ───────────────────────────────────────────────────────────
  // The contract, not just the flag: engage the bypass parameter and the
  // output must become the INPUT, aligned to the plugin's own latency. A
  // bypass that merely mutes, forgets the dry delay, or leaves the DSP in the
  // path would all fail this.
  if (!isInstrument) {
    // Found by its FLAG rather than by being last. It WAS last, right up
    // until the program-change parameter landed after it, and then this check
    // failed on a plugin that had done nothing wrong. Which index carries the
    // bypass is the wrapper's business; the flag is what a host looks for.
    Steinberg_Vst_ParameterInfo binfo{};
    bool foundBypass = false;
    for (Steinberg_int32 i = 0; i < numParams; ++i) {
      Steinberg_Vst_ParameterInfo probe{};
      if (controller->lpVtbl->getParameterInfo(controller, i, &probe) != Steinberg_kResultOk)
        continue;
      if ((probe.flags & Steinberg_Vst_ParameterInfo_ParameterFlags_kIsBypass) == 0) continue;
      binfo = probe;
      foundBypass = true;
      break;
    }
    check(foundBypass, "an effect has a parameter flagged kIsBypass");

    const Steinberg_uint32 L = processor->lpVtbl->getLatencySamples(processor);

    ValueQueue q;
    q.lpVtbl = &g_vqVtbl;
    q.id = binfo.id;
    q.value = 1.0; // engage
    queues.push_back(q);

    int phase = 0;
    double maxErr = 0.0;
    for (int b = 0; b < 40; ++b) {
      for (Steinberg_int32 i = 0; i < blockSize; ++i) {
        inL[i] = inR[i] =
            0.25f * (float) std::sin(2.0 * 3.14159265358979 * 997.0 * phase / 48000.0);
        ++phase;
      }
      processor->lpVtbl->process(processor, &data);
      queues.clear();
      if (b >= 20) { // the 20 ms crossfade is long over by here
        for (Steinberg_int32 i = 0; i < blockSize; ++i) {
          const int n = phase - blockSize + i - (int) L;
          const double expect = 0.25 * std::sin(2.0 * 3.14159265358979 * 997.0 * n / 48000.0);
          const double err = std::fabs((double) outL[i] - expect);
          if (err > maxErr) maxErr = err;
        }
      }
    }
    check(maxErr < 1e-3, "bypassed output IS the input, latency-aligned");
    check(controller->lpVtbl->getParamNormalized(controller, binfo.id) >= 0.5,
          "the controller reflects the engaged bypass");

    // Disengage and prove the wet path comes back: with the first parameter at
    // maximum the saturator audibly reshapes a 0.25 sine, so wet != dry.
    ValueQueue off = q;
    off.value = 0.0;
    queues.push_back(off);
    ValueQueue drive;
    drive.lpVtbl = &g_vqVtbl;
    drive.id = 0;
    drive.value = 1.0;
    queues.push_back(drive);
    // A ducker's processing is only audible when its key is hot.
    if (expectSidechain)
      for (Steinberg_int32 i = 0; i < blockSize; ++i) scL[i] = scR[i] = 0.9f;
    double wetDiff = 0.0;
    for (int b = 0; b < 20; ++b) {
      for (Steinberg_int32 i = 0; i < blockSize; ++i) {
        inL[i] = inR[i] =
            0.25f * (float) std::sin(2.0 * 3.14159265358979 * 997.0 * phase / 48000.0);
        ++phase;
      }
      processor->lpVtbl->process(processor, &data);
      queues.clear();
      if (b >= 15) {
        for (Steinberg_int32 i = 0; i < blockSize; ++i) {
          const int n = phase - blockSize + i - (int) L;
          const double dry = 0.25 * std::sin(2.0 * 3.14159265358979 * 997.0 * n / 48000.0);
          const double err = std::fabs((double) outL[i] - dry);
          if (err > wetDiff) wetDiff = err;
        }
      }
    }
    check(wetDiff > 1e-3, "…and disengaging brings the processing back");
    if (expectSidechain) {
      std::fill(scL.begin(), scL.end(), 0.0f);
      std::fill(scR.begin(), scR.end(), 0.0f);
    }

    // Leave the parameters as the later sections expect them.
    ValueQueue restore = drive;
    restore.value = 0.0;
    queues.push_back(restore);
    run(1, true);
    queues.clear();
  }

  // ── State round-trip ──────────────────────────────────────────────────────
  {
    controller->lpVtbl->setParamNormalized(controller, 0, 0.6);
    MemStream saved;
    saved.lpVtbl = &g_streamVtbl;
    check(component->lpVtbl->getState(component, (Steinberg_IBStream*) &saved) ==
              Steinberg_kResultOk,
          "state saves");
    check(!saved.bytes.empty(), "…and produced bytes");

    // ── The host introduced itself ──────────────────────────────────
    //
    // VST3 makes a plugin ASK: the context handed to initialize may or may
    // not be an IHostApplication, and that is the only place a host names
    // itself. The wrapper used to discard that parameter entirely, so a
    // plugin could not tell one host from another -- and this test used to
    // pass nullptr, which proves only that the plugin survives not being
    // told.
    if (expectBridge) {
      const std::string identity(saved.bytes.begin(), saved.bytes.end());
      const bool named = identity.find("Sonore VST3 Test Host") != std::string::npos;
      std::printf("  ---- host identity in saved state: %s ----\n", named ? "present" : "ABSENT");
      check(named, "the plugin was told which host is running it, by name");
    }

    // ── And which track it is on ─────────────────────────────────
    //
    // A plugin that knows its track can tint itself to match it, which is
    // what makes a rack of eight identical compressors eight distinguishable
    // ones. VST3 pushes this through IInfoListener rather than letting the
    // plugin ask, and offers strictly less than CLAP does: a name, a colour
    // and an index, but nothing saying whether this is a return or the
    // master.
    if (expectBridge) {
      Steinberg_Vst_ChannelContext_IInfoListener* listener = nullptr;
      const bool got = controller->lpVtbl->queryInterface(
                           controller, Steinberg_Vst_ChannelContext_IInfoListener_iid,
                           (void**) &listener) == Steinberg_kResultOk;
      check(got && listener, "a plugin that wants track info offers IInfoListener");
      if (got && listener) {
        g_attrs.lpVtbl = &g_attrVtbl;
        check(listener->lpVtbl->setChannelContextInfos(
                  listener, (struct Steinberg_Vst_IAttributeList*) &g_attrs) ==
                  Steinberg_kResultOk,
              "…and accepts the host's attribute list");
        listener->lpVtbl->release(listener);

        MemStream afterTrack;
        afterTrack.lpVtbl = &g_streamVtbl;
        check(component->lpVtbl->getState(component, (Steinberg_IBStream*) &afterTrack) ==
                  Steinberg_kResultOk,
              "…and its state still saves");
        const std::string bytes(afterTrack.bytes.begin(), afterTrack.bytes.end());
        const bool named = bytes.find("Verb Return") != std::string::npos;
        const bool coloured = bytes.find("#2ab17c") != std::string::npos;
        std::printf("  ---- track: name %s, colour %s ----\n",
                    named ? "present" : "ABSENT", coloured ? "present" : "ABSENT");
        check(named, "…the track's name reached the plugin, UTF-16 decoded");
        check(coloured, "…and its colour, unpacked from the ARGB word");
      }
    }

    controller->lpVtbl->setParamNormalized(controller, 0, 0.1);
    saved.pos = 0;
    check(component->lpVtbl->setState(component, (Steinberg_IBStream*) &saved) ==
              Steinberg_kResultOk,
          "state loads");
    checkNear(controller->lpVtbl->getParamNormalized(controller, 0), 0.6, 1e-3,
              "the saved value came back");

    MemStream junk;
    junk.lpVtbl = &g_streamVtbl;
    junk.bytes = {'n', 'o', 'p', 'e', 0, 0, 0, 0, 0, 0, 0, 0};
    check(component->lpVtbl->setState(component, (Steinberg_IBStream*) &junk) !=
              Steinberg_kResultOk,
          "a corrupt state blob is refused");
  }

  // ── The event stream under mutation ─────────────────────────────────────────
  //
  // The CLAP host test's sibling: parameter queues naming ids nobody declared
  // with values that are not numbers, and events of every type -- including
  // ones that do not exist -- with every field random, offsets past the block,
  // sysex whose size and buffer disagree. After each block the output must be
  // finite and every parameter must still be a finite normalised value.
  {
    std::mt19937 rng(0x5EEDF00Du);
    auto rnd = [&](uint32_t n) { return n ? (uint32_t) (rng() % n) : 0u; };
    auto rndDouble = [&]() -> double {
      switch (rnd(8)) {
        case 0: return std::numeric_limits<double>::quiet_NaN();
        case 1: return std::numeric_limits<double>::infinity();
        case 2: return -std::numeric_limits<double>::infinity();
        case 3: return 1e300;
        case 4: return -1e300;
        case 5: return (double) rng() / 4294967295.0 * 3.0 - 1.0;
        default: return (double) rng() / 4294967295.0;
      }
    };
    const Steinberg_int32 nP = controller->lpVtbl->getParameterCount(controller);
    std::vector<uint8_t> sysexBytes(4096);
    for (auto& b : sysexBytes) b = (uint8_t) rng();
    bool finiteAll = true;
    int fuzzedBlocks = 0, fuzzedEvents = 0, fuzzedPoints = 0;
    for (int it = 0; it < 600; ++it) {
      queues.clear();
      events.clear();
      const int nQ = (int) rnd(6);
      for (int q = 0; q < nQ; ++q, ++fuzzedPoints) {
        ValueQueue vq;
        vq.lpVtbl = &g_vqVtbl;
        vq.id = rnd(3) == 0 ? (Steinberg_Vst_ParamID) rng()
                            : (Steinberg_Vst_ParamID) rnd((uint32_t) nP + 3);
        vq.value = rndDouble();
        queues.push_back(vq);
      }
      const int nE = (int) rnd(8);
      for (int k = 0; k < nE; ++k, ++fuzzedEvents) {
        Steinberg_Vst_Event e{};
        auto* bytes = reinterpret_cast<uint8_t*>(&e);
        for (size_t i = 0; i < sizeof(e); ++i) bytes[i] = (uint8_t) rng();
        e.busIndex = rnd(3) == 0 ? (Steinberg_int32) rng() : 0;
        e.sampleOffset = rnd(4) == 0 ? (Steinberg_int32) rng() : (Steinberg_int32) rnd((uint32_t) blockSize + 1);
        e.ppqPosition = rndDouble();
        e.type = (Steinberg_uint16) (rnd(4) == 0 ? rng() : rnd(9)); // 0..7 exist
        if (e.type == Steinberg_Vst_Event_EventTypes_kDataEvent) {
          e.Steinberg_Vst_Event_data.bytes = rnd(4) == 0 ? nullptr : sysexBytes.data();
          e.Steinberg_Vst_Event_data.size =
              rnd(3) == 0 ? (Steinberg_uint32) rng() : rnd((uint32_t) sysexBytes.size() + 1);
        }
        if (e.type == Steinberg_Vst_Event_EventTypes_kNoteOnEvent ||
            e.type == Steinberg_Vst_Event_EventTypes_kNoteOffEvent) {
          e.Steinberg_Vst_Event_noteOn.velocity = (float) rndDouble();
          e.Steinberg_Vst_Event_noteOn.tuning = (float) rndDouble();
        }
        if (e.type == Steinberg_Vst_Event_EventTypes_kNoteExpressionValueEvent)
          e.Steinberg_Vst_Event_noteExpressionValue.value = rndDouble();
        events.push_back(e);
      }
      for (Steinberg_int32 i = 0; i < blockSize; ++i) {
        const float s = 0.25f * (float) std::sin(2.0 * 3.14159265358979 * 440.0 * i / 48000.0);
        inL[i] = s;
        inR[i] = s;
      }
      processor->lpVtbl->process(processor, &data);
      for (Steinberg_int32 i = 0; i < blockSize; ++i)
        if (!std::isfinite(outL[i]) || !std::isfinite(outR[i])) finiteAll = false;
      ++fuzzedBlocks;
    }
    queues.clear();
    events.clear();
    char note[220];
    std::snprintf(note, sizeof(note), "%d blocks carrying %d mutated events and %d mutated "
                  "parameter points processed to finite audio", fuzzedBlocks, fuzzedEvents,
                  fuzzedPoints);
    check(finiteAll, note);
    bool inRange = true;
    for (Steinberg_int32 i = 0; i < nP; ++i) {
      Steinberg_Vst_ParameterInfo pi{};
      if (controller->lpVtbl->getParameterInfo(controller, i, &pi) != Steinberg_kResultOk) continue;
      const double v = controller->lpVtbl->getParamNormalized(controller, pi.id);
      if (!std::isfinite(v) || v < -1e-9 || v > 1.0 + 1e-9) {
        inRange = false;
        std::printf("  ---- parameter %u reads %g normalised ----\n", pi.id, v);
      }
    }
    check(inRange, "...and every parameter is a finite normalised value afterwards");
    for (Steinberg_int32 i = 0; i < nP; ++i) {
      Steinberg_Vst_ParameterInfo pi{};
      if (controller->lpVtbl->getParameterInfo(controller, i, &pi) != Steinberg_kResultOk) continue;
      ValueQueue vq;
      vq.lpVtbl = &g_vqVtbl;
      vq.id = pi.id;
      vq.value = pi.defaultNormalizedValue;
      queues.push_back(vq);
    }
    const Result afterStorm = run(8, isInstrument);
    check(afterStorm.finite, "...and it still processes afterwards");
  }

  // ── The editor ────────────────────────────────────────────────────────────
  {
    Steinberg_IPlugView* view = controller->lpVtbl->createView(controller, "editor");
    check(view != nullptr, "the controller creates an editor view");
    if (view) {
#if defined(_WIN32)
      const char* platform = Steinberg_kPlatformTypeHWND;
#elif defined(__APPLE__)
      const char* platform = Steinberg_kPlatformTypeNSView;
#else
      const char* platform = Steinberg_kPlatformTypeX11EmbedWindowID;
#endif
      check(view->lpVtbl->isPlatformTypeSupported(view, platform) == Steinberg_kResultTrue,
            "the view supports this platform's window type");
      check(view->lpVtbl->isPlatformTypeSupported(view, "NonsenseType") != Steinberg_kResultTrue,
            "an unknown window type is refused");

      Steinberg_ViewRect rect{};
      check(view->lpVtbl->getSize(view, &rect) == Steinberg_kResultOk &&
                rect.right > rect.left && rect.bottom > rect.top,
            "the view reports a usable size");
      check(view->lpVtbl->canResize(view) == Steinberg_kResultTrue, "the view is resizable");

      Steinberg_ViewRect tiny{0, 0, 10, 10};
      view->lpVtbl->checkSizeConstraint(view, &tiny);
      check(tiny.right - tiny.left >= 320 && tiny.bottom - tiny.top >= 200,
            "a size constraint enforces a floor instead of collapsing the page");

      Steinberg_ViewRect resized{0, 0, 640, 380};
      check(view->lpVtbl->onSize(view, &resized) == Steinberg_kResultOk, "the view accepts a size");
      view->lpVtbl->getSize(view, &rect);
      check(rect.right - rect.left == 640 && rect.bottom - rect.top == 380,
            "…and reports the new size back");

#if defined(_WIN32)
      // Attach for real. The contract checks above pass just as happily against
      // a view that never manages to show anything, which is precisely the
      // failure mode a plugin ships with.
      HWND parent = CreateWindowExW(0, L"STATIC", L"sonore vst3 host", WS_OVERLAPPEDWINDOW, 0, 0,
                                    640, 380, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
      check(parent != nullptr, "the test host created a parent window");
      if (parent) {
        check(view->lpVtbl->attached(view, parent, platform) == Steinberg_kResultOk,
              "the view attaches to a real host window");

        // The same measurement the CLAP host test makes, and the point of
        // making it twice: one plugin must not open a native editor in one
        // format and a web one in another.
        {
          HWND child = GetWindow(parent, GW_CHILD);
          wchar_t className[128] = {0};
          if (child) GetClassNameW(child, className, 128);
          const bool isNative = std::wcsncmp(className, L"SonoreNativeUI_", 15) == 0;
          char narrow[160] = {0};
          WideCharToMultiByte(CP_UTF8, 0, className, -1, narrow, sizeof(narrow) - 1, nullptr,
                              nullptr);
          char line[240];
          std::snprintf(line, sizeof(line), "the editor window is of class \"%s\"", narrow);
          check(child != nullptr, line);
          if (expectEditor == 1)
            check(isNative, "the VST3 view opened the SAME native editor the CLAP build opens");
          else if (expectEditor == 2)
            check(!isNative && child != nullptr, "and a plugin with a page still gets its page");
        }

        MSG msg;
        auto pump = [&](int rounds) {
          for (int i = 0; i < rounds; ++i) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
              TranslateMessage(&msg);
              DispatchMessageW(&msg);
            }
            Sleep(5);
          }
        };
        pump(400);
        check(true, "the editor survived two seconds of a real message loop");

        if (expectBridge) {
          // The probe's page drives parameter 1 to 0.777 on load. Seeing it here
          // proves the whole VST3 editor chain: view attached, webview created,
          // bridge injected, message parsed, queue drained on the audio thread,
          // AND that the host was told, through IComponentHandler.
          double probe = 0.0;
          for (int i = 0; i < 200; ++i) {
            pump(2);
            processor->lpVtbl->process(processor, &data);
            queues.clear();
            events.clear();
            probe = controller->lpVtbl->normalizedParamToPlain(
                controller, 1, controller->lpVtbl->getParamNormalized(controller, 1));
            if (std::fabs(probe - 0.777) < 1e-3) break;
          }
          checkNear(probe, 0.777, 1e-3, "the page drove a real parameter through the bridge");
          check(handler.edits > 0, "…and the host was told through IComponentHandler");

          // The bag, on its own parameter. The page moves 0 to 1.25 only when
          // a state object arrives with a host name in it, so this value is
          // the bag having crossed rather than a coincidence.
          double gain = 0.0;
          for (int i = 0; i < 200; ++i) {
            pump(2);
            processor->lpVtbl->process(processor, &data);
            queues.clear();
            events.clear();
            gain = controller->lpVtbl->normalizedParamToPlain(
                controller, 0, controller->lpVtbl->getParamNormalized(controller, 0));
            if (std::fabs(gain - 1.25) < 1e-3) break;
          }
          checkNear(gain, 1.25, 1e-3, "the DSP's state bag reached the page");

          // ── A right-click reaches the host ───────────────────
          //
          // The same journey as the parameter above, ending at the HOST
          // instead of the DSP. VST3 spells it as a third interface on the
          // handler: the plugin asks for a menu for one parameter, already
          // populated with the host's own items, and pops it up. Without it a
          // user cannot MIDI-learn a knob or remove its automation -- those
          // live in that menu and nowhere else.
          for (int i = 0; i < 100 && g_menuPopups == 0; ++i) pump(2);
          std::printf("  ---- context menu: %d created, %d popup(s) for param %u at %d,%d, "
                      "%d release(s) ----\n",
                      g_menuCreated, g_menuPopups, (unsigned) g_menuParamId, g_menuX, g_menuY,
                      g_menuReleases);
          check(g_menuCreated > 0, "a right-click in the page asks the host for its menu");
          check(g_menuParamId == 2, "…naming the control that was clicked");
          check(g_menuPopups > 0, "…and the plugin actually shows it");
          check(g_menuX == 12 && g_menuY == 34, "…at the position the click happened");
          // The menu is a COM object the plugin now owns. Leaking one leaks a
          // window as well as memory, and nothing else in a DAW will free it.
          check(g_menuReleases > 0 && g_contextMenu.refs == 0,
                "…and releases it afterwards rather than leaking a window");

          // ── Latency that MOVES ──────────────────────────────
          //
          // A plugin whose oversampling or look-ahead is a switch has to tell
          // the host every time it moves, or the host's delay compensation
          // stays on the old number and everything running in parallel with it
          // is quietly early. VST3 says that with
          // restartComponent(kLatencyChanged), and this wrapper never sent it
          // at all.
          //
          // The sequence is the one a host produces: write the parameter,
          // process a block so the DSP sees it, then POLL a parameter -- which
          // is where the check lives, because VST3 gives a plugin no way to
          // ask to be called on the main thread.
          {
            Steinberg_Vst_ParamID switchId = 0xffffffff;
            const Steinberg_int32 n = controller->lpVtbl->getParameterCount(controller);
            for (Steinberg_int32 i = 0; i < n; ++i) {
              Steinberg_Vst_ParameterInfo info{};
              if (controller->lpVtbl->getParameterInfo(controller, i, &info) !=
                  Steinberg_kResultOk)
                continue;
              std::string title;
              for (int k = 0; k < 128 && info.title[k]; ++k) title.push_back((char) info.title[k]);
              if (title == "Big Latency") switchId = info.id;
            }
            check(switchId != 0xffffffff, "the probe exposes a latency switch");

            if (switchId != 0xffffffff) {
              const Steinberg_uint32 before = processor->lpVtbl->getLatencySamples(processor);
              handler.latencyRestarts = 0;

              controller->lpVtbl->setParamNormalized(controller, switchId, 1.0);
              {
                ValueQueue q;
                q.lpVtbl = &g_vqVtbl;
                q.id = switchId;
                q.value = 1.0;
                queues.push_back(q);
              }
              processor->lpVtbl->process(processor, &data);
              queues.clear();
              // The poll a host does to keep its display current.
              controller->lpVtbl->getParamNormalized(controller, 0);

              const Steinberg_uint32 after = processor->lpVtbl->getLatencySamples(processor);
              std::printf("  ---- latency switch: %u -> %u samples, %d restart(s) ----\n",
                          (unsigned) before, (unsigned) after, handler.latencyRestarts);
              check(after == 512, "moving the switch moves the plugin's latency");
              check(handler.latencyRestarts > 0,
                    "…and the host is told with restartComponent(kLatencyChanged)");

              // Back, and told again: a notification that fires one way leaves
              // the compensation wrong for half of what a user does.
              handler.latencyRestarts = 0;
              controller->lpVtbl->setParamNormalized(controller, switchId, 0.0);
              {
                ValueQueue q;
                q.lpVtbl = &g_vqVtbl;
                q.id = switchId;
                q.value = 0.0;
                queues.push_back(q);
              }
              processor->lpVtbl->process(processor, &data);
              queues.clear();
              controller->lpVtbl->getParamNormalized(controller, 0);
              check(processor->lpVtbl->getLatencySamples(processor) == 64,
                    "switching back restores the latency");
              check(handler.latencyRestarts > 0, "…with a notification for that too");

              // ── A control can refuse to be automated ──────────────
              //
              // The latency switch reconfigures the plugin rather than moving
              // it, and a host recording that as automation would re-plan its
              // delay compensation at every point on the curve. Every
              // parameter carried kCanAutomate unconditionally before, so the
              // check that ordinary ones still do matters as much.
              {
                Steinberg_Vst_ParameterInfo plain{}, switched{};
                bool foundSwitch = false;
                controller->lpVtbl->getParameterInfo(controller, 0, &plain);
                const Steinberg_int32 total =
                    controller->lpVtbl->getParameterCount(controller);
                for (Steinberg_int32 i = 0; i < total; ++i) {
                  Steinberg_Vst_ParameterInfo info{};
                  if (controller->lpVtbl->getParameterInfo(controller, i, &info) !=
                      Steinberg_kResultOk)
                    continue;
                  std::string title;
                  for (int k = 0; k < 128 && info.title[k]; ++k)
                    title.push_back((char) info.title[k]);
                  if (title == "Big Latency") {
                    switched = info;
                    foundSwitch = true;
                  }
                }
                check(foundSwitch, "the latency switch is in the parameter list");
                const bool plainOk =
                    (plain.flags &
                     Steinberg_Vst_ParameterInfo_ParameterFlags_kCanAutomate) != 0;
                const bool switchOk =
                    (switched.flags &
                     Steinberg_Vst_ParameterInfo_ParameterFlags_kCanAutomate) != 0;
                std::printf("  ---- kCanAutomate: param 0 %s, latency switch %s ----\n",
                            plainOk ? "yes" : "no", switchOk ? "yes" : "no");
                check(plainOk, "an ordinary control is automatable");
                check(!switchOk, "…and one that reconfigures the plugin says it is not");
                check((switched.flags &
                       Steinberg_Vst_ParameterInfo_ParameterFlags_kIsList) != 0,
                      "…without losing the flags it still deserves");
              }

              // ── A stepped control says what its steps ARE ───────────
              //
              // Without names a stepped parameter renders as its index in the
              // host's generic editor and its automation lane -- which is
              // exactly where somebody works on a control whose own face is
              // closed.
              {
                auto readAt = [&](double normalised) {
                  Steinberg_Vst_String128 wide{};
                  controller->lpVtbl->getParamStringByValue(controller, switchId, normalised,
                                                            wide);
                  std::string narrow;
                  for (int k = 0; k < 128 && wide[k]; ++k) narrow.push_back((char) wide[k]);
                  return narrow;
                };
                const std::string low = readAt(0.0), high = readAt(1.0);
                std::printf("  ---- stepped values read \"%s\" and \"%s\" ----\n", low.c_str(),
                            high.c_str());
                check(low == "Short" && high == "Long",
                      "a stepped parameter renders the NAME of its step, not its index");

                // And back: every "type a value" box hands the string it read
                // straight back, and a plugin that cannot parse its own output
                // leaves the control where it was and says nothing.
                Steinberg_Vst_TChar typed[8] = {'L', 'o', 'n', 'g', 0};
                Steinberg_Vst_ParamValue parsed = -1.0;
                check(controller->lpVtbl->getParamValueByString(controller, switchId, typed,
                                                                &parsed) == Steinberg_kResultOk,
                      "…and a name typed back in is understood");
                check(parsed == 1.0, "…as the step it names");
              }

              // ── State the host cannot see ──────────────────────
              //
              // A host knows about anything it did itself. It has no idea
              // about a sampler that loaded a file through its own browser --
              // no parameter moved and nothing the host did caused it. If
              // nobody says so the session is never marked dirty, the DAW
              // closes without asking, and the work is gone.
              //
              // setDirty lives on IComponentHandler2, which a host offers or
              // does not; the plugin has to QUERY for it.
              {
                controller->lpVtbl->getParamNormalized(controller, 0); // drain
                handler.dirtyMarks = 0;

                controller->lpVtbl->setParamNormalized(controller, switchId, 1.0);
                {
                  ValueQueue q;
                  q.lpVtbl = &g_vqVtbl;
                  q.id = switchId;
                  q.value = 1.0;
                  queues.push_back(q);
                }
                processor->lpVtbl->process(processor, &data);
                queues.clear();
                check(handler.dirtyMarks == 0,
                      "process() does not mark the session dirty from the audio thread");

                controller->lpVtbl->getParamNormalized(controller, 0);
                std::printf("  ---- state dirty: %d mark(s) after the poll ----\n",
                            handler.dirtyMarks);
                check(handler.dirtyMarks > 0, "…the parameter poll delivers it");

                // Once per change, not once per poll. A host marked dirty on
                // every poll is a host that can never be clean.
                handler.dirtyMarks = 0;
                for (int i = 0; i < 10; ++i)
                  controller->lpVtbl->getParamNormalized(controller, 0);
                check(handler.dirtyMarks == 0, "…once per change, not once per poll");

                controller->lpVtbl->setParamNormalized(controller, switchId, 0.0);
                controller->lpVtbl->getParamNormalized(controller, 0);
              }

              // And nothing is announced when nothing moved. A host told its
              // graph is stale on every poll rebuilds it on every poll.
              handler.latencyRestarts = 0;
              for (int i = 0; i < 10; ++i)
                controller->lpVtbl->getParamNormalized(controller, 0);
              check(handler.latencyRestarts == 0,
                    "and polling a plugin that has not changed announces nothing");
            }
          }
          check(handler.begins > 0 && handler.ends > 0,
                "…wrapped in a gesture, so the drag is one undo step");
        }

        check(view->lpVtbl->removed(view) == Steinberg_kResultOk, "the view detaches");
        DestroyWindow(parent);
      }
#endif
      view->lpVtbl->release(view);
      check(true, "the view releases cleanly");
    }
  }

  // ── Teardown ──────────────────────────────────────────────────────────────
  processor->lpVtbl->setProcessing(processor, 0);
  component->lpVtbl->setActive(component, 0);
  check(component->lpVtbl->setActive(component, 1) == Steinberg_kResultOk,
        "the component re-activates");
  component->lpVtbl->setActive(component, 0);
  // ── Channel flexibility, audibly ──────────────────────────────────────────
  // Renegotiate while INACTIVE, reactivate, and unity must pass every channel
  // through exactly -- distinct per-channel signals catch a wrapper that only
  // wires channel 0.
  if (expectChanMin > 0) {
    // Earlier sections deliberately moved parameters around (automation,
    // state). Unity needs the DEFAULTS back.
    for (Steinberg_int32 i = 0; i < numParams; ++i) {
      Steinberg_Vst_ParameterInfo pi{};
      if (controller->lpVtbl->getParameterInfo(controller, i, &pi) == Steinberg_kResultOk)
        controller->lpVtbl->setParamNormalized(controller, pi.id, pi.defaultNormalizedValue);
    }
    for (int width : {expectChanMin, expectChanMax}) {
      component->lpVtbl->setActive(component, 0);
      Steinberg_Vst_SpeakerArrangement arr = 0;
      for (int b = 0; b < width; ++b) arr |= (Steinberg_Vst_SpeakerArrangement) 1 << b;
      Steinberg_Vst_SpeakerArrangement arrPair[2] = {arr, arr};
      check(processor->lpVtbl->setBusArrangements(processor, arrPair, wantIns, &arr,
                                                 wantOuts) == Steinberg_kResultOk,
            "the width negotiates while inactive");
      component->lpVtbl->setActive(component, 1);

      std::vector<std::vector<float>> inCh((size_t) width), outCh((size_t) width);
      std::vector<float*> inP((size_t) width), outP((size_t) width);
      for (int c = 0; c < width; ++c) {
        inCh[(size_t) c].assign((size_t) blockSize, 0.1f * (float) (c + 1));
        outCh[(size_t) c].assign((size_t) blockSize, -1.0f);
        inP[(size_t) c] = inCh[(size_t) c].data();
        outP[(size_t) c] = outCh[(size_t) c].data();
      }
      Steinberg_Vst_AudioBusBuffers inW{}, outW{};
      inW.numChannels = width;
      inW.Steinberg_Vst_AudioBusBuffers_channelBuffers32 = inP.data();
      outW.numChannels = width;
      outW.Steinberg_Vst_AudioBusBuffers_channelBuffers32 = outP.data();
      Steinberg_Vst_ProcessData d2{};
      d2.processMode = 0;
      d2.symbolicSampleSize = Steinberg_Vst_SymbolicSampleSizes_kSample32;
      d2.numSamples = blockSize;
      d2.numInputs = 1;
      d2.numOutputs = 1;
      d2.inputs = &inW;
      d2.outputs = &outW;
      processor->lpVtbl->process(processor, &d2);

      double worst = 0.0;
      for (int c = 0; c < width; ++c)
        for (Steinberg_int32 i = 0; i < blockSize; ++i) {
          const double err = std::fabs((double) outCh[(size_t) c][(size_t) i] -
                                       (double) inCh[(size_t) c][(size_t) i]);
          if (err > worst) worst = err;
        }
      char what[96];
      std::snprintf(what, sizeof(what), "unity passes all %d channels through exactly",
                    width);
      check(worst < 1e-6, what);
    }
    component->lpVtbl->setActive(component, 0);
  }

  component->lpVtbl->terminate(component);

  controller->lpVtbl->release(controller);
  processor->lpVtbl->release(processor);
  component->lpVtbl->release(component);

#if defined(_WIN32)
  if (auto exitFn = (bool (*)()) symbolOf(lib, "ExitDll")) exitFn();
#elif defined(__APPLE__)
  if (auto exitFn = (bool (*)()) symbolOf(lib, "bundleExit")) exitFn();
#else
  if (auto exitFn = (bool (*)()) symbolOf(lib, "ModuleExit")) exitFn();
#endif
  closeLib(lib);

  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  if (g_failures == 0) std::printf("SONORE VST3 HOST TEST PASSED\n");
  return g_failures == 0 ? 0 : 1;
}
