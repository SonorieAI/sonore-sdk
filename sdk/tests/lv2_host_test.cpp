// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the LV2 wrapper driven exactly as a host drives it.
//
// Loads the BUILT bundle the way Ardour does: read the TTL for the plugin's
// shape, dlopen the binary, get lv2_descriptor, instantiate with a real
// urid:map, connect every port, run audio and MIDI through it, and round-trip
// state through the state interface. The TTL is parsed (minimally: it is our
// own generated format) precisely because a TTL that disagrees with
// connect_port() is a crash in a real host, so the test must read the metadata
// rather than share the plugin's compile-time constants.
#include <sonore/midi_ci.h>
#include <lv2/atom/atom.h>
#include <lv2/core/lv2.h>
#include <lv2/midi/midi.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>

// The worker ABI, declared here rather than taken from the SDK. This file is
// a HOST: reading our own headers for a layout the plugin also reads would
// make the two agree with each other instead of with the spec.
typedef void* Lv2WorkerRespondHandle;
typedef void* Lv2WorkerScheduleHandle;
enum Lv2WorkerStatus { kLv2WorkerSuccess = 0, kLv2WorkerErrUnknown = 1 };
typedef Lv2WorkerStatus (*Lv2WorkerRespondFunction)(Lv2WorkerRespondHandle, uint32_t,
                                                    const void*);
struct Lv2WorkerSchedule {
  Lv2WorkerScheduleHandle handle;
  Lv2WorkerStatus (*schedule_work)(Lv2WorkerScheduleHandle, uint32_t, const void*);
};
struct Lv2WorkerInterface {
  Lv2WorkerStatus (*work)(LV2_Handle, Lv2WorkerRespondFunction, Lv2WorkerRespondHandle, uint32_t,
                          const void*);
  Lv2WorkerStatus (*work_response)(LV2_Handle, uint32_t, const void*);
  Lv2WorkerStatus (*end_run)(LV2_Handle);
};

#include <cmath>
#include <limits>
#include <random>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
using LibHandle = HMODULE;
static LibHandle openLib(const char* p) { return LoadLibraryA(p); }
static void* symbolOf(LibHandle h, const char* n) { return (void*) GetProcAddress(h, n); }
#else
#include <dlfcn.h>
using LibHandle = void*;
static LibHandle openLib(const char* p) { return dlopen(p, RTLD_LOCAL | RTLD_NOW); }
static void* symbolOf(LibHandle h, const char* n) { return dlsym(h, n); }
#endif

static int g_failures = 0;
static int g_checks = 0;

static void check(bool ok, const char* what) {
  ++g_checks;
  std::printf(ok ? "  ok   %s\n" : "  FAIL %s\n", what);
  if (!ok) ++g_failures;
}

// ── A real urid:map ──────────────────────────────────────────────────────────

struct UridMap {
  std::map<std::string, LV2_URID> ids;
  LV2_URID next = 1;

  static LV2_URID mapCb(LV2_URID_Map_Handle handle, const char* uri) {
    auto* self = (UridMap*) handle;
    auto found = self->ids.find(uri);
    if (found != self->ids.end()) return found->second;
    const LV2_URID id = self->next++;
    self->ids.emplace(uri, id);
    return id;
  }
};

// ── State callbacks over a memory store ──────────────────────────────────────

struct StateStore {
  std::map<uint32_t, std::vector<uint8_t>> values;
  std::map<uint32_t, uint32_t> types;

  static LV2_State_Status storeCb(LV2_State_Handle handle, uint32_t key, const void* value,
                                  size_t size, uint32_t type, uint32_t) {
    auto* self = (StateStore*) handle;
    const auto* bytes = (const uint8_t*) value;
    self->values[key] = std::vector<uint8_t>(bytes, bytes + size);
    self->types[key] = type;
    return LV2_STATE_SUCCESS;
  }
  static const void* retrieveCb(LV2_State_Handle handle, uint32_t key, size_t* size,
                                uint32_t* type, uint32_t* flags) {
    auto* self = (StateStore*) handle;
    auto found = self->values.find(key);
    if (found == self->values.end()) return nullptr;
    if (size) *size = found->second.size();
    if (type) *type = self->types[key];
    if (flags) *flags = 0;
    return found->second.data();
  }
};

// ── Minimal TTL reading (our own generated format) ───────────────────────────

struct PortInfo {
  int index = -1;
  bool control = false, audio = false, atom = false, input = false, output = false;
  float def = 0.0f;
  /** The port's symbol. A test that located a port by INDEX would be
   *  recomputing the wrapper's own layout arithmetic and agreeing with itself;
   *  the symbol is what a host actually looks a port up by. */
  std::string symbol;
};

