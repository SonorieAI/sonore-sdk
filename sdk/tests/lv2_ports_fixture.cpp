// SPDX-License-Identifier: Apache-2.0
// Sonore SDK test fixture: an LV2 plugin that checks what its host gave it.
//
// Two of the core spec's MUSTs, which the SDK's own LV2 wrapper never leans
// on and so could not show:
//
//   - instantiate()'s bundle_path "MUST include the trailing directory
//     separator so that simply appending a filename will yield the path to
//     that file in the bundle". sonore/host.h passed it without one.
//   - every port must be connected before run() unless it is declared
//     lv2:connectionOptional. host.h connected control, audio and atom ports
//     and skipped the rest, so a CV port was left null.
//
// And one SHOULD: urid:map may be called from more than one thread -- a
// plugin's worker, its UI -- and jalv, the reference host, locks it. host.h's
// was a bare std::map. instantiate() maps from four threads at once and
// checks every answer.
//
// It records the bundle path it was given, every run() that finds a port
// unconnected, and every inconsistent mapping, in the process environment it
// shares with the test.
//
// A second plugin in the same binary, urn:sonore:test:lv2-atoms, has a MIDI
// atom input, a MIDI atom output and a "mode" control, and misbehaves on
// request the way other people's plugins do. The test writes its TTL into a
// bundle of its own, beside a copy of this binary, so the build's bundle
// stays as it is. Mode 0 echoes its input and counts every event timed at
// or past the end of the block; mode 1 writes nothing to its output; mode 2
// writes one event and then claims a sequence two gigabytes long. Its fourth
// port, a control OUTPUT, reports 0.25 + mode / 8 every block -- a meter, as
// far as anyone outside can tell -- for a bundle that declares it, and 1e10
// in mode 3.
//
// And an interface for it, urn:sonore:test:lv2-atoms#ui, in the same binary:
// it draws nothing and records every float port_event the host sends it, as
// SONORE_LV2_UI_PORT<index>, in thousandths.
//
// A third plugin, urn:sonore:test:lv2-echo, and its interface talk in atoms,
// as a plugin with a file browser or a state view does. Port 0 is an atom
// input and port 1 the notify output, both carrying urn:sonore:test:echo#
// message; ports 2 and 3 are MIDI in and out. The plugin hands every message
// that arrives on port 0 back out on port 1, and every MIDI event on port 3.
// Its interface, on each idle(), writes as many messages to port 0 as
// SONORE_LV2_ECHO_SEND asks, SONORE_LV2_ECHO_BYTES long, numbered from 1 --
// through write_function, as atom:eventTransfer -- and records the ones that
// come back through port_event. A message's body is its number, its length
// and a pattern made from the number, so each end checks it arrived whole.
// With SONORE_LV2_ECHO_CLOSE set, its next idle() answers 1 -- "the UI has
// been closed" -- and it counts every idle() and port_event the host makes
// on it after that, and every cleanup().
//
// A fourth, urn:sonore:test:lv2-options, is built the way the DPF framework
// builds its plugins: no instance without options:options, and its block
// size and rate read from them. It records what the host gave it.
//
// A fifth, urn:sonore:test:lv2-worker, does its slow work as LV2 samplers
// do: no instance without worker:schedule, a job from every run(), and the
// answers taken back in order in work_response().
//
// A sixth, urn:sonore:test:lv2-state, counts every time its state restore()
// meets its run() or its work() on another thread, which the state
// extension forbids.

#include <lv2/atom/atom.h>
#include <lv2/core/lv2.h>
#include <lv2/midi/midi.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

constexpr int kPorts = 5; // audio in, audio out, CV in, CV out, control in

void setVar(const char* name, const char* value) {
#if defined(_WIN32)
  SetEnvironmentVariableA(name, value);
#else
  setenv(name, value, 1);
#endif
}

int readVar(const char* name) {
  char text[32] = {0};
#if defined(_WIN32)
  GetEnvironmentVariableA(name, text, (DWORD) sizeof(text));
#else
  if (const char* v = std::getenv(name)) std::snprintf(text, sizeof(text), "%s", v);
#endif
  return std::atoi(text);
}

struct Fixture {
  float* ports[kPorts] = {};
};

/** Four threads map overlapping sets of URIs at once; each id must be
 *  nonzero, the same for the same URI, and different for different ones. */
int mapFromThreads(const LV2_URID_Map* map) {
  constexpr int kThreads = 4, kUris = 3000;
  std::vector<std::vector<LV2_URID>> ids(kThreads, std::vector<LV2_URID>(kUris, 0));
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      char uri[64];
      for (int i = 0; i < kUris; ++i) {
        const int which = (i * 7 + t * 13) % kUris; // the threads overlap in a different order
        std::snprintf(uri, sizeof(uri), "urn:sonore:test:uri:%d", which);
        ids[(size_t) t][(size_t) which] = map->map(map->handle, uri);
      }
    });
  }
  for (auto& th : threads) th.join();
  int bad = 0;
  std::vector<LV2_URID> seen;
  for (int i = 0; i < kUris; ++i) {
    const LV2_URID id = ids[0][(size_t) i];
    for (int t = 1; t < kThreads; ++t)
      if (ids[(size_t) t][(size_t) i] != id) ++bad;
    if (id == 0) ++bad;
    seen.push_back(id);
  }
  std::vector<LV2_URID> sorted = seen;
  std::sort(sorted.begin(), sorted.end());
  for (size_t i = 1; i < sorted.size(); ++i)
    if (sorted[i] == sorted[i - 1]) ++bad;
  return bad;
}

