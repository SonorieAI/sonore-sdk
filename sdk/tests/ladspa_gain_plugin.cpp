// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: a LADSPA library of two tiny plugins, for host.h to load.
//
// There is no LADSPA plugin on a Windows machine to test against, and the ones
// on a Linux machine are whatever the distribution shipped. So the test builds
// its own: compiled against ladspa_abi.h -- the same declarations the host
// reads -- as a .dll on Windows and a .so elsewhere, and loaded by
// plugin_state_test through LADSPAPluginFormat like any other.
//
// Two plugins, because one cannot exercise the parts of the format that
// matter:
//   0  "sonore_test_gain"   mono, a gain in dB (bounded, default 0) and a
//                           toggled phase invert. The arithmetic the test
//                           checks against.
//   1  "sonore_test_stereo" stereo pass-through with a "latency" output port
//                           reporting a fixed 64 samples, an integer control,
//                           and a corner frequency bounded as FRACTIONS of
//                           the sample rate with a logarithmic "low" default
//                           -- the hint rules a host most easily gets wrong.
//   2  "sonore_test_malformed"  only while SONORE_LADSPA_TEST_MALFORMED is set
//                           in the environment, so every scan that counts this
//                           library's plugins still sees two: a port that
//                           claims to be audio AND control, which a host must
//                           refuse rather than connect to a single float.
#include <sonore/ladspa_abi.h>

#include <cmath>
#include <cstdlib>
#include <new>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#if defined(_WIN32)
#define SONORE_LADSPA_EXPORT extern "C" __declspec(dllexport)
#else
#define SONORE_LADSPA_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

using namespace sonore::ladspa;

// ── 0: mono gain ─────────────────────────────────────────────────────────────

enum { kGainPort = 0, kInvertPort, kGainIn, kGainOut, kGainPortCount };

struct Gain {
  Data* ports[kGainPortCount] = {};
  unsigned long rate = 0;
  int activations = 0;
};

const PortDescriptor kGainDescriptors[kGainPortCount] = {
    kPortInput | kPortControl, kPortInput | kPortControl, kPortInput | kPortAudio,
    kPortOutput | kPortAudio};
const char* const kGainNames[kGainPortCount] = {"Gain (dB)", "Invert", "Input", "Output"};
const PortRangeHint kGainHints[kGainPortCount] = {
    {kHintBoundedBelow | kHintBoundedAbove | kHintDefault0, -60.0f, 24.0f},
    {kHintToggled | kHintDefault0, 0.0f, 0.0f},
    {0, 0.0f, 0.0f},
    {0, 0.0f, 0.0f}};

Handle gainInstantiate(const Descriptor*, unsigned long rate) {
  Gain* g = new (std::nothrow) Gain();
  if (g) g->rate = rate;
  return g;
}
void gainConnect(Handle h, unsigned long port, Data* data) {
  if (port < kGainPortCount) static_cast<Gain*>(h)->ports[port] = data;
}
void gainActivate(Handle h) { ++static_cast<Gain*>(h)->activations; }
void gainRun(Handle h, unsigned long n) {
  Gain* g = static_cast<Gain*>(h);
  float factor = std::pow(10.0f, *g->ports[kGainPort] / 20.0f);
  if (*g->ports[kInvertPort] > 0.0f) factor = -factor;
  const Data* in = g->ports[kGainIn];
  Data* out = g->ports[kGainOut];
  for (unsigned long i = 0; i < n; ++i) out[i] = in[i] * factor;
}
void gainCleanup(Handle h) { delete static_cast<Gain*>(h); }

// ── 1: stereo, latency, rate-relative bounds ─────────────────────────────────

enum { kStLatency = 0, kStSteps, kStCorner, kStInL, kStInR, kStOutL, kStOutR, kStPortCount };

struct Stereo {
  Data* ports[kStPortCount] = {};
};

const PortDescriptor kStDescriptors[kStPortCount] = {
    kPortOutput | kPortControl, kPortInput | kPortControl, kPortInput | kPortControl,
    kPortInput | kPortAudio,    kPortInput | kPortAudio,   kPortOutput | kPortAudio,
    kPortOutput | kPortAudio};
const char* const kStNames[kStPortCount] = {"latency", "Steps", "Corner", "In L",
                                            "In R",    "Out L", "Out R"};
const PortRangeHint kStHints[kStPortCount] = {
    {0, 0.0f, 0.0f},
    {kHintBoundedBelow | kHintBoundedAbove | kHintInteger | kHintDefaultMiddle, 1.0f, 8.0f},
    {kHintBoundedBelow | kHintBoundedAbove | kHintSampleRate | kHintLogarithmic | kHintDefaultLow,
     0.0001f, 0.25f},
    {0, 0.0f, 0.0f},
    {0, 0.0f, 0.0f},
    {0, 0.0f, 0.0f},
    {0, 0.0f, 0.0f}};

