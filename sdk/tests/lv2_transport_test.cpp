// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the host's timeline reaching an LV2 DSP.
//
// LV2 has no process context. A host says where its transport is by sending
// time:Position objects on an atom input that declares it understands them,
// when the transport jumps -- start, stop, locate, a tempo change -- and the
// plugin carries the position forward itself in between. The wrapper had no
// such port and handed every DSP an empty TransportInfo, so a synced effect
// had no tempo, no position and no play state in Ardour or Carla, where CLAP,
// VST3 and AU all gave it one.
//
// This compiles the wrapper in, sends positions the way a host does, and
// reads back what the DSP was told.
#define SONORE_NUM_PARAMS 1

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <sonore/dsp.h>
#include <sonore/transport.h>
#include <sonore/turtle.h>

namespace {
sonore::TransportInfo g_seen;
int g_calls = 0;
} // namespace

// The context form, so the fixture can have an aux bus: its name, and the
// parameter group's, go into the TTL the test reads back.
struct SonoreDsp {
  void prepare(const sonore::ProcessSpec&) {}
  void setTransport(const sonore::TransportInfo& t) {
    g_seen = t;
    ++g_calls;
  }
  void process(sonore::ProcessContext&, const float*) {}
};

#include <sonore/plugin.h>

// A quote and a backslash in the names: each closed or broke the Turtle
// literal it was written into, and the plugin's whole TTL with it.
static const sonore::ParamInfo kParamTable[SONORE_NUM_PARAMS] = {
    {"unused", "Unused", "", 0.0f, 1.0f, 0.0f, 0, "Amp \"Clean\" In\\Out"},
};
static const sonore::AuxBusInfo kAuxOutputs[1] = {{"Stem \"A\"", 2}};

static sonore::PluginDescriptor fixture() {
  sonore::PluginDescriptor d;
  d.id = "com.sonorie.test.lv2transport";
  d.name = "Sonore LV2 Transport Fixture";
  d.description = "Records the transport it is given.";
  d.url = "https://sonorie.com";
  d.params = kParamTable;
  d.numParams = SONORE_NUM_PARAMS;
  d.category = "utility";
  d.auxOutputs = kAuxOutputs;
  d.numAuxOutputs = 1;
  return d;
}
static const sonore::PluginDescriptor kDesc = fixture();

#include <sonore/clap_wrapper.h>
#include <sonore/lv2_wrapper.h>

using namespace sonore::lv2;

namespace {
int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

bool approx(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }

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

#define TIME "http://lv2plug.in/ns/ext/time#"

/** Writes one atom sequence holding at most one time:Position object, the
 *  shape a host's forge produces: each property's value padded to eight. */
struct PositionWriter {
  UridMap& map;
  std::vector<uint8_t>& buffer;
  std::vector<uint8_t> props;

  void raw(const void* p, size_t n) {
    const auto* b = (const uint8_t*) p;
    props.insert(props.end(), b, b + n);
    while (props.size() % 8) props.push_back(0);
  }
  void header(const char* key, const char* type, uint32_t size) {
    LV2_Atom_Property_Body body{};
    body.key = map(key);
    body.context = 0;
    body.value.size = size;
    body.value.type = map(type);
    props.insert(props.end(), (const uint8_t*) &body, (const uint8_t*) &body + sizeof(body));
  }
  void f(const char* key, float v) {
    header(key, LV2_ATOM__Float, 4);
    raw(&v, 4);
  }
  void l(const char* key, int64_t v) {
    header(key, LV2_ATOM__Long, 8);
    raw(&v, 8);
  }
  void i(const char* key, int32_t v) {
    header(key, LV2_ATOM__Int, 4);
    raw(&v, 4);
  }