LV2_Handle instantiate(const LV2_Descriptor*, double, const char* bundlePath,
                       const LV2_Feature* const* features) {
  setVar("SONORE_LV2_FIXTURE_BUNDLE", bundlePath ? bundlePath : "(null)");
  const LV2_URID_Map* map = nullptr;
  for (int i = 0; features && features[i]; ++i)
    if (std::strcmp(features[i]->URI, LV2_URID__map) == 0)
      map = static_cast<const LV2_URID_Map*>(features[i]->data);
  char text[32];
  std::snprintf(text, sizeof(text), "%d", map ? mapFromThreads(map) : -1);
  setVar("SONORE_LV2_FIXTURE_MAP_ERRORS", text);
  return new Fixture();
}

void connectPort(LV2_Handle h, uint32_t port, void* data) {
  if (port < (uint32_t) kPorts) static_cast<Fixture*>(h)->ports[port] = static_cast<float*>(data);
}

void run(LV2_Handle h, uint32_t frames) {
  auto* f = static_cast<Fixture*>(h);
  int missing = 0;
  for (int p = 0; p < kPorts; ++p)
    if (!f->ports[p]) ++missing;
  if (missing) {
    char text[32];
    std::snprintf(text, sizeof(text), "%d", readVar("SONORE_LV2_FIXTURE_UNCONNECTED") + missing);
    setVar("SONORE_LV2_FIXTURE_UNCONNECTED", text);
    return;
  }
  // A real plugin reads and writes every one of them, CV included.
  for (uint32_t i = 0; i < frames; ++i) {
    f->ports[1][i] = f->ports[0][i] * (1.0f + f->ports[2][i]) * *f->ports[4];
    f->ports[3][i] = f->ports[2][i];
  }
}

void cleanup(LV2_Handle h) { delete static_cast<Fixture*>(h); }

const LV2_Descriptor kDescriptor = {
    "urn:sonore:test:lv2-ports", instantiate, connectPort, nullptr, run, nullptr, cleanup, nullptr,
};

// ── urn:sonore:test:lv2-atoms ───────────────────────────────────────────────

struct Atoms {
  const LV2_Atom_Sequence* in = nullptr; // port 0
  LV2_Atom_Sequence* out = nullptr;      // port 1
  const float* mode = nullptr;           // port 2
  float* level = nullptr;                // port 3, where a bundle declares it
  LV2_URID sequence = 0, midiEvent = 0;
};

LV2_Handle instantiateAtoms(const LV2_Descriptor*, double, const char*,
                            const LV2_Feature* const* features) {
  const LV2_URID_Map* map = nullptr;
  for (int i = 0; features && features[i]; ++i)
    if (std::strcmp(features[i]->URI, LV2_URID__map) == 0)
      map = static_cast<const LV2_URID_Map*>(features[i]->data);
  if (!map) return nullptr;
  auto* a = new Atoms();
  a->sequence = map->map(map->handle, LV2_ATOM__Sequence);
  a->midiEvent = map->map(map->handle, LV2_MIDI__MidiEvent);
  return a;
}

void connectAtoms(LV2_Handle h, uint32_t port, void* data) {
  auto* a = static_cast<Atoms*>(h);
  if (port == 0) a->in = static_cast<const LV2_Atom_Sequence*>(data);
  if (port == 1) a->out = static_cast<LV2_Atom_Sequence*>(data);
  if (port == 2) a->mode = static_cast<const float*>(data);
  if (port == 3) a->level = static_cast<float*>(data);
}

void runAtoms(LV2_Handle h, uint32_t frames) {
  auto* a = static_cast<Atoms*>(h);
  if (!a->in || !a->out || !a->mode) return;
  const int mode = (int) *a->mode;
  // Mode 3 reports 1e10, which no uint32 holds: a bundle that names this
  // port "latency" has a plugin reporting a latency of no number of samples.
  if (a->level) *a->level = mode == 3 ? 1.0e10f : 0.25f + 0.125f * (float) mode;
  if (mode == 1) return; // nothing written: the host must not read last block's events again

  const uint32_t capacity = a->out->atom.size;
  a->out->atom.type = a->sequence;
  a->out->atom.size = (uint32_t) sizeof(LV2_Atom_Sequence_Body);
  a->out->body.unit = 0;
  a->out->body.pad = 0;
  uint8_t* outBody = reinterpret_cast<uint8_t*>(&a->out->body);
  auto append = [&](int64_t time, const uint8_t* bytes, uint32_t size) {
    const uint32_t need = (uint32_t) sizeof(LV2_Atom_Event) + ((size + 7u) & ~7u);
    if (a->out->atom.size + need > capacity) return;
    auto* ev = reinterpret_cast<LV2_Atom_Event*>(outBody + a->out->atom.size);
    ev->time.frames = time;
    ev->body.type = a->midiEvent;
    ev->body.size = size;
    std::memcpy(reinterpret_cast<uint8_t*>(ev) + sizeof(LV2_Atom_Event), bytes, size);
    a->out->atom.size += need;
  };

  int late = 0;
  const uint8_t* inBody = reinterpret_cast<const uint8_t*>(&a->in->body);
  for (uint32_t offset = (uint32_t) sizeof(LV2_Atom_Sequence_Body);
       offset + sizeof(LV2_Atom_Event) <= a->in->atom.size;) {
    const auto* ev = reinterpret_cast<const LV2_Atom_Event*>(inBody + offset);
    if (ev->time.frames < 0 || ev->time.frames >= (int64_t) frames) ++late;
    if (mode == 0 && ev->body.type == a->midiEvent)
      append(ev->time.frames, reinterpret_cast<const uint8_t*>(ev) + sizeof(LV2_Atom_Event),
             ev->body.size);
    offset += (uint32_t) sizeof(LV2_Atom_Event) + ((ev->body.size + 7u) & ~7u);
  }
  if (late) {
    char text[32];
    std::snprintf(text, sizeof(text), "%d", readVar("SONORE_LV2_FIXTURE_LATE_EVENTS") + late);
    setVar("SONORE_LV2_FIXTURE_LATE_EVENTS", text);
  }

  if (mode == 2) {
    // One honest event, then a size no buffer a host gave is that long: a
    // plugin's bug, and the host's to survive.
    const uint8_t on[3] = {0x90, 64, 100};
    append(0, on, 3);
    a->out->atom.size = 0x7FFFFFF0u;
  }
}