Handle stInstantiate(const Descriptor*, unsigned long) { return new (std::nothrow) Stereo(); }
void stConnect(Handle h, unsigned long port, Data* data) {
  if (port < kStPortCount) static_cast<Stereo*>(h)->ports[port] = data;
}
void stRun(Handle h, unsigned long n) {
  Stereo* s = static_cast<Stereo*>(h);
  *s->ports[kStLatency] = 64.0f;
  for (unsigned long i = 0; i < n; ++i) {
    s->ports[kStOutL][i] = s->ports[kStInL][i];
    s->ports[kStOutR][i] = s->ports[kStInR][i];
  }
}
void stCleanup(Handle h) { delete static_cast<Stereo*>(h); }

Descriptor makeGain() {
  Descriptor d{};
  d.uniqueId = 4242001ul;
  d.label = "sonore_test_gain";
  d.properties = kPropertyHardRtCapable;
  d.name = "Sonore Test Gain";
  d.maker = "Sonore SDK tests";
  d.copyright = "Apache-2.0";
  d.portCount = kGainPortCount;
  d.portDescriptors = kGainDescriptors;
  d.portNames = kGainNames;
  d.portRangeHints = kGainHints;
  d.instantiate = gainInstantiate;
  d.connect_port = gainConnect;
  d.activate = gainActivate;
  d.run = gainRun;
  d.cleanup = gainCleanup;
  return d;
}

Descriptor makeStereo() {
  Descriptor d{};
  d.uniqueId = 4242002ul;
  d.label = "sonore_test_stereo";
  d.properties = kPropertyHardRtCapable | kPropertyInplaceBroken;
  d.name = "Sonore Test Stereo";
  d.maker = "Sonore SDK tests";
  d.copyright = "Apache-2.0";
  d.portCount = kStPortCount;
  d.portDescriptors = kStDescriptors;
  d.portNames = kStNames;
  d.portRangeHints = kStHints;
  d.instantiate = stInstantiate;
  d.connect_port = stConnect;
  d.run = stRun;
  d.cleanup = stCleanup;
  return d;
}

// ── 2: malformed, on request ─────────────────────────────────────────────────
//
// Port 0 is declared audio AND control. The plugin treats it as the audio
// output it also says it is, and writes a block of samples to it -- so a host
// that connects it to one control float has the plugin write past that float.

enum { kBadOut = 0, kBadIn, kBadPortCount };

struct Malformed {
  Data* ports[kBadPortCount] = {};
};

const PortDescriptor kBadDescriptors[kBadPortCount] = {kPortOutput | kPortAudio | kPortControl,
                                                       kPortInput | kPortAudio};
const char* const kBadNames[kBadPortCount] = {"Out", "In"};

Handle badInstantiate(const Descriptor*, unsigned long) { return new (std::nothrow) Malformed(); }
void badConnect(Handle h, unsigned long port, Data* data) {
  if (port < kBadPortCount) static_cast<Malformed*>(h)->ports[port] = data;
}
void badRun(Handle h, unsigned long n) {
  Malformed* m = static_cast<Malformed*>(h);
  for (unsigned long i = 0; i < n; ++i) m->ports[kBadOut][i] = m->ports[kBadIn][i];
}
void badCleanup(Handle h) { delete static_cast<Malformed*>(h); }

Descriptor makeMalformed() {
  Descriptor d{};
  d.uniqueId = 4242003ul;
  d.label = "sonore_test_malformed";
  d.name = "Sonore Test Malformed";
  d.maker = "Sonore SDK tests";
  d.copyright = "Apache-2.0";
  d.portCount = kBadPortCount;
  d.portDescriptors = kBadDescriptors;
  d.portNames = kBadNames;
  d.instantiate = badInstantiate;
  d.connect_port = badConnect;
  d.run = badRun;
  d.cleanup = badCleanup;
  return d;
}

/** The PROCESS environment, not a C runtime's copy of it: the test sets the
 *  variable from its own runtime, which on Windows need not be this one. */
bool malformedWanted() {
#if defined(_WIN32)
  char value[8];
  return GetEnvironmentVariableA("SONORE_LADSPA_TEST_MALFORMED", value, sizeof(value)) > 0;
#else
  return std::getenv("SONORE_LADSPA_TEST_MALFORMED") != nullptr;
#endif
}

const Descriptor kGain = makeGain();
const Descriptor kStereo = makeStereo();
const Descriptor kMalformed = makeMalformed();

} // namespace

SONORE_LADSPA_EXPORT const sonore::ladspa::Descriptor* ladspa_descriptor(unsigned long index) {
  if (index == 2) return malformedWanted() ? &kMalformed : nullptr;
  return index == 0 ? &kGain : (index == 1 ? &kStereo : nullptr);
}