  /** Close the object into the sequence, stamped at `frame`; with `send`
   *  false, an empty one. */
  void finish(bool send, int64_t frame = 0) {
    std::memset(buffer.data(), 0, buffer.size());
    auto* seq = (LV2_Atom_Sequence*) buffer.data();
    seq->atom.type = map(LV2_ATOM__Sequence);
    seq->atom.size = (uint32_t) sizeof(LV2_Atom_Sequence_Body);
    if (send) {
      auto* ev = (LV2_Atom_Event*) (seq + 1);
      ev->time.frames = frame;
      ev->body.type = map(LV2_ATOM__Object);
      ev->body.size = (uint32_t) (sizeof(LV2_Atom_Object_Body) + props.size());
      auto* obj = (LV2_Atom_Object_Body*) (ev + 1);
      obj->id = 0;
      obj->otype = map(TIME "Position");
      std::memcpy(obj + 1, props.data(), props.size());
      seq->atom.size += (uint32_t) (sizeof(LV2_Atom_Event) + ((ev->body.size + 7u) & ~7u));
    }
    props.clear();
  }
};
} // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0); // a crash still shows how far it got
  std::printf("lv2_transport_test: time:Position into setTransport\n");

  // ── The port, declared so a host knows to send ─────────────────────────
  check(timePorts() == 1, "a DSP that takes the transport gets a time input");
  {
    // A named file beside the binary: tmpfile() on Windows writes to the
    // root of the drive, which an ordinary user may not.
    std::FILE* ttl = std::fopen("lv2_transport_test.ttl", "w+b");
    check(ttl != nullptr, "the TTL can be written");
    if (!ttl) return 1;
    writeTtl(ttl, "Fixture.dll");
    std::fflush(ttl);
    std::rewind(ttl);
    std::string text;
    char chunk[4096];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof(chunk), ttl)) > 0) text.append(chunk, n);
    std::fclose(ttl);
    std::remove("lv2_transport_test.ttl");
    const bool supports = text.find("atom:supports time:Position") != std::string::npos;
    const bool prefixed = text.find("@prefix time:") != std::string::npos;
    char index[64];
    std::snprintf(index, sizeof(index), "lv2:index %d", portTimeIn());
    check(supports && prefixed && text.find(index) != std::string::npos,
          "and the TTL declares it, at its index, as supporting time:Position");

    // The whole file is Turtle, names with quotes and backslashes included:
    // the group's and the aux bus's were written raw, and a host skips a
    // bundle whose TTL does not parse.
    sonore::turtle::Document doc;
    const bool parsed = sonore::turtle::parse(text, "file:///fixture.lv2/", &doc);
    int groupNames = 0, auxNames = 0;
    for (const sonore::turtle::Triple& t : doc.triples) {
      if (!t.objectIsLiteral) continue;
      if (t.object == "Amp \"Clean\" In\\Out") ++groupNames;
      if (t.object == "Stem \"A\" 1" || t.object == "Stem \"A\" 2") ++auxNames;
    }
    char line[160];
    std::snprintf(line, sizeof(line),
                  "the TTL parses (%s) and gives back the group's name twice (%d) and the aux "
                  "bus's ports theirs (%d)",
                  parsed ? "yes" : "no", groupNames, auxNames);
    check(parsed && groupNames == 2 && auxNames == 2, line);
  }

  UridMap urids;
  LV2_URID_Map map{&urids, UridMap::mapCb};
  LV2_Feature mapFeature{LV2_URID__map, &map};
  const LV2_Feature* features[] = {&mapFeature, nullptr};

  constexpr double kRate = 48000.0;
  const LV2_Descriptor* d = descriptor();
  LV2_Handle handle = d->instantiate(d, kRate, ".", features);
  check(handle != nullptr, "the plugin instantiates");
  if (!handle) return 1;

  // Every port its own zeroed buffer, room for the longest run below (48000
  // floats): a zero atom sequence claims nothing.
  const int ports = portTimeIn() + 1;
  std::vector<std::vector<uint8_t>> buffers((size_t) ports, std::vector<uint8_t>(1 << 18, 0));
  for (int p = 0; p < ports; ++p) d->connect_port(handle, (uint32_t) p, buffers[(size_t) p].data());
  d->activate(handle);
  PositionWriter pos{urids, buffers[(size_t) portTimeIn()], {}};

  // ── Playing, 3/4 at 90, bar 2 beat 1.5, two seconds in ─────────────────
  pos.f(TIME "speed", 1.0f);
  pos.f(TIME "beatsPerMinute", 90.0f);
  pos.f(TIME "beatsPerBar", 3.0f);
  pos.i(TIME "beatUnit", 4);
  pos.l(TIME "bar", 2);
  pos.f(TIME "barBeat", 1.5f);
  pos.l(TIME "frame", 96000);
  pos.finish(true);
  d->run(handle, 256);
  std::printf("  ---- told: playing %d, %.2f bpm, %d/%d, beat %.4f, bar %d from %.4f, %.4f s ----\n",
              (int) g_seen.isPlaying, g_seen.tempo, g_seen.timeSigNumerator,
              g_seen.timeSigDenominator, g_seen.positionBeats, (int) g_seen.barNumber,
              g_seen.barStartBeats, g_seen.positionSeconds);
  check(g_calls > 0 && g_seen.isPlaying, "a position with speed 1 says the transport is playing");
  check(g_seen.hasTempo && approx(g_seen.tempo, 90.0), "with the host's tempo");
  check(g_seen.timeSigNumerator == 3 && g_seen.timeSigDenominator == 4, "and its time signature");
  check(g_seen.hasBeats && approx(g_seen.positionBeats, 7.5) && approx(g_seen.barStartBeats, 6.0) &&
            g_seen.barNumber == 2,
        "bar 2, beat 1.5 of 3/4 is quarter note 7.5, in a bar that began at 6");
  check(g_seen.hasSeconds && approx(g_seen.positionSeconds, 2.0), "and frame 96000 is two seconds");

  // ── Carried forward between positions, as a host expects ───────────────
  pos.finish(false);
  d->run(handle, 24000);
  const double afterFirst = 7.5 + 256.0 / kRate * 1.5;
  check(approx(g_seen.positionBeats, afterFirst, 1e-6) &&
            approx(g_seen.positionSeconds, 2.0 + 256.0 / kRate, 1e-9),
        "with no new position the next run starts where the last one ended");
  d->run(handle, 256);
  const double afterSecond = afterFirst + 0.5 * 1.5; // half a second at 1.5 quarters/s
  std::printf("  ---- carried to beat %.4f, bar %d from %.4f ----\n", g_seen.positionBeats,
              (int) g_seen.barNumber, g_seen.barStartBeats);
  check(approx(g_seen.positionBeats, afterSecond, 1e-6),
        "half a second later it is three quarters of a beat on");
  check(g_seen.barNumber == 2 && approx(g_seen.barStartBeats, 6.0),
        "still in bar 2, which runs to quarter note 9");
  pos.finish(false);
  d->run(handle, 48000);
  d->run(handle, 16);
  check(g_seen.barNumber == 3 && approx(g_seen.barStartBeats, 9.0),
        "and a second later the bar has turned over to 3");

  // ── Stopped ────────────────────────────────────────────────────────────
  pos.f(TIME "speed", 0.0f);
  pos.finish(true);
  d->run(handle, 256);
  const double stoppedAt = g_seen.positionBeats;
  check(!g_seen.isPlaying, "speed 0 says the transport stopped");
  pos.finish(false);
  d->run(handle, 4800);
  d->run(handle, 256);
  check(approx(g_seen.positionBeats, stoppedAt), "and a stopped transport stays where it is");

  // ── 6/8, where a beat is an eighth ─────────────────────────────────────
  pos.f(TIME "speed", 1.0f);
  pos.f(TIME "beatsPerMinute", 180.0f);
  pos.f(TIME "beatsPerBar", 6.0f);
  pos.i(TIME "beatUnit", 8);
  pos.l(TIME "bar", 1);
  pos.f(TIME "barBeat", 3.0f);
  pos.finish(true);
  d->run(handle, 256);
  std::printf("  ---- 6/8: %.2f bpm, beat %.4f from %.4f ----\n", g_seen.tempo,
              g_seen.positionBeats, g_seen.barStartBeats);
  check(approx(g_seen.tempo, 90.0),
        "180 eighths a minute is 90 quarters, the unit every format's tempo is in");
  check(approx(g_seen.positionBeats, 4.5) && approx(g_seen.barStartBeats, 3.0),
        "bar 1, eighth 3 of 6/8 is quarter note 4.5, in a bar that began at 3");

  // ── Rubbish: a property claiming more than the object holds ────────────
  pos.f(TIME "beatsPerMinute", 60.0f);
  pos.finish(true);
  {
    auto* seq = (LV2_Atom_Sequence*) buffers[(size_t) portTimeIn()].data();
    auto* ev = (LV2_Atom_Event*) (seq + 1);
    auto* prop = (LV2_Atom_Property_Body*) ((uint8_t*) (ev + 1) + sizeof(LV2_Atom_Object_Body));
    prop->value.size = 0xFFFFFFF0u;
  }
  d->run(handle, 256);
  check(approx(g_seen.tempo, 90.0), "a property running past its object is ignored, not read");

  // ── A position stamped mid-run ─────────────────────────────────────────
  //
  // At frame 300 of a 512-frame run the song is at quarter note 8, 120 bpm.
  // It is carried forward from there: carried over the whole run, the next
  // run started 300 frames ahead, and stayed ahead.
  pos.f(TIME "speed", 1.0f);
  pos.f(TIME "beatsPerMinute", 120.0f);
  pos.f(TIME "beatsPerBar", 4.0f);
  pos.i(TIME "beatUnit", 4);
  pos.l(TIME "bar", 2);
  pos.f(TIME "barBeat", 0.0f);
  pos.l(TIME "frame", 96000);
  pos.finish(true, 300);
  d->run(handle, 512);
  pos.finish(false);
  d->run(handle, 512);
  {
    const double truth = 2.0 + (512.0 - 300.0) / kRate; // seconds at the second run's start
    const double off = (g_seen.positionSeconds - truth) * kRate;
    char line[160];
    std::snprintf(line, sizeof(line),
                  "a position stamped at frame 300 is carried from frame 300 (%.1f frames off)", off);
    check(std::fabs(off) < 0.01 && approx(g_seen.positionBeats, 8.0 + (512.0 - 300.0) / kRate * 2.0, 1e-9),
          line);
  }

  d->deactivate(handle);
  d->cleanup(handle);
  std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