void cleanupAtoms(LV2_Handle h) { delete static_cast<Atoms*>(h); }

const LV2_Descriptor kAtomsDescriptor = {
    "urn:sonore:test:lv2-atoms", instantiateAtoms, connectAtoms, nullptr, runAtoms, nullptr,
    cleanupAtoms, nullptr,
};

// ── urn:sonore:test:lv2-atoms#ui ────────────────────────────────────────────
//
// The UI ABI spelled out here, as a host spells it: the extension's header is
// not part of the core package this SDK carries.

struct UiDescriptor {
  const char* URI;
  void* (*instantiate)(const UiDescriptor*, const char*, const char*,
                       void (*)(void*, uint32_t, uint32_t, uint32_t, const void*), void*, void**,
                       const LV2_Feature* const*);
  void (*cleanup)(void*);
  void (*port_event)(void*, uint32_t, uint32_t, uint32_t, const void*);
  const void* (*extension_data)(const char*);
};

void* uiInstantiate(const UiDescriptor*, const char*, const char*,
                    void (*)(void*, uint32_t, uint32_t, uint32_t, const void*), void*,
                    void** widget, const LV2_Feature* const*) {
  if (widget) *widget = nullptr; // nothing drawn, nothing to embed
  return new int(0);
}
void uiCleanup(void* ui) { delete static_cast<int*>(ui); }
void uiPortEvent(void*, uint32_t port, uint32_t size, uint32_t format, const void* buffer) {
  if (format != 0 || size != sizeof(float) || !buffer) return;
  char name[48], text[32];
  std::snprintf(name, sizeof(name), "SONORE_LV2_UI_PORT%u", (unsigned) port);
  std::snprintf(text, sizeof(text), "%d", (int) (*static_cast<const float*>(buffer) * 1000.0f + 0.5f));
  setVar(name, text);
}
const void* uiExtension(const char*) { return nullptr; }

const UiDescriptor kUiDescriptor = {"urn:sonore:test:lv2-atoms#ui", uiInstantiate, uiCleanup,
                                    uiPortEvent, uiExtension};

// ── urn:sonore:test:lv2-echo ────────────────────────────────────────────────

const char* const kEchoMessage = "urn:sonore:test:echo#message";
constexpr uint32_t kEchoHeader = 8; // the number, then the length, before the pattern

uint8_t echoByte(uint32_t number, uint32_t i) { return (uint8_t) (number * 31u + i * 7u + 1u); }

/** A message's body: `length` bytes, its number and length first. */
void fillEcho(uint8_t* body, uint32_t number, uint32_t length) {
  std::memcpy(body, &number, 4);
  std::memcpy(body + 4, &length, 4);
  for (uint32_t i = kEchoHeader; i < length; ++i) body[i] = echoByte(number, i);
}
/** Its number if `body` is a whole message of `size` bytes, else 0. */
uint32_t checkEcho(const uint8_t* body, uint32_t size) {
  if (size < kEchoHeader) return 0;
  uint32_t number = 0, length = 0;
  std::memcpy(&number, body, 4);
  std::memcpy(&length, body + 4, 4);
  if (length != size || number == 0) return 0;
  for (uint32_t i = kEchoHeader; i < length; ++i)
    if (body[i] != echoByte(number, i)) return 0;
  return number;
}

void addVar(const char* name, int by) {
  char text[32];
  std::snprintf(text, sizeof(text), "%d", readVar(name) + by);
  setVar(name, text);
}

const LV2_URID_Map* findMap(const LV2_Feature* const* features) {
  for (int i = 0; features && features[i]; ++i)
    if (std::strcmp(features[i]->URI, LV2_URID__map) == 0)
      return static_cast<const LV2_URID_Map*>(features[i]->data);
  return nullptr;
}

struct Echo {
  const LV2_Atom_Sequence* control = nullptr; // port 0
  LV2_Atom_Sequence* notify = nullptr;        // port 1
  const LV2_Atom_Sequence* midiIn = nullptr;  // port 2
  LV2_Atom_Sequence* midiOut = nullptr;       // port 3
  LV2_URID sequence = 0, midiEvent = 0, message = 0;
};