static std::string readFile(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return "";
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

static std::vector<PortInfo> parsePorts(const std::string& ttl) {
  std::vector<PortInfo> ports;
  size_t pos = 0;
  while ((pos = ttl.find("lv2:index ", pos)) != std::string::npos) {
    // Each port block in our generated TTL runs from its type line (before the
    // index) to the next "] , [" or "] .": scan a window around the index.
    const size_t blockStart = ttl.rfind('[', pos);
    size_t blockEnd = ttl.find(']', pos);
    if (blockStart == std::string::npos || blockEnd == std::string::npos) break;
    const std::string block = ttl.substr(blockStart, blockEnd - blockStart);

    PortInfo p;
    p.index = std::atoi(ttl.c_str() + pos + 10);
    p.control = block.find("lv2:ControlPort") != std::string::npos;
    p.audio = block.find("lv2:AudioPort") != std::string::npos;
    p.atom = block.find("atom:AtomPort") != std::string::npos;
    p.input = block.find("lv2:InputPort") != std::string::npos;
    p.output = block.find("lv2:OutputPort") != std::string::npos;
    const size_t at = block.find("lv2:default ");
    if (at != std::string::npos) p.def = (float) std::atof(block.c_str() + at + 12);
    const size_t sym = block.find("lv2:symbol \"");
    if (sym != std::string::npos) {
      const size_t start = sym + 12;
      const size_t end = block.find('"', start);
      if (end != std::string::npos) p.symbol = block.substr(start, end - start);
    }
    ports.push_back(p);
    pos = blockEnd;
  }
  return ports;
}

int main(int argc, char** argv) {
  // Unbuffered, so a crash under ctest still shows the line it crashed after.
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 2) {
    std::printf("usage: lv2_host_test <bundle-dir>\n");
    return 2;
  }
  const std::string bundle = argv[1];
  bool expectSidechain = false;
  int expectAuxOuts = -1;
  bool expectMidiOut = false;
  /** Is this the fixture that REPORTS what the host told it? Only the probe
   *  can answer a question about whether a flag arrived, because only the
   *  probe turns what it was told back into audio. */
  bool expectProbe = false;
  for (int i = 2; i < argc; ++i) {
    if (std::strcmp(argv[i], "--expect-sidechain") == 0) expectSidechain = true;
    if (std::strcmp(argv[i], "--expect-midi-out") == 0) expectMidiOut = true;
    if (std::strcmp(argv[i], "--expect-probe") == 0) expectProbe = true;
    if (std::strcmp(argv[i], "--expect-aux-outs") == 0 && i + 1 < argc) {
      expectAuxOuts = std::atoi(argv[i + 1]);
      ++i;
    }
  }
  std::printf("Sonore LV2 host test\n  bundle: %s\n\n", bundle.c_str());

  // ── The metadata a host reads first ───────────────────────────────────────
  const std::string manifest = readFile(bundle + "/manifest.ttl");
  check(!manifest.empty(), "manifest.ttl exists");
  check(manifest.find("lv2:binary") != std::string::npos, "…and names a binary");

  const std::string ttl = readFile(bundle + "/plugin.ttl");
  check(!ttl.empty(), "plugin.ttl exists");
  check(ttl.find("lv2:requiredFeature urid:map") != std::string::npos,
        "urid:map is declared required (the code depends on it)");
  check(ttl.find("state:interface") != std::string::npos, "the state interface is declared");

  const std::vector<PortInfo> ports = parsePorts(ttl);
  check(!ports.empty(), "the TTL declares ports");
  int controls = 0, audioIn = 0, audioOut = 0, atomIn = 0;
  int maxIndex = -1;
  bool indicesDense = true;
  std::vector<bool> seen(ports.size(), false);
  for (const PortInfo& p : ports) {
    if (p.control) ++controls;
    if (p.audio && p.input) ++audioIn;
    if (p.audio && p.output) ++audioOut;
    // An atom INPUT no longer means MIDI. Every plugin now has one for its
    // interface to ask questions over, so the two are told apart by SYMBOL --
    // which is what a host looks a port up by, and what this file says
    // everywhere else.
    if (p.atom && p.input && p.symbol != "uiControl") ++atomIn;
    if (p.index > maxIndex) maxIndex = p.index;
    if (p.index >= 0 && p.index < (int) seen.size() && !seen[p.index]) seen[p.index] = true;
    else indicesDense = false;
  }
  {
    // The interface's own two ports. They exist on every plugin, because a
    // port map that varies with what a DSP declares is a map that renumbers
    // when somebody adds a feature -- and the numbering is published in a ttl
    // file that sessions refer to.
    bool haveControl = false, haveNotify = false;
    for (const PortInfo& p : ports) {
      if (p.symbol == "uiControl") haveControl = p.atom && p.input;
      if (p.symbol == "uiNotify") haveNotify = p.atom && p.output;
    }
    check(haveControl, "there is an atom INPUT for the interface to ask over");
    check(haveNotify, "\u2026and an atom OUTPUT for the plugin to answer on");
  }

  check(indicesDense && maxIndex == (int) ports.size() - 1,
        "port indices are dense and unique: hosts connect by index");

  // Note-driven or signal-driven is decided by the PORTS, the way a host
  // decides it: a MIDI input and no audio input means notes come first. The
  // taxonomy class cannot answer this -- an arpeggiator is note-driven but
  // files itself under Utility rather than Instrument.
  const bool instrument = atomIn == 1 && audioIn == 0;
  std::printf("  ---- graded as %s ----\n",
              instrument ? (expectMidiOut ? "a NOTE EFFECT" : "an INSTRUMENT") : "an EFFECT");
  // The main stereo bus, plus two ports per aux bus when the plugin has them.
  check(audioOut == 2 + (expectAuxOuts > 0 ? expectAuxOuts * 2 : 0),
        expectAuxOuts > 0 ? "the main stereo bus plus every aux channel"
                          : "two audio outputs");
  if (instrument) {
    check(atomIn == 1 && audioIn == 0, "an instrument has a MIDI port and no audio input");
  } else {
    check(audioIn == (expectSidechain ? 4 : 2) && atomIn == 0,
          expectSidechain ? "a sidechain effect has FOUR audio inputs and no MIDI port"
                          : "an effect has stereo input and no MIDI port");
  }

  // ── Load the binary the TTL named ─────────────────────────────────────────
  const size_t at = manifest.find("lv2:binary <");
  std::string binaryName = manifest.substr(at + 12);
  binaryName = binaryName.substr(0, binaryName.find('>'));
  const std::string binaryPath = bundle + "/" + binaryName;

  LibHandle lib = openLib(binaryPath.c_str());
  check(lib != nullptr, "the binary loads");
  if (!lib) return 1;

  using DescriptorFn = const LV2_Descriptor* (*) (uint32_t);
  auto descriptorFn = (DescriptorFn) symbolOf(lib, "lv2_descriptor");
  check(descriptorFn != nullptr, "lv2_descriptor is exported");
  if (!descriptorFn) return 1;

  const LV2_Descriptor* descriptor = descriptorFn(0);
  check(descriptor != nullptr, "descriptor 0 exists");
  check(descriptorFn(1) == nullptr, "descriptor 1 does not (one plugin per bundle)");
  if (!descriptor) return 1;
  check(ttl.find(descriptor->URI) != std::string::npos,
        "the code's URI matches the TTL's: hosts match plugins BY URI");

  // ── Instantiate with a real urid:map ──────────────────────────────────────
  UridMap urids;
  // The worker feature, which is how an LV2 plugin gets a thread it is
  // allowed to read a file on. A real host runs work() on a background thread
  // and delivers work_response() before the next run(); this host does both
  // inline, because what is being tested is that the plugin ASKS rather than
  // reading the file in run() itself.
  struct Worker {
    const LV2_Descriptor* descriptor = nullptr;
    LV2_Handle handle = nullptr;
    int scheduled = 0;
    int responded = 0;
  };
  static Worker g_worker;
  struct WorkerBridge {
    static Lv2WorkerStatus respondCb(Lv2WorkerRespondHandle, uint32_t, const void*) {
      ++g_worker.responded;
      return kLv2WorkerSuccess;
    }
    static Lv2WorkerStatus scheduleCb(Lv2WorkerScheduleHandle, uint32_t size, const void* data) {
      ++g_worker.scheduled;
      if (!g_worker.descriptor || !g_worker.handle) return kLv2WorkerErrUnknown;
      const auto* iface = (const Lv2WorkerInterface*) g_worker.descriptor->extension_data(
          "http://lv2plug.in/ns/ext/worker#interface");
      if (!iface || !iface->work) return kLv2WorkerErrUnknown;
      const Lv2WorkerStatus st =
          iface->work(g_worker.handle, WorkerBridge::respondCb, nullptr, size, data);
      if (iface->work_response) iface->work_response(g_worker.handle, 0, nullptr);
      return st;
    }
  };
  Lv2WorkerSchedule workerSchedule{nullptr, WorkerBridge::scheduleCb};
  LV2_Feature workerFeature{"http://lv2plug.in/ns/ext/worker#schedule", &workerSchedule};

  LV2_URID_Map map{&urids, UridMap::mapCb};
  LV2_Feature mapFeature{LV2_URID__map, &map};
  const LV2_Feature* features[] = {&mapFeature, &workerFeature, nullptr};

  LV2_Handle handle = descriptor->instantiate(descriptor, 48000.0, bundle.c_str(), features);
  check(handle != nullptr, "the plugin instantiates");
  if (!handle) return 1;

  const LV2_Feature* bare[] = {nullptr};
  LV2_Handle without = descriptor->instantiate(descriptor, 48000.0, bundle.c_str(), bare);
  check(without == nullptr, "…and refuses to instantiate without urid:map, as declared");
  if (without) descriptor->cleanup(without);

  // ── Connect everything and run ────────────────────────────────────────────
  constexpr uint32_t kFrames = 512;
  std::vector<float> controlValues(ports.size(), 0.0f);
  std::vector<float> inL(kFrames), inR(kFrames), outL(kFrames, -1.0f), outR(kFrames, -1.0f);
  std::vector<float> scL(kFrames, 0.0f), scR(kFrames, 0.0f);
  // LV2 flattens aux buses into ordinary output ports, so they connect like
  // any other output -- and must still receive their own band.
  std::vector<std::vector<float>> auxOut;
  for (int i = 0; i < 8; ++i) auxOut.emplace_back(kFrames, 0.0f);

  // An atom sequence with one note-on, hand-built to the published layout.
  alignas(8) uint8_t midiBuffer[sizeof(LV2_Atom_Sequence) + 64];
  auto* seq = (LV2_Atom_Sequence*) midiBuffer;
  {
    const LV2_URID midiEvent = UridMap::mapCb(&urids, LV2_MIDI__MidiEvent);
    auto* event = (LV2_Atom_Event*) ((uint8_t*) &seq->body + sizeof(LV2_Atom_Sequence_Body));
    event->time.frames = 0;
    event->body.size = 3;
    event->body.type = midiEvent;
    auto* bytes = (uint8_t*) event + sizeof(LV2_Atom_Event);
    bytes[0] = 0x90; // note on
    bytes[1] = 69;   // A4
    bytes[2] = 100;
    seq->atom.type = UridMap::mapCb(&urids, LV2_ATOM__Sequence);
    seq->atom.size = (uint32_t) (sizeof(LV2_Atom_Sequence_Body) + sizeof(LV2_Atom_Event) + 8);
    seq->body.unit = 0;
    seq->body.pad = 0;
  }

  // The host owns the output sequence buffer and sets atom.size to its
  // CAPACITY before each run; the plugin overwrites it with what it wrote.
  alignas(8) uint8_t midiOutBuffer[4096];
  auto* outSeq = (LV2_Atom_Sequence*) midiOutBuffer;
  // The interface's notify port gets its OWN buffer. The plugin writes both
  // in the same run(), so sharing one would have each overwrite the other and
  // the failure would look like a plugin that emits no MIDI.
  alignas(8) uint8_t uiNotifyBuffer[4096];
  auto* notifySeq = (LV2_Atom_Sequence*) uiNotifyBuffer;
  alignas(8) uint8_t uiControlBuffer[1024];
  auto* controlSeq = (LV2_Atom_Sequence*) uiControlBuffer;
  int atomOutSeen = 0;

  int audioInSeen = 0, audioOutSeen = 0;
  // Which port takes incoming MIDI, so one test can point it somewhere else.
  int portMidiIn = -1;
  for (const PortInfo& p : ports) {
    if (p.control) {
      controlValues[(size_t) p.index] = p.def; // defaults from the TTL, as hosts do
      descriptor->connect_port(handle, (uint32_t) p.index, &controlValues[(size_t) p.index]);
    } else if (p.audio && p.input) {
      // Port order is [main L][main R][sidechain L][sidechain R] by index.
      float* bufs[4] = {inL.data(), inR.data(), scL.data(), scR.data()};
      descriptor->connect_port(handle, (uint32_t) p.index,
                               bufs[audioInSeen < 4 ? audioInSeen : 3]);
      ++audioInSeen;
    } else if (p.audio && p.output) {
      // Ports 0 and 1 are the main bus; anything after is an aux channel.
      float* dst = audioOutSeen == 0   ? outL.data()
                   : audioOutSeen == 1 ? outR.data()
                                       : auxOut[(size_t) (audioOutSeen - 2)].data();
      descriptor->connect_port(handle, (uint32_t) p.index, dst);
      ++audioOutSeen;
    } else if (p.atom && p.output) {
      if (p.symbol == "uiNotify") {
        descriptor->connect_port(handle, (uint32_t) p.index, notifySeq);
      } else {
        descriptor->connect_port(handle, (uint32_t) p.index, outSeq);
        ++atomOutSeen; // the MIDI one, which is what the count is about
      }
    } else if (p.atom) {
      descriptor->connect_port(handle, (uint32_t) p.index,
                               p.symbol == "uiControl" ? controlSeq : seq);
      if (p.symbol != "uiControl") portMidiIn = p.index;
    }
  }

  g_worker.descriptor = descriptor;
  g_worker.handle = handle;
  if (descriptor->activate) descriptor->activate(handle);

  double energy = 0.0;
  bool finite = true;
  float peak = 0.0f;
  int phase = 0;
  for (int block = 0; block < 40; ++block) {
    for (uint32_t i = 0; i < kFrames; ++i) {
      const float s = 0.25f * (float) std::sin(2.0 * 3.14159265358979 * 440.0 * phase / 48000.0);
      inL[i] = s;
      inR[i] = s;
      ++phase;
    }
    descriptor->run(handle, kFrames);
    for (uint32_t i = 0; i < kFrames; ++i) {
      if (!std::isfinite(outL[i]) || !std::isfinite(outR[i])) finite = false;
      const float a = outL[i] < 0.0f ? -outL[i] : outL[i];
      if (a > peak) peak = a;
      energy += (double) outL[i] * outL[i];
    }
  }
  check(finite, "audio stays finite (no NaN/Inf)");
  if (expectMidiOut)
    check(energy == 0.0, "a note effect stays audibly silent");
  else
    check(energy > 0.0, instrument ? "the note-on makes the instrument sound"
                                   : "the effect produces sound");
  check(peak < 10.0f, "the output does not blow up");


  // \u2500\u2500 The meter ports \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
  //
  // An LV2 interface is a SEPARATE MODULE from the plugin and shares no
  // memory with it, so a level reaches it only through a port. Without these
  // an LV2 build was the one format whose editor could not draw a meter --
  // and the plugin side did not even measure one.
  //
  // Located by symbol, like everything else here: an index would pass while
  // pointing at whatever happened to sit there.
  {
    int peakIdx = -1, rmsIdx = -1;
    for (const PortInfo& p : ports) {
      if (p.symbol == "meterPeak") peakIdx = p.index;
      if (p.symbol == "meterRms") rmsIdx = p.index;
    }
    check(peakIdx >= 0 && rmsIdx >= 0, "the plugin publishes its level on output ports");
    if (peakIdx >= 0 && rmsIdx >= 0) {
      const float metPeak = controlValues[(size_t) peakIdx];
      const float metRms = controlValues[(size_t) rmsIdx];
      std::printf("  ---- meter ports: peak %.4f, rms %.4f (audio peak %.4f) ----\n",
                  metPeak, metRms, peak);
      if (expectMidiOut) {
        // A note effect is silent by design, so its meter must be too --
        // which is worth checking, because a meter that reads something when
        // nothing is playing is the version of this bug a user reports.
        check(metPeak == 0.0f, "a silent plugin reports a silent meter");
      } else {
        check(metPeak > 0.0f, "\u2026and it is not zero while audio is playing");
        // RMS below peak, and both inside the block that was just rendered.
        // This is what says the two ports are not the same number twice.
        check(metRms > 0.0f && metRms < metPeak,
              "\u2026with RMS below peak, so the two ports are really two");
        check(metPeak <= peak + 1e-3f,
              "\u2026and the peak it reports is one the audio actually reached");
      }
    }
  }


  // ── The rates a host really uses ──────────────────────────────────────────
  //
  // LV2 fixes the sample rate at INSTANTIATE, not at activate, so sweeping it
  // exercises a path the other two formats do not have: a fresh instance per
  // rate, each one building its filters from a number it was handed once and
  // can never revise.
  //
  // 8 kHz is the one that hurts. Nyquist is 4 kHz, so a tone control that goes
  // to 18 kHz is asking for a cutoff above half the sample rate, and a filter
  // that does not clamp produces NaN on its first sample.
  {
    const double rates[] = {8000.0, 22050.0, 96000.0, 192000.0};
    const uint32_t blocks[] = {1u, 7u, kFrames};
    bool allInstantiated = true, allFinite = true, allSane = true;
    double worst = 0.0;

    for (double rate : rates) {
      LV2_Handle h = descriptor->instantiate(descriptor, rate, bundle.c_str(), features);
      if (!h) {
        allInstantiated = false;
        continue;
      }
      // The same wiring the main instance got, port for port. A rate sweep
      // that skipped a port would be measuring a plugin with a dangling
      // pointer rather than one at an unusual rate.
      int inSeen = 0, outSeen = 0;
      for (const PortInfo& pi : ports) {
        if (pi.control) {
          descriptor->connect_port(h, (uint32_t) pi.index, &controlValues[(size_t) pi.index]);
        } else if (pi.audio && pi.input) {
          float* bufs[4] = {inL.data(), inR.data(), scL.data(), scR.data()};
          descriptor->connect_port(h, (uint32_t) pi.index, bufs[inSeen < 4 ? inSeen : 3]);
          ++inSeen;
        } else if (pi.audio && pi.output) {
          float* dst = outSeen == 0   ? outL.data()
                       : outSeen == 1 ? outR.data()
                                      : auxOut[(size_t) (outSeen - 2)].data();
          descriptor->connect_port(h, (uint32_t) pi.index, dst);
          ++outSeen;
        } else if (pi.atom && pi.output) {
          descriptor->connect_port(h, (uint32_t) pi.index, outSeq);
        } else if (pi.atom) {
          descriptor->connect_port(h, (uint32_t) pi.index, seq);
        }
      }
      if (descriptor->activate) descriptor->activate(h);

      for (uint32_t frames : blocks) {
        uint32_t lcg = 99u;
        for (int b = 0; b < 30; ++b) {
          for (uint32_t i = 0; i < frames; ++i) {
            lcg = lcg * 1664525u + 1013904223u;
            inL[i] = inR[i] = (float) ((int32_t) (lcg >> 8) % 20001 - 10000) / 40000.0f;
          }
          descriptor->run(h, frames);
          for (uint32_t i = 0; i < frames; ++i) {
            if (!std::isfinite(outL[i]) || !std::isfinite(outR[i])) allFinite = false;
            const double a = std::fabs((double) outL[i]);
            if (a > worst) worst = a;
            // A huge but representable value reaches the user as a burst loud
            // enough to damage monitors; a NaN at least gets muted.
            if (a > 100.0) allSane = false;
          }
        }
      }
      if (descriptor->deactivate) descriptor->deactivate(h);
      descriptor->cleanup(h);
    }

    std::printf("  ---- 8 k to 192 kHz, blocks of 1, 7 and %u: worst |out| %.3g ----\n",
                (unsigned) kFrames, worst);
    check(allInstantiated, "the plugin instantiates at every rate a host might use");
    check(allFinite, "…and produces finite audio at all of them");
    check(allSane, "…that never blows past sanity, whatever the rate");
  }

  // ── lv2:freeWheeling ──────────────────────────────────────────────────────
  //
  // LV2's way of saying what clap.render and VST3's processMode say: the host
  // is rendering faster than real time, so a DSP that trades quality for CPU
  // can stop trading. The SDK grew that flag and wired it into two formats;
  // this port is the third, and without it an LV2 bounce silently got the
  // monitoring-quality render.
  //
  // The interesting half, exactly as in CLAP: a DSP using the SIMPLE
  // process() signature has no context to read the flag from and only learns
  // of it because the wrapper re-runs prepare(). The probe reads spec.offline
  // in prepare() and adds 0.125 to its telemetry channel, a value none of the
  // other terms in that sum can produce or cancel.
  {
    int freeWheelIdx = -1;
    for (const PortInfo& p : ports)
      if (p.symbol == "freewheel") freeWheelIdx = p.index;
    check(freeWheelIdx >= 0, "an lv2:freeWheeling port exists");

    if (freeWheelIdx >= 0 && expectProbe) {
      auto telemetry = [&]() {
        for (uint32_t i = 0; i < kFrames; ++i) inL[i] = inR[i] = 0.0f;
        descriptor->run(handle, kFrames);
        return (double) outR[kFrames - 1];
      };
      controlValues[(size_t) freeWheelIdx] = 0.0f;
      telemetry();
      const double live = telemetry();

      controlValues[(size_t) freeWheelIdx] = 1.0f;
      telemetry();
      const double offline = telemetry();

      std::printf("  ---- probe telemetry: live %.4f, freewheeling %.4f ----\n", live, offline);
      check(std::fabs((offline - live) - 0.125) < 1e-4,
            "the freewheel flag reached a DSP that never sees a ProcessContext");

      controlValues[(size_t) freeWheelIdx] = 0.0f;
      telemetry();
      check(std::fabs(telemetry() - live) < 1e-4, "…and dropping it puts the plugin back");
    }
  }

  // ── Host bypass (lv2:enabled) and latency (lv2:latency) ───────────────────
  // The contract, not the metadata: drive enabled to 0 and the output must
  // become the INPUT, aligned to the latency the plugin itself publishes on
  // its designated output port.
  if (!instrument && !expectMidiOut) {
    int enabledIdx = -1, latencyIdx = -1;
    for (const PortInfo& p : ports) {
      // By SYMBOL, not by position. This used to take "the last control input
      // port", which was lv2:enabled right up until lv2:freeWheeling was added
      // after it -- and then the bypass contract check was quietly driving the
      // freewheel port instead. A host looks a port up by its symbol; so does
      // this now.
      if (p.control && p.input && p.symbol == "enabled") enabledIdx = p.index;
      if (p.control && p.output && p.symbol == "latency") latencyIdx = p.index;
    }
    check(enabledIdx >= 0, "an lv2:enabled control port exists");
    // Only a DSP that declares latencySamples() grows the lv2:latency port --
    // the saturator (oversampled) does, the ducker does not.
    // Only a DSP that declares latencySamples() grows the port; among the
    // examples that is the oversampled saturator, not the ducker or splitter.
    if (!expectSidechain && expectAuxOuts <= 0)
      check(latencyIdx >= 0, "an lv2:latency output port exists (this DSP delays)");
    if (enabledIdx >= 0) {
      int L = 0;
      if (latencyIdx >= 0) {
        descriptor->run(handle, kFrames);
        L = (int) controlValues[(size_t) latencyIdx];
        check(L > 0, "a delaying DSP publishes a non-zero lv2:latency");
        // The probe is skipped below for the same reason the CLAP and VST3
        // tests skip it: it declares 64 samples to prove the number travels
        // and delays nothing, and its right channel is telemetry rather than
        // audio. Measuring either on a fixture measures the fixture.

        // ...and that it is non-zero says nothing about whether it is RIGHT.
        // The check below this one proves the wrapper's BYPASS path is aligned
        // to L, which is a different claim: it would still pass if the plugin
        // published a latency its active path did not actually have. That is
        // not hypothetical: the convolution reverb declared 512 samples while
        // its dry signal came out at lag zero, and only a measurement of the
        // ACTIVE path found it.
        //
        // Broadband noise, because a periodic input correlates just as well at
        // every multiple of its period and so cannot tell 0 from 48.
        // The probe used to be skipped here, because it published 64 samples
        // of latency and delayed by none -- a fixture that cannot be checked
        // against is a fixture that hides exactly the bug this measures. It
        // delays properly now, so it is measured like everything else, and it
        // is the one plugin in the suite whose delay compensation is verified
        // rather than assumed.
        {
        const size_t captureLen = 8192;
        const size_t maxLag = (size_t) L * 2 + 512;
        std::vector<float> sent, got;
        unsigned lcg = 12345u;
        while (got.size() < captureLen + maxLag) {
          for (uint32_t i = 0; i < kFrames; ++i) {
            lcg = lcg * 1664525u + 1013904223u;
            const float v = (float) ((int) (lcg >> 8) % 20001 - 10000) / 40000.0f;
            inL[i] = inR[i] = v;
            sent.push_back(v);
          }
          descriptor->run(handle, kFrames);
          for (uint32_t i = 0; i < kFrames; ++i) got.push_back(outL[i]);
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
        std::printf("  ---- latency: published %d samples, measured %u (r=%.3f) ----\n", L,
                    (unsigned) bestLag, best);
        check(best > 0.2, "the output correlates with the input at SOME lag");
        const size_t err = bestLag > (size_t) L ? bestLag - (size_t) L : (size_t) L - bestLag;
        check(err <= 32, "the published latency is the delay the plugin ACTUALLY has");
        }
      }

      controlValues[(size_t) enabledIdx] = 0.0f; // enabled <= 0 means bypassed
      double maxErr = 0.0;
      int ph = 0;
      for (int block = 0; block < 40; ++block) {
        for (uint32_t i = 0; i < kFrames; ++i) {
          inL[i] = inR[i] =
              0.25f * (float) std::sin(2.0 * 3.14159265358979 * 997.0 * ph / 48000.0);
          ++ph;
        }
        descriptor->run(handle, kFrames);
        if (block >= 20) { // the 20 ms crossfade is long over
          for (uint32_t i = 0; i < kFrames; ++i) {
            const int n = ph - (int) kFrames + (int) i - L;
            const double expect = 0.25 * std::sin(2.0 * 3.14159265358979 * 997.0 * n / 48000.0);
            const double err = std::fabs((double) outL[i] - expect);
            if (err > maxErr) maxErr = err;
          }
        }
      }
      if (!expectProbe)
        check(maxErr < 1e-3, "bypassed output IS the input, latency-aligned");

      controlValues[(size_t) enabledIdx] = 1.0f; // back on
      // A ducker's processing is only audible when its key is hot.
      if (expectSidechain)
        for (uint32_t i = 0; i < kFrames; ++i) scL[i] = scR[i] = 0.9f;
      double wetDiff = 0.0;
      for (int block = 0; block < 20; ++block) {
        for (uint32_t i = 0; i < kFrames; ++i) {
          inL[i] = inR[i] =
              0.25f * (float) std::sin(2.0 * 3.14159265358979 * 997.0 * ph / 48000.0);
          ++ph;
        }
        descriptor->run(handle, kFrames);
        if (block >= 15) {
          for (uint32_t i = 0; i < kFrames; ++i) {
            const int n = ph - (int) kFrames + (int) i - L;
            const double dry = 0.25 * std::sin(2.0 * 3.14159265358979 * 997.0 * n / 48000.0);
            const double err = std::fabs((double) outL[i] - dry);
            if (err > wetDiff) wetDiff = err;
          }
        }
      }
      // Not asked of the probe, and this reason is a real one rather than the
      // old one: its audio path is a unity-gain delay BY DESIGN, so an
      // unbypassed output is the latency-aligned input and "processing" is
      // indistinguishable from not. Every other plugin here changes its
      // signal, and for those this is the check that the bypass actually let
      // go again.
      if (!expectProbe) check(wetDiff > 1e-3, "…and re-enabling brings the processing back");
      if (expectSidechain)
        for (uint32_t i = 0; i < kFrames; ++i) scL[i] = scR[i] = 0.0f;
    }
  }

  // ── Emitted MIDI ──────────────────────────────────────────────────────────
  if (expectMidiOut) {
    check(atomOutSeen == 1, "the TTL declares an atom:Sequence OUTPUT port");
    int noteOns = 0, noteOffs = 0;
    bool framesInBlock = true, typedCorrectly = true;
    const LV2_URID midiEvent = UridMap::mapCb(&urids, LV2_MIDI__MidiEvent);
    for (int block = 0; block < 40; ++block) {
      // Reset the capacity every run, exactly as a host does.
      outSeq->atom.size = (uint32_t) (sizeof(midiOutBuffer) - sizeof(LV2_Atom));
      outSeq->atom.type = UridMap::mapCb(&urids, LV2_ATOM__Sequence);
      descriptor->run(handle, kFrames);
      const auto* begin = (const uint8_t*) &outSeq->body + sizeof(LV2_Atom_Sequence_Body);
      const auto* end = (const uint8_t*) &outSeq->body + outSeq->atom.size;
      for (const uint8_t* q = begin; q + sizeof(LV2_Atom_Event) <= end;) {
        const auto* ev = (const LV2_Atom_Event*) q;
        const uint8_t* body = q + sizeof(LV2_Atom_Event);
        if (body + ev->body.size > end) break;
        if (ev->body.type != midiEvent) typedCorrectly = false;
        if (ev->time.frames < 0 || ev->time.frames >= (int64_t) kFrames) framesInBlock = false;
        if (ev->body.size >= 3) {
          if ((body[0] & 0xf0) == 0x90 && body[2] > 0) ++noteOns;
          if ((body[0] & 0xf0) == 0x80) ++noteOffs;
        }
        q += sizeof(LV2_Atom_Event) + ((ev->body.size + 7u) & ~7u);
      }
    }
    check(noteOns > 1, "the note effect emits note-ons into the sequence");
    check(noteOffs > 0, "…and releases them again");
    check(typedCorrectly, "…every event typed as midi:MidiEvent");
    check(framesInBlock, "…with frame stamps inside the block");

    // ── SysEx through LV2, and the alignment it depends on ──────────────────
    //
    // An LV2 atom body is padded to a multiple of eight, and the pad is part
    // of what the NEXT event's offset is measured from. Getting that wrong
    // does not truncate the message -- it misaligns everything after it, and
    // the symptom is a note that vanishes rather than a SysEx that looks
    // broken.
    //
    // So the input is a SysEx of DELIBERATELY AWKWARD length followed by a
    // note-on, and the check is that both come out. Nothing had ever run this
    // code: the wrapper learned to emit SysEx and no test sent one.
    {
      const LV2_URID midiEvent = UridMap::mapCb(&urids, LV2_MIDI__MidiEvent);
      sonore::midici::DeviceIdentity identity;
      identity.muid = 0x0ABCDEF;
      identity.manufacturer[0] = 0x7D;
      const std::vector<uint8_t> discovery = sonore::midici::encodeDiscovery(identity);

      // A buffer big enough for the SysEx event plus a note event after it.
      std::vector<uint8_t> inBytes(sizeof(LV2_Atom_Sequence) + discovery.size() + 128, 0);
      auto* inSeq = (LV2_Atom_Sequence*) inBytes.data();
      inSeq->atom.type = UridMap::mapCb(&urids, LV2_ATOM__Sequence);
      inSeq->body.unit = 0;
      inSeq->body.pad = 0;
      uint32_t used = (uint32_t) sizeof(LV2_Atom_Sequence_Body);
      auto* base = (uint8_t*) &inSeq->body;

      // SEVEN bytes first, and the length is the whole point.
      //
      // An atom body is padded to a multiple of eight, so a message whose
      // length already IS a multiple of eight pads to itself and proves
      // nothing. The MIDI-CI discovery message is exactly 32 bytes -- which
      // is why the first two versions of this test passed with the padding
      // deliberately broken. Measured, after assuming otherwise twice.
      const std::vector<uint8_t> odd = {0xF0, 0x7D, 0x11, 0x22, 0x33, 0x44, 0xF7};
      auto* oddEvent = (LV2_Atom_Event*) (base + used);
      oddEvent->time.frames = 0;
      oddEvent->body.size = (uint32_t) odd.size();
      oddEvent->body.type = midiEvent;
      std::memcpy((uint8_t*) oddEvent + sizeof(LV2_Atom_Event), odd.data(), odd.size());
      used += (uint32_t) sizeof(LV2_Atom_Event) + (((uint32_t) odd.size() + 7u) & ~7u);

      // And the discovery AFTER it, so where it lands depends on that pad.
      auto* sysexEvent = (LV2_Atom_Event*) (base + used);
      sysexEvent->time.frames = 1;
      sysexEvent->body.size = (uint32_t) discovery.size();
      sysexEvent->body.type = midiEvent;
      std::memcpy((uint8_t*) sysexEvent + sizeof(LV2_Atom_Event), discovery.data(),
                  discovery.size());
      used += (uint32_t) sizeof(LV2_Atom_Event) + (((uint32_t) discovery.size() + 7u) & ~7u);

      // A SECOND SysEx, and this is the one that tests the alignment.
      //
      // The first version of this check sent a control change after the
      // SysEx and asserted it still arrived. It always did -- and it would
      // have arrived with the padding deliberately broken too, because the
      // wrapper writes short messages BEFORE SysEx, so the CC came out first
      // and nothing ever followed a SysEx's pad. Verified by breaking the
      // pad on purpose: all three checks still passed.
      //
      // Two SysEx messages, the first of a length that is NOT a multiple of
      // eight, is what actually depends on the pad: it decides where the
      // second one begins.
      inSeq->atom.size = used;

      // Point the plugin at this sequence for one run.
      descriptor->connect_port(handle, (uint32_t) portMidiIn, inSeq);
      outSeq->atom.size = (uint32_t) (sizeof(midiOutBuffer) - sizeof(LV2_Atom));
      outSeq->atom.type = UridMap::mapCb(&urids, LV2_ATOM__Sequence);
      descriptor->run(handle, kFrames);
      descriptor->connect_port(handle, (uint32_t) portMidiIn, seq);

      bool sysexBack = false, secondBack = false, parses = false;
      const auto* begin = (const uint8_t*) &outSeq->body + sizeof(LV2_Atom_Sequence_Body);
      const auto* end = (const uint8_t*) &outSeq->body + outSeq->atom.size;
      for (const uint8_t* q = begin; q + sizeof(LV2_Atom_Event) <= end;) {
        const auto* ev = (const LV2_Atom_Event*) q;
        const uint8_t* body = q + sizeof(LV2_Atom_Event);
        if (body + ev->body.size > end) break;
        if (ev->body.size == discovery.size() &&
            std::equal(body, body + ev->body.size, discovery.begin())) {
          sysexBack = true;
          sonore::midici::Message decoded;
          if (sonore::midici::decode(body, ev->body.size, &decoded) &&
              decoded.type == sonore::midici::MessageType::Discovery)
            parses = true;
        }
        if (ev->body.size == odd.size() && std::equal(body, body + ev->body.size, odd.begin()))
          secondBack = true;
        q += sizeof(LV2_Atom_Event) + ((ev->body.size + 7u) & ~7u);
      }
      check(sysexBack, "…a SysEx sent in comes back out of the atom sequence");
      check(parses,
            "…and still decodes as MIDI-CI -- it sits AFTER a seven-byte message, so "
            "finding it at all is what the atom padding decides");
      check(secondBack, "…the seven-byte SysEx in front of it survives too");
    }
  }

  // ── Aux output buses ──────────────────────────────────────────────────────
  if (expectAuxOuts > 0) {
    check(audioOutSeen == 2 + expectAuxOuts * 2,
          "the TTL declares the main bus plus its aux channels");
    auto energies = [&](double hz) {
      static int ph = 0;
      double mainE = 0.0, lastAuxE = 0.0;
      const size_t lastAux = (size_t) ((expectAuxOuts - 1) * 2);
      for (int block = 0; block < 30; ++block) {
        for (uint32_t i = 0; i < kFrames; ++i) {
          inL[i] = inR[i] =
              0.5f * (float) std::sin(2.0 * 3.14159265358979 * hz * ph / 48000.0);
          ++ph;
        }
        descriptor->run(handle, kFrames);
        if (block >= 15)
          for (uint32_t i = 0; i < kFrames; ++i) {
            mainE += (double) outL[i] * outL[i];
            const float a = auxOut[lastAux][i];
            lastAuxE += (double) a * a;
          }
      }
      return std::pair<double, double>(mainE, lastAuxE);
    };
    const auto low = energies(60.0);
    check(low.first > 1e-3, "a low tone reaches the MAIN ports");
    check(low.second < low.first * 0.01, "…and stays off the high aux ports");
    const auto high = energies(12000.0);
    check(high.second > 1e-3, "a high tone reaches the AUX ports");
    check(high.first < high.second * 0.01, "…and stays off the main ports");
  }

  // ── Sidechain ─────────────────────────────────────────────────────────────
  if (expectSidechain) {
    auto measure = [&](float key) {
      for (uint32_t i = 0; i < kFrames; ++i) {
        scL[i] = scR[i] = key;
      }
      double e = 0.0;
      int ph = 0;
      for (int block = 0; block < 40; ++block) {
        for (uint32_t i = 0; i < kFrames; ++i) {
          inL[i] = inR[i] =
              0.25f * (float) std::sin(2.0 * 3.14159265358979 * 440.0 * ph / 48000.0);
          ++ph;
        }
        descriptor->run(handle, kFrames);
        if (block >= 20)
          for (uint32_t i = 0; i < kFrames; ++i) e += (double) outL[i] * outL[i];
      }
      return e;
    };
    const double quietKey = measure(0.0f);
    const double hotKey = measure(0.9f);
    for (uint32_t i = 0; i < kFrames; ++i) scL[i] = scR[i] = 0.0f;
    check(quietKey > 0.0, "with a silent key the main signal passes");
    check(hotKey < quietKey * 0.5,
          "a hot key ducks the main signal (the sidechain reaches the DSP)");
  }

  // ── State round-trip through the declared interface ───────────────────────
  const auto* state =
      (const LV2_State_Interface*) descriptor->extension_data(LV2_STATE__interface);
  check(state != nullptr, "the state interface is reachable");
  if (state) {
    // Move control 0 off its default, save, verify the blob exists.
    controlValues[0] = controlValues[0] + 0.25f;
    descriptor->run(handle, kFrames); // controls are read in run()
    StateStore store;
    check(state->save(handle, StateStore::storeCb, &store, 0, features) == LV2_STATE_SUCCESS,
          "state saves");
    check(!store.values.empty(), "…and stored a blob");
    check(state->restore(handle, StateStore::retrieveCb, &store, 0, features) ==
              LV2_STATE_SUCCESS,
          "state restores");

    // The blob must carry the FULL session, not just the parameter values. LV2
    // used to write only the 12-byte SNRS header + one float per parameter,
    // while stamping the current version number -- so a host dropped the DSP's
    // StateBag (a sampler's loaded file), the bypass state, the selected preset
    // and the editor size on every reload. The complete blob adds at least a
    // bypass byte + a preset int + an editor-size pair (13 bytes) beyond the
    // params, so it is strictly larger than a params-only blob would be. Count
    // the control-INPUT ports as an over-estimate of the parameter count (it
    // also counts the bypass and freewheel ports, at most 3 extra), and 13 > 12
    // keeps the inequality true even then.
    size_t controlInputs = 0;
    for (const PortInfo& p : ports)
      if (p.control && p.input) ++controlInputs;
    // store.values is keyed by property URID; the blob is one entry, so its
    // byte length is the single value's size, not the map's entry count.
    const size_t blobBytes = store.values.empty() ? 0 : store.values.begin()->second.size();
    const size_t paramsOnly = 12 /* sizeof SNRS header */ + sizeof(float) * controlInputs;
    check(blobBytes > paramsOnly,
          "the saved blob carries the whole session (bag/bypass/preset/editor), not just params");
  }

  // \u2500\u2500 The interface's atom protocol \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
  //
  // An LV2 interface is a separate module and reaches the plugin only through
  // ports, so anything structured -- which file is loaded, what a waveform
  // looks like -- travels as an atom. Both directions matter and neither
  // covers the other: an interface opened AFTER the plugin has missed every
  // change and has to ask, and one already open has to be told when something
  // moves.
  //
  // Driven here exactly as a host drives it: the request goes into the input
  // sequence, run() happens, and the answer is read out of the output one.
  {
    const LV2_URID uridSequence = map.map(map.handle, LV2_ATOM__Sequence);
    const LV2_URID uridRequest = map.map(map.handle, "urn:sonorie:ui:stateRequest");
    const LV2_URID uridJson = map.map(map.handle, "urn:sonorie:ui:stateJson");

    // A request, written the way the interface writes one.
    controlSeq->atom.type = uridSequence;
    controlSeq->atom.size = (uint32_t) (sizeof(LV2_Atom_Sequence_Body) + sizeof(LV2_Atom_Event));
    controlSeq->body.unit = 0;
    controlSeq->body.pad = 0;
    auto* req = (LV2_Atom_Event*) ((uint8_t*) &controlSeq->body + sizeof(LV2_Atom_Sequence_Body));
    req->time.frames = 0;
    req->body.size = 0;
    req->body.type = uridRequest;

    // The host sets the OUTPUT sequence's size to the buffer's capacity
    // before each run; the plugin overwrites it with what it wrote.
    notifySeq->atom.size = (uint32_t) sizeof(uiNotifyBuffer) - (uint32_t) sizeof(LV2_Atom);
    notifySeq->atom.type = uridSequence;
    descriptor->run(handle, kFrames);

    std::string json;
    const auto* body = (const uint8_t*) &notifySeq->body;
    uint32_t offset = (uint32_t) sizeof(LV2_Atom_Sequence_Body);
    while (offset + sizeof(LV2_Atom_Event) <= notifySeq->atom.size) {
      const auto* ev = (const LV2_Atom_Event*) (body + offset);
      if (ev->body.type == uridJson)
        json.assign((const char*) ev + sizeof(LV2_Atom_Event), ev->body.size);
      const uint32_t step = (uint32_t) sizeof(LV2_Atom_Event) + ((ev->body.size + 7u) & ~7u);
      if (step == 0) break;
      offset += step;
    }

    std::printf("  ---- answer to a state request: %s ----\n",
                json.empty() ? "(nothing)" : json.c_str());
    check(!json.empty(), "asking over the atom port gets an answer");
    check(json.front() == '{' && json.back() == '}',
          "\u2026and the answer is the object literal a page can be handed");

    // Asked once, answered once. A plugin that re-sent it every block would
    // put an eval on the interface's clock for ever, which is the version of
    // this that looks fine and makes a DAW feel slow.
    controlSeq->atom.size = (uint32_t) sizeof(LV2_Atom_Sequence_Body); // no request
    notifySeq->atom.size = (uint32_t) sizeof(uiNotifyBuffer) - (uint32_t) sizeof(LV2_Atom);
    descriptor->run(handle, kFrames);
    check(notifySeq->atom.size == (uint32_t) sizeof(LV2_Atom_Sequence_Body),
          "\u2026and nothing is sent again until something changes or asks");

    // \u2500\u2500 A file the interface picked \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
    //
    // LV2's plugin side has no main thread. run() is the audio callback, so
    // reading a file there is a dropout with a plausible excuse -- and the
    // host is the only thing that owns a thread to do it on. The plugin must
    // therefore SCHEDULE the work rather than do it, and this checks that it
    // does: the request goes in as an atom, and the host's worker is what
    // ends up being called.
    const LV2_URID uridLoadFile = map.map(map.handle, "urn:sonorie:ui:loadFile");
    const char payload[] = "sample\0C:/kits/kick.wav";
    const uint32_t payloadSize = (uint32_t) sizeof(payload) - 1;

    controlSeq->atom.type = uridSequence;
    controlSeq->body.unit = 0;
    controlSeq->body.pad = 0;
    auto* load = (LV2_Atom_Event*) ((uint8_t*) &controlSeq->body + sizeof(LV2_Atom_Sequence_Body));
    load->time.frames = 0;
    load->body.size = payloadSize;
    load->body.type = uridLoadFile;
    std::memcpy((uint8_t*) load + sizeof(LV2_Atom_Event), payload, payloadSize);
    controlSeq->atom.size = (uint32_t) (sizeof(LV2_Atom_Sequence_Body) + sizeof(LV2_Atom_Event) +
                                        ((payloadSize + 7u) & ~7u));

    const int scheduledBefore = g_worker.scheduled;
    notifySeq->atom.size = (uint32_t) sizeof(uiNotifyBuffer) - (uint32_t) sizeof(LV2_Atom);
    descriptor->run(handle, kFrames);
    std::printf("  ---- a file was %s ----\n",
                g_worker.scheduled > scheduledBefore ? "handed to the worker thread"
                                                     : "NOT scheduled");
    check(g_worker.scheduled > scheduledBefore,
          "a file to load is handed to the host's worker, never read in run()");
    check(g_worker.responded > 0, "\u2026and the worker answers when it is done");

    // \u2500\u2500 A sequence full of rubbish \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
    //
    // The control port is memory a HOST fills in, and a plugin that trusts it
    // crashes in somebody's session rather than in a test. An event claiming
    // more bytes than the sequence holds is the shape that matters: read
    // naively, the walk steps into whatever follows and reads a size out of
    // it.
    //
    // What is checked is that run() RETURNS and the plugin still works
    // afterwards. There is no assertion about what it made of the rubbish,
    // because the honest answer is that it should make nothing of it.
    {
      controlSeq->atom.type = uridSequence;
      controlSeq->body.unit = 0;
      controlSeq->body.pad = 0;
      auto* bad =
          (LV2_Atom_Event*) ((uint8_t*) &controlSeq->body + sizeof(LV2_Atom_Sequence_Body));
      bad->time.frames = 0;
      bad->body.type = uridRequest;
      bad->body.size = 0xFFFFFF00u; // far past the end, and near enough to
                                    // wrap a 32-bit offset+size comparison
      controlSeq->atom.size =
          (uint32_t) (sizeof(LV2_Atom_Sequence_Body) + sizeof(LV2_Atom_Event) + 8);

      notifySeq->atom.size = (uint32_t) sizeof(uiNotifyBuffer) - (uint32_t) sizeof(LV2_Atom);
      descriptor->run(handle, kFrames);
      check(true, "a sequence whose event runs past the end does not hang the plugin");

      // ...and it is still alive. A walk that ran off the end could leave the
      // plugin in a state where the next block is the one that crashes.
      controlSeq->atom.size = (uint32_t) sizeof(LV2_Atom_Sequence_Body);
      notifySeq->atom.size = (uint32_t) sizeof(uiNotifyBuffer) - (uint32_t) sizeof(LV2_Atom);
      descriptor->run(handle, kFrames);
      bool finiteAfter = true;
      for (uint32_t i = 0; i < kFrames; ++i)
        if (!std::isfinite(outL[i]) || !std::isfinite(outR[i])) finiteAfter = false;
      check(finiteAfter, "\u2026and the block after it is still audio");
    }

    // \u2500\u2500 Which keys are sounding \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500
    //
    // The last thing an LV2 interface could not see that the others could. It
    // cannot travel on a control port: a 32-bit mask does not survive a
    // float's 24-bit mantissa, so it rides the same atom port as everything
    // else structured.
    //
    // Only checked on a plugin that takes notes -- an effect never has a key
    // down, and asserting an empty keyboard would pass whether the mechanism
    // worked or not.
    if (instrument) {
      auto* noteSeq = seq; // the MIDI input the host already connected
      noteSeq->atom.type = uridSequence;
      noteSeq->body.unit = 0;
      noteSeq->body.pad = 0;
      auto* on = (LV2_Atom_Event*) ((uint8_t*) &noteSeq->body + sizeof(LV2_Atom_Sequence_Body));
      on->time.frames = 0;
      on->body.size = 3;
      on->body.type = UridMap::mapCb(&urids, LV2_MIDI__MidiEvent);
      uint8_t* midi = (uint8_t*) on + sizeof(LV2_Atom_Event);
      midi[0] = 0x90;
      midi[1] = 64; // E4: bit 0 of word 2, so a wrong word split shows up
      midi[2] = 100;
      // ...and a note-off for the key the test has been holding since the
      // first block. Without it the mask carries two keys and this would be
      // asserting the arithmetic of a note nobody meant to include -- which
      // is what the first version of this check did, and it was wrong about
      // which one was the mistake.
      auto* off = (LV2_Atom_Event*) ((uint8_t*) on + sizeof(LV2_Atom_Event) + 8);
      off->time.frames = 0;
      off->body.size = 3;
      off->body.type = UridMap::mapCb(&urids, LV2_MIDI__MidiEvent);
      uint8_t* offMidi = (uint8_t*) off + sizeof(LV2_Atom_Event);
      offMidi[0] = 0x80;
      offMidi[1] = 69;
      offMidi[2] = 0;
      noteSeq->atom.size =
          (uint32_t) (sizeof(LV2_Atom_Sequence_Body) + 2 * (sizeof(LV2_Atom_Event) + 8));

      const LV2_URID uridNotes = map.map(map.handle, "urn:sonorie:ui:notes");
      notifySeq->atom.size = (uint32_t) sizeof(uiNotifyBuffer) - (uint32_t) sizeof(LV2_Atom);
      descriptor->run(handle, kFrames);

      uint32_t words[4] = {0, 0, 0, 0};
      bool sawNotes = false;
      const auto* nbody = (const uint8_t*) &notifySeq->body;
      uint32_t noff = (uint32_t) sizeof(LV2_Atom_Sequence_Body);
      while (noff + sizeof(LV2_Atom_Event) <= notifySeq->atom.size) {
        const auto* ev = (const LV2_Atom_Event*) (nbody + noff);
        if (ev->body.type == uridNotes && ev->body.size == sizeof(words)) {
          std::memcpy(words, (const uint8_t*) ev + sizeof(LV2_Atom_Event), sizeof(words));
          sawNotes = true;
        }
        const uint32_t step = (uint32_t) sizeof(LV2_Atom_Event) + ((ev->body.size + 7u) & ~7u);
        if (step == 0) break;
        noff += step;
      }
      std::printf("  ---- keyboard: %u %u %u %u ----\n", words[0], words[1], words[2],
                  words[3]);
      check(sawNotes, "a held note reaches the interface as a keyboard mask");
      // Key 64 is bit 0 of word 2, which is what a wrong word split would
      // get wrong. Key 69 is bit 5 of the same word and is ALSO down: this
      // host has been re-sending its note-on every block since the start,
      // and the mask COUNTS rather than flags, so the key stays down
      // however many note-offs follow that many note-ons. Asserting the
      // word equals exactly one would be asserting the test's own history.
      check((words[2] & 1u) != 0u, "\u2026in the word that key belongs to");
      check(words[0] == 0u && words[1] == 0u && words[3] == 0u,
            "\u2026and no key outside it is lit");
    }
  }

  // ── The atom sequence and the control ports under mutation ─────────────────────
  //
  // LV2 hands a plugin a sequence and trusts it to walk it by the sizes the
  // host wrote. So: events whose body.size claims more than the buffer holds,
  // times before the block and past it, types that are not MIDI, MIDI bytes of
  // every kind including none, and a sequence whose own atom.size lies (kept
  // inside the buffer, because the plugin has no other bound to check against
  // -- an LV2 input atom has no capacity). Control ports read NaN, infinities
  // and numbers no knob can reach. After every run the output is finite.
  if (portMidiIn >= 0 || !ports.empty()) {
    std::mt19937 rng(0x1A70Fu);
    auto rnd = [&](uint32_t n) { return n ? (uint32_t) (rng() % n) : 0u; };
    const LV2_URID seqUrid = UridMap::mapCb(&urids, LV2_ATOM__Sequence);
    const LV2_URID midiUrid = UridMap::mapCb(&urids, LV2_MIDI__MidiEvent);
    alignas(8) uint8_t fuzzBuffer[1024];
    auto* fz = (LV2_Atom_Sequence*) fuzzBuffer;
    if (portMidiIn >= 0) descriptor->connect_port(handle, (uint32_t) portMidiIn, fz);
    const size_t cap = sizeof(fuzzBuffer) - sizeof(LV2_Atom);
    bool finiteAll = true;
    float peakAll = 0.0f;
    int runs = 0, fuzzedEvents = 0;
    for (int it = 0; it < 500; ++it) {
      fz->atom.type = rnd(6) == 0 ? (LV2_URID) rng() : seqUrid;
      fz->body.unit = rnd(2) ? 0 : (LV2_URID) rng();
      fz->body.pad = 0;
      size_t off = sizeof(LV2_Atom_Sequence_Body);
      const int n = (int) rnd(10);
      for (int k = 0; k < n; ++k) {
        if (off + sizeof(LV2_Atom_Event) + 16 > cap) break;
        auto* ev = (LV2_Atom_Event*) ((uint8_t*) &fz->body + off);
        ev->time.frames = rnd(3) == 0 ? (int64_t) rng() - 100 : (int64_t) rnd(kFrames + 1);
        const uint32_t len = rnd(4) == 0 ? (uint32_t) (rng() % 5000) : rnd(9);
        ev->body.size = len;
        ev->body.type = rnd(4) == 0 ? (LV2_URID) rng() : midiUrid;
        auto* bytes = (uint8_t*) ev + sizeof(LV2_Atom_Event);
        for (int b = 0; b < 8; ++b) bytes[b] = (uint8_t) rng();
        const uint32_t held = len < 8 ? len : 8;
        off += sizeof(LV2_Atom_Event) + ((held + 7u) & ~7u);
        ++fuzzedEvents;
      }
      fz->atom.size = rnd(5) == 0 ? (uint32_t) rnd((uint32_t) cap) : (uint32_t) off;
      for (const PortInfo& p : ports) {
        if (!p.control || !p.input || rnd(3) != 0) continue; // a host never writes an OUTPUT port
        float v;
        switch (rnd(6)) {
          case 0: v = std::numeric_limits<float>::quiet_NaN(); break;
          case 1: v = std::numeric_limits<float>::infinity(); break;
          case 2: v = -std::numeric_limits<float>::infinity(); break;
          case 3: v = 1e30f; break;
          case 4: v = -1e30f; break;
          default: v = (float) rng() / 4294967295.0f * 200.0f - 100.0f; break;
        }
        controlValues[(size_t) p.index] = v;
      }
      for (uint32_t i = 0; i < kFrames; ++i) {
        const float s = 0.25f * (float) std::sin(2.0 * 3.14159265358979 * 440.0 * i / 48000.0);
        inL[i] = s;
        inR[i] = s;
      }
      descriptor->run(handle, kFrames);
      for (uint32_t i = 0; i < kFrames; ++i) {
        if (!std::isfinite(outL[i]) || !std::isfinite(outR[i])) finiteAll = false;
        const float a = outL[i] < 0.0f ? -outL[i] : outL[i];
        if (a > peakAll) peakAll = a;
      }
      ++runs;
    }
    char note[200];
    std::snprintf(note, sizeof(note), "%d runs carrying %d mutated atom events and mutated "
                  "control values produced finite audio", runs, fuzzedEvents);
    check(finiteAll, note);
    check(peakAll < 100.0f, "...and bounded");
    // Back to the hand-built sequence and the TTL defaults for what follows.
    if (portMidiIn >= 0) descriptor->connect_port(handle, (uint32_t) portMidiIn, seq);
    for (const PortInfo& p : ports)
      if (p.control && p.input) controlValues[(size_t) p.index] = p.def;
  }

  if (descriptor->deactivate) descriptor->deactivate(handle);
  descriptor->cleanup(handle);

  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  if (g_failures == 0) std::printf("SONORE LV2 HOST TEST PASSED\n");
  return g_failures == 0 ? 0 : 1;
}
