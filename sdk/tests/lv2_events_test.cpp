// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: what the LV2 wrapper hands a DSP of the host's MIDI.
//
// The wrapper compiled into the test, driven as a host drives it, with a probe
// instrument that records every MIDI message and where in the block it came.
//
// Event times. An atom event's frame was passed on as it came: one at the end
// of the run or past it sat where a DSP that stops at its last frame never
// reached it -- and in a run long enough to be sliced, it fell in no slice at
// all. Either way a note-off was lost and the note hung.
#define SONORE_NUM_PARAMS 1

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <sonore/dsp.h>

namespace {
struct Received {
  int status, data1, at;
};
std::vector<Received> g_received;
size_t g_offset = 0; // where in the run the DSP's block starts
} // namespace

struct SonoreDsp {
  void prepare(const sonore::ProcessSpec&) {}
  void process(sonore::AudioBlock<float>& io, const float*, sonore::MidiBuffer& midi) {
    for (const auto& e : midi)
      g_received.push_back({e.getMessage().getRawStatus(), e.getMessage().getRawData1(),
                            (int) g_offset + e.samplePosition});
    g_offset += io.getNumSamples();
  }
};

#include <sonore/plugin.h>

static const sonore::ParamInfo kParamTable[SONORE_NUM_PARAMS] = {{"u", "U", "", 0.0f, 1.0f, 0.0f, 0}};
static const sonore::PluginDescriptor kDesc = {
    "com.sonorie.test.lv2events", "Sonore LV2 Events Probe", "Sonorie", "1.0.0", "",
    "https://sonorie.com", true, kParamTable, SONORE_NUM_PARAMS};

#include <sonore/clap_wrapper.h>
#include <sonore/lv2_wrapper.h>

using namespace sonore::lv2;