LV2_Handle instantiateEcho(const LV2_Descriptor*, double, const char*,
                           const LV2_Feature* const* features) {
  const LV2_URID_Map* map = findMap(features);
  if (!map) return nullptr;
  auto* e = new Echo();
  e->sequence = map->map(map->handle, LV2_ATOM__Sequence);
  e->midiEvent = map->map(map->handle, LV2_MIDI__MidiEvent);
  e->message = map->map(map->handle, kEchoMessage);
  return e;
}

void connectEcho(LV2_Handle h, uint32_t port, void* data) {
  auto* e = static_cast<Echo*>(h);
  if (port == 0) e->control = static_cast<const LV2_Atom_Sequence*>(data);
  if (port == 1) e->notify = static_cast<LV2_Atom_Sequence*>(data);
  if (port == 2) e->midiIn = static_cast<const LV2_Atom_Sequence*>(data);
  if (port == 3) e->midiOut = static_cast<LV2_Atom_Sequence*>(data);
}

/** Every event of `in` whose type is `type`, appended to `out` at its time,
 *  `out` claimed first: a host reads back what the plugin wrote. Messages
 *  are checked and counted on the way. */
void echoEvents(const Echo* e, const LV2_Atom_Sequence* in, LV2_Atom_Sequence* out, LV2_URID type,
                uint32_t frames) {
  const uint32_t capacity = out->atom.size;
  out->atom.type = e->sequence;
  out->atom.size = (uint32_t) sizeof(LV2_Atom_Sequence_Body);
  out->body.unit = 0;
  out->body.pad = 0;
  if (in->atom.type != e->sequence) return;
  const uint8_t* inBody = reinterpret_cast<const uint8_t*>(&in->body);
  uint8_t* outBody = reinterpret_cast<uint8_t*>(&out->body);
  for (uint32_t offset = (uint32_t) sizeof(LV2_Atom_Sequence_Body);
       offset + sizeof(LV2_Atom_Event) <= in->atom.size;) {
    const auto* ev = reinterpret_cast<const LV2_Atom_Event*>(inBody + offset);
    const uint32_t need = (uint32_t) sizeof(LV2_Atom_Event) + ((ev->body.size + 7u) & ~7u);
    if (ev->time.frames < 0 || ev->time.frames >= (int64_t) frames) addVar("SONORE_LV2_ECHO_LATE", 1);
    if (ev->body.type == type) {
      if (type == e->message) {
        addVar("SONORE_LV2_ECHO_HEARD", 1);
        if (checkEcho(reinterpret_cast<const uint8_t*>(ev) + sizeof(LV2_Atom_Event), ev->body.size))
          addVar("SONORE_LV2_ECHO_INTACT", 1);
      }
      if (out->atom.size + need <= capacity) {
        std::memcpy(outBody + out->atom.size, ev, sizeof(LV2_Atom_Event) + ev->body.size);
        out->atom.size += need;
      } else {
        addVar("SONORE_LV2_ECHO_NO_ROOM", 1);
      }
    }
    offset += need;
  }
}

void runEcho(LV2_Handle h, uint32_t frames) {
  auto* e = static_cast<Echo*>(h);
  if (!e->control || !e->notify || !e->midiIn || !e->midiOut) {
    addVar("SONORE_LV2_ECHO_UNCONNECTED", 1);
    return;
  }
  echoEvents(e, e->control, e->notify, e->message, frames);
  echoEvents(e, e->midiIn, e->midiOut, e->midiEvent, frames);
}

void cleanupEcho(LV2_Handle h) { delete static_cast<Echo*>(h); }

const LV2_Descriptor kEchoDescriptor = {
    "urn:sonore:test:lv2-echo", instantiateEcho, connectEcho, nullptr, runEcho, nullptr,
    cleanupEcho, nullptr,
};

// ── urn:sonore:test:lv2-options ─────────────────────────────────────────────
//
// A plugin built the way the DPF framework builds them -- a large share of
// the LV2 plugins there are: it refuses to instantiate without options:options
// and reads its block size and rate from the options. It records what it was
// given: SONORE_LV2_OPTIONS_MAX and _MIN (block lengths), _RATE (Hz),
// _SEQUENCE (bufsz:sequenceSize), _BOUNDED (bufsz:boundedBlockLength was
// there) and _UNMAP (urid:unmap gave the map's URIs back). The option struct
// is declared here, as options.h has it: the header is not vendored.

struct OptionsOption {
  uint32_t context;
  uint32_t subject;
  LV2_URID key;
  uint32_t size;
  LV2_URID type;
  const void* value;
};

struct OptionsPlugin {
  const float* in = nullptr;
  float* out = nullptr;
};

LV2_Handle instantiateOptions(const LV2_Descriptor*, double, const char*, const LV2_Feature* const* features) {
  const LV2_URID_Map* map = nullptr;
  const LV2_URID_Unmap* unmap = nullptr;
  const OptionsOption* options = nullptr;
  bool bounded = false;
  for (int i = 0; features && features[i]; ++i) {
    const char* uri = features[i]->URI;
    if (std::strcmp(uri, LV2_URID__map) == 0) map = static_cast<const LV2_URID_Map*>(features[i]->data);
    if (std::strcmp(uri, LV2_URID__unmap) == 0)
      unmap = static_cast<const LV2_URID_Unmap*>(features[i]->data);
    if (std::strcmp(uri, "http://lv2plug.in/ns/ext/options#options") == 0)
      options = static_cast<const OptionsOption*>(features[i]->data);
    if (std::strcmp(uri, "http://lv2plug.in/ns/ext/buf-size#boundedBlockLength") == 0) bounded = true;
  }
  if (!map || !options) return nullptr; // "Options feature missing, cannot continue!"
  const LV2_URID atomInt = map->map(map->handle, LV2_ATOM__Int);
  const LV2_URID atomFloat = map->map(map->handle, LV2_ATOM__Float);
  const LV2_URID maxKey = map->map(map->handle, "http://lv2plug.in/ns/ext/buf-size#maxBlockLength");
  const LV2_URID minKey = map->map(map->handle, "http://lv2plug.in/ns/ext/buf-size#minBlockLength");
  const LV2_URID seqKey = map->map(map->handle, "http://lv2plug.in/ns/ext/buf-size#sequenceSize");
  const LV2_URID rateKey = map->map(map->handle, "http://lv2plug.in/ns/ext/parameters#sampleRate");
  char text[32];
  for (const OptionsOption* o = options; o->key != 0 || o->value != nullptr; ++o) {
    const char* name = o->key == maxKey   ? "SONORE_LV2_OPTIONS_MAX"
                       : o->key == minKey ? "SONORE_LV2_OPTIONS_MIN"
                       : o->key == seqKey ? "SONORE_LV2_OPTIONS_SEQUENCE"
                       : o->key == rateKey ? "SONORE_LV2_OPTIONS_RATE"
                                           : nullptr;
    if (!name || !o->value) continue;
    if (o->type == atomInt && o->size == sizeof(int32_t))
      std::snprintf(text, sizeof(text), "%d", (int) *static_cast<const int32_t*>(o->value));
    else if (o->type == atomFloat && o->size == sizeof(float))
      std::snprintf(text, sizeof(text), "%d", (int) *static_cast<const float*>(o->value));
    else
      continue;
    setVar(name, text);
  }
  setVar("SONORE_LV2_OPTIONS_BOUNDED", bounded ? "1" : "0");
  const LV2_URID probe = map->map(map->handle, "urn:sonore:test:lv2-options#probe");
  const char* back = unmap ? unmap->unmap(unmap->handle, probe) : nullptr;
  setVar("SONORE_LV2_OPTIONS_UNMAP",
         back && std::strcmp(back, "urn:sonore:test:lv2-options#probe") == 0 ? "1" : "0");
  return new OptionsPlugin();
}

void connectOptions(LV2_Handle h, uint32_t port, void* data) {
  auto* p = static_cast<OptionsPlugin*>(h);
  if (port == 0) p->in = static_cast<const float*>(data);
  if (port == 1) p->out = static_cast<float*>(data);
}

void runOptions(LV2_Handle h, uint32_t frames) {
  auto* p = static_cast<OptionsPlugin*>(h);
  if (p->in && p->out)
    for (uint32_t i = 0; i < frames; ++i) p->out[i] = p->in[i];
}

void cleanupOptions(LV2_Handle h) { delete static_cast<OptionsPlugin*>(h); }

const LV2_Descriptor kOptionsDescriptor = {
    "urn:sonore:test:lv2-options", instantiateOptions, connectOptions, nullptr, runOptions, nullptr,
    cleanupOptions, nullptr,
};

// ── urn:sonore:test:lv2-worker ──────────────────────────────────────────────
//
// A plugin that does its slow work the way LV2 samplers do: no instance
// without worker:schedule, a job scheduled from run(), done on the host's
// worker, and the answer taken back in work_response() before the next
// run(). Each run() schedules job N (a 4-byte number); work() answers N + 1000.
// It records SONORE_LV2_WORKER_RESPONSES (answers taken, which must arrive
// in order), _BAD (an answer out of order or wrong), _OFF_THREAD (work() ran
// on a thread that is not run()'s), _END_RUNS (end_run() calls) and _RUNS.
// The worker ABI is declared here, as lv2_host_test does.

using WorkStatus = uint32_t; // LV2_Worker_Status: 0 success, 2 no space
using WorkRespond = WorkStatus (*)(void*, uint32_t, const void*);
struct WorkSchedule {
  void* handle;
  WorkStatus (*schedule_work)(void*, uint32_t, const void*);
};
struct WorkInterface {
  WorkStatus (*work)(LV2_Handle, WorkRespond, void*, uint32_t, const void*);
  WorkStatus (*work_response)(LV2_Handle, uint32_t, const void*);
  WorkStatus (*end_run)(LV2_Handle);
};

struct WorkerPlugin {
  const WorkSchedule* schedule = nullptr;
  const float* in = nullptr;
  float* out = nullptr;
  uint32_t scheduled = 0, answered = 0;
  int endRuns = 0;
  std::thread::id runThread;
};

LV2_Handle instantiateWorker(const LV2_Descriptor*, double, const char*, const LV2_Feature* const* features) {
  const WorkSchedule* schedule = nullptr;
  for (int i = 0; features && features[i]; ++i)
    if (std::strcmp(features[i]->URI, "http://lv2plug.in/ns/ext/worker#schedule") == 0)
      schedule = static_cast<const WorkSchedule*>(features[i]->data);
  if (!schedule) return nullptr;
  for (const char* name : {"SONORE_LV2_WORKER_RESPONSES", "SONORE_LV2_WORKER_BAD",
                           "SONORE_LV2_WORKER_OFF_THREAD", "SONORE_LV2_WORKER_END_RUNS",
                           "SONORE_LV2_WORKER_RUNS"})
    setVar(name, "0");
  auto* p = new WorkerPlugin();
  p->schedule = schedule;
  return p;
}