namespace {
int g_checks = 0, g_failures = 0;

void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

struct UridMap {
  std::vector<std::string> uris;
  static LV2_URID mapCb(LV2_URID_Map_Handle h, const char* uri) {
    auto* self = (UridMap*) h;
    for (size_t i = 0; i < self->uris.size(); ++i)
      if (self->uris[i] == uri) return (LV2_URID) (i + 1);
    self->uris.emplace_back(uri);
    return (LV2_URID) self->uris.size();
  }
  LV2_URID operator()(const char* uri) { return mapCb(this, uri); }
};

/** Writes an atom:Sequence of MIDI events into a port's buffer. */
struct MidiWriter {
  UridMap& map;
  std::vector<uint8_t>& buffer;
  void begin() {
    std::memset(buffer.data(), 0, buffer.size());
    auto* seq = (LV2_Atom_Sequence*) buffer.data();
    seq->atom.type = map(LV2_ATOM__Sequence);
    seq->atom.size = (uint32_t) sizeof(LV2_Atom_Sequence_Body);
  }
  void midi(int64_t frames, uint8_t status, uint8_t d1, uint8_t d2) {
    auto* seq = (LV2_Atom_Sequence*) buffer.data();
    auto* ev = (LV2_Atom_Event*) ((uint8_t*) &seq->body + seq->atom.size);
    ev->time.frames = frames;
    ev->body.type = map(LV2_MIDI__MidiEvent);
    ev->body.size = 3;
    auto* b = (uint8_t*) (ev + 1);
    b[0] = status;
    b[1] = d1;
    b[2] = d2;
    seq->atom.size += (uint32_t) (sizeof(LV2_Atom_Event) + 8u);
  }
};

int receivedAt(int status, int data1) {
  for (const Received& r : g_received)
    if (r.status == status && r.data1 == data1) return r.at;
  return -1000;
}
} // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("── LV2 MIDI into the DSP ──\n");
  UridMap urids;
  LV2_URID_Map map{&urids, UridMap::mapCb};
  LV2_Feature mapFeature{LV2_URID__map, &map};
  const LV2_Feature* features[] = {&mapFeature, nullptr};
  const LV2_Descriptor* d = descriptor();
  LV2_Handle handle = d->instantiate(d, 48000.0, ".", features);
  check(handle != nullptr, "the probe instrument instantiates");
  if (!handle) return 1;
  const int ports = portTimeIn() + 1;
  std::vector<std::vector<uint8_t>> buffers((size_t) ports, std::vector<uint8_t>(1 << 17, 0));
  for (int p = 0; p < ports; ++p) d->connect_port(handle, (uint32_t) p, buffers[(size_t) p].data());
  d->activate(handle);
  MidiWriter midi{urids, buffers[(size_t) portMidiIn()]};
  MidiWriter time{urids, buffers[(size_t) portTimeIn()]};
  time.begin();

  // A note-off stamped at the end of a 512-frame run.
  midi.begin();
  midi.midi(0, 0x90, 60, 100);
  midi.midi(512, 0x80, 60, 0);
  g_received.clear();
  g_offset = 0;
  d->run(handle, 512);
  {
    char line[160];
    std::snprintf(line, sizeof(line), "a note-off stamped at the end of a 512-frame run lands on its last frame (%d)",
                  receivedAt(0x80, 60));
    check(receivedAt(0x80, 60) == 511, line);
  }

  // The same in a run long enough to be sliced (8192 + 808).
  midi.begin();
  midi.midi(0, 0x90, 61, 100);
  midi.midi(9000, 0x80, 61, 0);
  midi.midi(1LL << 40, 0x80, 62, 0);
  g_received.clear();
  g_offset = 0;
  d->run(handle, 9000);
  {
    char line[160];
    std::snprintf(line, sizeof(line),
                  "...and in a sliced 9000-frame run, stamped at 9000 and at 2^40 (%d, %d)",
                  receivedAt(0x80, 61), receivedAt(0x80, 62));
    check(receivedAt(0x80, 61) == 8999 && receivedAt(0x80, 62) == 8999, line);
  }

  // A note-off in a run of no frames. Its events were read, no block ran,
  // and the next run cleared them before reading its own: the note-off never
  // reached the DSP and the note hung. It plays at the start of the next run,
  // once.
  midi.begin();
  midi.midi(0, 0x90, 63, 100);
  g_received.clear();
  g_offset = 0;
  d->run(handle, 64);
  midi.begin();
  midi.midi(0, 0x80, 63, 0);
  d->run(handle, 0);
  midi.begin();
  d->run(handle, 64);
  d->run(handle, 64);
  {
    int offs = 0;
    for (const Received& r : g_received)
      if (r.status == 0x80 && r.data1 == 63) ++offs;
    char line[160];
    std::snprintf(line, sizeof(line),
                  "a note-off in a zero-frame run plays at the start of the next one (%d, %d time(s))",
                  receivedAt(0x80, 63), offs);
    check(receivedAt(0x80, 63) == 64 && offs == 1, line);
  }

  // The numbers the bundle publishes: every float as itself. %g wrote six
  // significant digits, and a default of 0.70710678 came out as 0.707107.
  {
    bool same = true;
    char worst[64] = "";
    for (float v : {0.70710678f, 0.123456789f, 20000.0f, -96.0f, 0.1f, 1e-7f, 3.4e38f}) {
      char text[32];
      ttlNumber(text, sizeof(text), v);
      if ((float) std::strtod(text, nullptr) != v) {
        same = false;
        std::snprintf(worst, sizeof(worst), "%.9g as %s", (double) v, text);
      }
    }
    char tenth[32];
    ttlNumber(tenth, sizeof(tenth), 0.1f);
    char line[160];
    std::snprintf(line, sizeof(line), "a published number reads back as the same float, and 0.1 as \"%s\" %s",
                  tenth, worst);
    check(same && std::strcmp(tenth, "0.1") == 0, line);
  }

  d->deactivate(handle);
  d->cleanup(handle);
  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  if (g_failures == 0) std::printf("SONORE LV2 EVENTS PASSED\n");
  return g_failures == 0 ? 0 : 1;
}