void connectWorker(LV2_Handle h, uint32_t port, void* data) {
  auto* p = static_cast<WorkerPlugin*>(h);
  if (port == 0) p->in = static_cast<const float*>(data);
  if (port == 1) p->out = static_cast<float*>(data);
}

void setCount(const char* name, int value) {
  char text[32];
  std::snprintf(text, sizeof(text), "%d", value);
  setVar(name, text);
}

void runWorker(LV2_Handle h, uint32_t frames) {
  auto* p = static_cast<WorkerPlugin*>(h);
  p->runThread = std::this_thread::get_id();
  if (p->in && p->out)
    for (uint32_t i = 0; i < frames; ++i) p->out[i] = p->in[i];
  const uint32_t job = ++p->scheduled;
  p->schedule->schedule_work(p->schedule->handle, sizeof(job), &job);
  setCount("SONORE_LV2_WORKER_RUNS", (int) p->scheduled);
}

WorkStatus workWorker(LV2_Handle h, WorkRespond respond, void* handle, uint32_t size, const void* data) {
  auto* p = static_cast<WorkerPlugin*>(h);
  if (std::this_thread::get_id() != p->runThread) setVar("SONORE_LV2_WORKER_OFF_THREAD", "1");
  uint32_t job = 0;
  if (size == sizeof(job)) std::memcpy(&job, data, sizeof(job));
  const uint32_t answer = job + 1000;
  return respond(handle, sizeof(answer), &answer);
}

WorkStatus workResponse(LV2_Handle h, uint32_t size, const void* data) {
  auto* p = static_cast<WorkerPlugin*>(h);
  uint32_t answer = 0;
  if (size == sizeof(answer)) std::memcpy(&answer, data, sizeof(answer));
  if (answer != p->answered + 1 + 1000) setCount("SONORE_LV2_WORKER_BAD", readVar("SONORE_LV2_WORKER_BAD") + 1);
  ++p->answered;
  setCount("SONORE_LV2_WORKER_RESPONSES", (int) p->answered);
  return 0;
}

WorkStatus endRunWorker(LV2_Handle h) {
  auto* p = static_cast<WorkerPlugin*>(h);
  setCount("SONORE_LV2_WORKER_END_RUNS", ++p->endRuns);
  return 0;
}

const WorkInterface kWorkInterface = {workWorker, workResponse, endRunWorker};

const void* workerExtension(const char* uri) {
  return std::strcmp(uri, "http://lv2plug.in/ns/ext/worker#interface") == 0 ? &kWorkInterface : nullptr;
}

void cleanupWorker(LV2_Handle h) { delete static_cast<WorkerPlugin*>(h); }

const LV2_Descriptor kWorkerDescriptor = {
    "urn:sonore:test:lv2-worker", instantiateWorker, connectWorker, nullptr, runWorker, nullptr,
    cleanupWorker, workerExtension,
};

// ── urn:sonore:test:lv2-state ───────────────────────────────────────────────
//
// A plugin whose state restore must have the instance to itself, as the
// state extension says of restore(): it "MUST NOT be called concurrently
// with any other function on the same plugin instance". This one rebuilds
// in restore() what run() reads, as a sampler swapping its sample does, and
// takes a couple of milliseconds over it. Its run() and its worker's work()
// mark themselves busy; every one of them that finds a restore() under way,
// and every restore() that finds one of them busy, is an overlap, counted in
// SONORE_LV2_STATE_OVERLAPS. SONORE_LV2_STATE_RESTORES counts restores and
// SONORE_LV2_STATE_RUNS, on cleanup, runs. It saves one property, and
// schedules a job from every run(). Its activate() and deactivate() -- which
// LV2 puts in the instantiation class, beside nothing else either -- count
// as busy too, and SONORE_LV2_STATE_ACTIVATIONS counts activations.

struct StatePlugin {
  const WorkSchedule* schedule = nullptr;
  const float* in = nullptr;
  float* out = nullptr;
  LV2_URID key = 0, type = 0;
  std::atomic<int> busy{0};      // inside run() or work()
  std::atomic<int> restoring{0}; // inside restore()
  std::atomic<int> overlaps{0};
  std::atomic<int> runs{0};
  std::atomic<int> activations{0};
  int restores = 0;
};

LV2_Handle instantiateState(const LV2_Descriptor*, double, const char*, const LV2_Feature* const* features) {
  const WorkSchedule* schedule = nullptr;
  const LV2_URID_Map* map = nullptr;
  for (int i = 0; features && features[i]; ++i) {
    if (std::strcmp(features[i]->URI, "http://lv2plug.in/ns/ext/worker#schedule") == 0)
      schedule = static_cast<const WorkSchedule*>(features[i]->data);
    if (std::strcmp(features[i]->URI, LV2_URID__map) == 0)
      map = static_cast<const LV2_URID_Map*>(features[i]->data);
  }
  if (!schedule || !map) return nullptr;
  setVar("SONORE_LV2_STATE_OVERLAPS", "0");
  setVar("SONORE_LV2_STATE_RESTORES", "0");
  auto* p = new StatePlugin();
  p->schedule = schedule;
  p->key = map->map(map->handle, "urn:sonore:test:lv2-state#sample");
  p->type = map->map(map->handle, LV2_ATOM__Int);
  return p;
}

void connectState(LV2_Handle h, uint32_t port, void* data) {
  auto* p = static_cast<StatePlugin*>(h);
  if (port == 0) p->in = static_cast<const float*>(data);
  if (port == 1) p->out = static_cast<float*>(data);
}

void runState(LV2_Handle h, uint32_t frames) {
  auto* p = static_cast<StatePlugin*>(h);
  p->busy.fetch_add(1);
  if (p->restoring.load()) p->overlaps.fetch_add(1);
  p->runs.fetch_add(1);
  if (p->in && p->out)
    for (uint32_t i = 0; i < frames; ++i) p->out[i] = p->in[i];
  const uint32_t job = 1;
  p->schedule->schedule_work(p->schedule->handle, sizeof(job), &job);
  if (p->restoring.load()) p->overlaps.fetch_add(1);
  p->busy.fetch_sub(1);
}

WorkStatus workState(LV2_Handle h, WorkRespond, void*, uint32_t, const void*) {
  auto* p = static_cast<StatePlugin*>(h);
  p->busy.fetch_add(1);
  if (p->restoring.load()) p->overlaps.fetch_add(1);
  std::this_thread::sleep_for(std::chrono::microseconds(200));
  if (p->restoring.load()) p->overlaps.fetch_add(1);
  p->busy.fetch_sub(1);
  return 0;
}

LV2_State_Status saveStatePlugin(LV2_Handle h, LV2_State_Store_Function store, LV2_State_Handle handle,
                                 uint32_t, const LV2_Feature* const*) {
  auto* p = static_cast<StatePlugin*>(h);
  const int32_t sample = 7;
  return store(handle, p->key, &sample, sizeof(sample), p->type,
               LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
}

LV2_State_Status restoreStatePlugin(LV2_Handle h, LV2_State_Retrieve_Function retrieve,
                                    LV2_State_Handle handle, uint32_t, const LV2_Feature* const*) {
  auto* p = static_cast<StatePlugin*>(h);
  p->restoring.store(1);
  if (p->busy.load()) p->overlaps.fetch_add(1);
  size_t size = 0;
  uint32_t type = 0, flags = 0;
  retrieve(handle, p->key, &size, &type, &flags);
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  if (p->busy.load()) p->overlaps.fetch_add(1);
  p->restoring.store(0);
  setCount("SONORE_LV2_STATE_RESTORES", ++p->restores);
  setCount("SONORE_LV2_STATE_OVERLAPS", p->overlaps.load());
  return LV2_STATE_SUCCESS;
}

const LV2_State_Interface kStateInterface = {saveStatePlugin, restoreStatePlugin};
const WorkInterface kStateWorkInterface = {workState, nullptr, nullptr};

const void* stateExtension(const char* uri) {
  if (std::strcmp(uri, LV2_STATE__interface) == 0) return &kStateInterface;
  if (std::strcmp(uri, "http://lv2plug.in/ns/ext/worker#interface") == 0) return &kStateWorkInterface;
  return nullptr;
}

void cleanupState(LV2_Handle h) {
  auto* p = static_cast<StatePlugin*>(h);
  setCount("SONORE_LV2_STATE_OVERLAPS", p->overlaps.load());
  setCount("SONORE_LV2_STATE_RUNS", p->runs.load());
  delete p;
}

/** activate() and deactivate(): busy like run(), and an overlap if a
 *  restore() or a work() is under way -- any of them. */
void instantiationClassCall(LV2_Handle h) {
  auto* p = static_cast<StatePlugin*>(h);
  if (p->busy.fetch_add(1) != 0 || p->restoring.load()) p->overlaps.fetch_add(1);
  // A spin, not a sleep: Windows sleeps a millisecond or more for any
  // request, and that is the audio thread's time here.
  const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(50);
  while (std::chrono::steady_clock::now() < until) {
  }
  if (p->restoring.load()) p->overlaps.fetch_add(1);
  p->busy.fetch_sub(1);
}
void activateState(LV2_Handle h) {
  instantiationClassCall(h);
  setCount("SONORE_LV2_STATE_ACTIVATIONS", ++static_cast<StatePlugin*>(h)->activations);
}
void deactivateState(LV2_Handle h) { instantiationClassCall(h); }

const LV2_Descriptor kStateDescriptor = {
    "urn:sonore:test:lv2-state", instantiateState, connectState, activateState, runState,
    deactivateState, cleanupState, stateExtension,
};

// ── urn:sonore:test:lv2-echo#ui ─────────────────────────────────────────────

using UiWrite = void (*)(void*, uint32_t, uint32_t, uint32_t, const void*);

struct EchoUi {
  UiWrite write = nullptr;
  void* controller = nullptr;
  LV2_URID transfer = 0, message = 0, midiEvent = 0;
  uint32_t sent = 0, lastHeard = 0;
  /** Its idle() has said it is closed: nothing should reach it but cleanup(). */
  bool closed = false;
};

void* echoUiInstantiate(const UiDescriptor*, const char*, const char*, UiWrite write,
                        void* controller, void** widget, const LV2_Feature* const* features) {
  if (widget) *widget = nullptr;
  auto* ui = new EchoUi();
  ui->write = write;
  ui->controller = controller;
  // An interface that talks in atoms needs the host's urid:map to name
  // eventTransfer, and its own message type.
  const LV2_URID_Map* map = findMap(features);
  setVar("SONORE_LV2_ECHO_UI_MAP", map ? "1" : "0");
  if (map) {
    ui->transfer = map->map(map->handle, LV2_ATOM__eventTransfer);
    ui->message = map->map(map->handle, kEchoMessage);
    ui->midiEvent = map->map(map->handle, LV2_MIDI__MidiEvent);
  }
  return ui;
}
void echoUiCleanup(void* handle) {
  addVar("SONORE_LV2_ECHO_UI_CLEANUPS", 1);
  delete static_cast<EchoUi*>(handle);
}

/** What the host forwards from the plugin's atom outputs: one atom per
 *  event, as eventTransfer has it. Messages are checked and must come in
 *  the order they were sent; MIDI should never come at all. */
void echoUiPortEvent(void* handle, uint32_t port, uint32_t size, uint32_t format,
                     const void* buffer) {
  auto* ui = static_cast<EchoUi*>(handle);
  if (ui && ui->closed) addVar("SONORE_LV2_ECHO_EVENTS_AFTER_CLOSE", 1);
  if (!ui || format == 0 || !buffer) return;
  if (format != ui->transfer || size < sizeof(LV2_Atom)) {
    addVar("SONORE_LV2_ECHO_UI_ODD", 1);
    return;
  }
  const auto* atom = static_cast<const LV2_Atom*>(buffer);
  if (sizeof(LV2_Atom) + atom->size != size) {
    addVar("SONORE_LV2_ECHO_UI_ODD", 1);
    return;
  }
  if (atom->type == ui->midiEvent) addVar("SONORE_LV2_ECHO_UI_MIDI", 1);
  if (atom->type != ui->message || port != 1) return;
  const uint32_t number = checkEcho(reinterpret_cast<const uint8_t*>(atom + 1), atom->size);
  addVar("SONORE_LV2_ECHO_UI_GOT", 1);
  if (number == 0) return;
  addVar("SONORE_LV2_ECHO_UI_INTACT", 1);
  if (number <= ui->lastHeard) addVar("SONORE_LV2_ECHO_UI_DISORDER", 1);
  ui->lastHeard = number;
  char text[32];
  std::snprintf(text, sizeof(text), "%u", number);
  setVar("SONORE_LV2_ECHO_UI_LAST", text);
}

/** The host's tick: the messages the test asked for, written to port 0. */
int echoUiIdle(void* handle) {
  auto* ui = static_cast<EchoUi*>(handle);
  if (ui && ui->closed) {
    addVar("SONORE_LV2_ECHO_IDLE_AFTER_CLOSE", 1);
    return 1;
  }
  // Closed by its own hand -- a close box of its own, a crashed renderer --
  // which LV2 says through idle()'s answer.
  if (ui && readVar("SONORE_LV2_ECHO_CLOSE")) {
    setVar("SONORE_LV2_ECHO_CLOSE", "0");
    ui->closed = true;
    return 1;
  }
  const int count = readVar("SONORE_LV2_ECHO_SEND");
  setVar("SONORE_LV2_ECHO_SEND", "0");
  if (!ui || !ui->write || !ui->transfer) return 0;
  const int asked = readVar("SONORE_LV2_ECHO_BYTES");
  const uint32_t length = asked >= (int) kEchoHeader ? (uint32_t) asked : 40u;
  std::vector<uint8_t> atom(sizeof(LV2_Atom) + length);
  for (int k = 0; k < count; ++k) {
    auto* header = reinterpret_cast<LV2_Atom*>(atom.data());
    header->size = length;
    header->type = ui->message;
    fillEcho(atom.data() + sizeof(LV2_Atom), ++ui->sent, length);
    ui->write(ui->controller, 0, (uint32_t) atom.size(), ui->transfer, atom.data());
    // The buffer is the interface's own, and reused at once.
    std::memset(atom.data() + sizeof(LV2_Atom), 0, length);
  }
  return 0;
}
struct UiIdle {
  int (*idle)(void*);
};
const UiIdle kEchoUiIdle = {echoUiIdle};
const void* echoUiExtension(const char* uri) {
  return uri && std::strcmp(uri, "http://lv2plug.in/ns/extensions/ui#idleInterface") == 0
             ? &kEchoUiIdle
             : nullptr;
}

const UiDescriptor kEchoUiDescriptor = {"urn:sonore:test:lv2-echo#ui", echoUiInstantiate,
                                        echoUiCleanup, echoUiPortEvent, echoUiExtension};

} // namespace

// LV2_SYMBOL_EXPORT carries the extern "C" itself.
LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) {
  switch (index) {
    case 0: return &kDescriptor;
    case 1: return &kAtomsDescriptor;
    case 2: return &kEchoDescriptor;
    case 3: return &kOptionsDescriptor;
    case 4: return &kWorkerDescriptor;
    case 5: return &kStateDescriptor;
    default: return nullptr;
  }
}

LV2_SYMBOL_EXPORT const void* lv2ui_descriptor(uint32_t index) {
  return index == 0 ? &kUiDescriptor : (index == 1 ? &kEchoUiDescriptor : nullptr);
}
