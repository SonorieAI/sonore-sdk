// SPDX-License-Identifier: Apache-2.0
// Mutation fuzzing for every parser in the SDK that reads bytes it did not
// write: PNG, TrueType, SVG, zip, Standard MIDI files, FLAC, WAV, AIFF, MP3,
// Ogg, WAV and AIFF streamed from disk rather than loaded (the reader, the
// sample streamer's window and a waveform built from the file), JSON, OSC,
// the state bag, base64, the Turtle a host reads from every
// LV2 bundle on the machine, the atom sequences an LV2 host hands a plugin
// every block, the VST3 preset file and the plugin cache.
//
// A parser is given a REFERENCE input and then thousands of mutations of it:
// every truncation, random byte flips, runs of the bytes that break size
// arithmetic (0x00, 0xFF, 0x7F, 0x80), and truncation plus flips together.
// The verdict is not "did it parse" -- a mutated file is allowed to be
// refused -- but "did it come back": no crash, no hang, and under a sanitizer
// no report. A watchdog thread turns a hang into a named failure rather than
// a stuck CI job.
//
// Two ways to run it:
//
//   fuzz_parsers                       every target, a quick pass (ctest)
//   fuzz_parsers --iterations 4000     the deep pass, under ASan+UBSan
//                                      (scripts/sdk-sanitize.mjs)
//   fuzz_parsers --target svg          one parser
//
// The first deep run found three parsers that gave way -- an SVG path that
// looped forever, a rasteriser that segfaulted on the NaN the loop had been
// hiding, and a FLAC channel sum that overflowed -- and none of them had a
// reference file that could have shown it. Kept as a test so that stays true.
#include <sonore/audiofile.h>
#include <sonore/audiostream.h>
#include <sonore/sample_stream.h>
#include <sonore/waveform.h>
#include <sonore/gfx/bitmap.h>
#include <sonore/gfx/graphics.h>
#include <sonore/gfx/png.h>
#include <sonore/gfx/svg.h>
#include <sonore/gfx/text_shaper.h>
#include <sonore/gfx/truetype.h>
#include <sonore/json.h>
#include <sonore/lv2_atom.h>
#include <sonore/midi_ci.h>
#include <sonore/midi_file.h>
#include <sonore/midi_input.h>
#include <sonore/osc.h>
#include <sonore/plugin_cache.h>
#include <sonore/preset_file.h>
#include <sonore/state_bag.h>
#include <sonore/turtle.h>
#include <sonore/ump.h>
#include <sonore/zip.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifndef SONORE_TEST_DATA_DIR
#define SONORE_TEST_DATA_DIR "tests/data"
#endif

namespace {

// ── The watchdog ─────────────────────────────────────────────────────────────
//
// A parser that loops is the failure the other checks cannot see: no crash,
// no sanitizer report, just a process that never returns. Every case arms a
// deadline; a thread watches it and, if it passes, names the case and leaves
// with a distinct exit code. The offending bytes are written beside the
// process so the case can be replayed.

std::atomic<long long> g_deadlineMs{0};
std::string g_target;
const char* g_phase = "";
size_t g_case = 0;
std::vector<uint8_t> g_current;
std::atomic<bool> g_done{false};

long long nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void saveCurrent(const char* name) {
  if (FILE* f = std::fopen(name, "wb")) {
    if (!g_current.empty()) std::fwrite(g_current.data(), 1, g_current.size(), f);
    std::fclose(f);
  }
}

// A crash is the finding this tool exists for, and a finding nobody can
// replay is a rumour. The handler writes the bytes that were being parsed,
// names the case, and then hands the signal BACK to whoever had it before --
// under a sanitizer that is AddressSanitizer's own handler, which prints the
// stack; without one it is the default action, and the exit code says
// crash. The first version exited here itself, which under ASan produced a
// named case with no stack: the bytes without the why. (fopen in a signal
// handler is not async-signal-safe; in a fuzzer's crash path that is a
// trade worth making, and the worst case is a second crash that loses the
// file.)
using SignalHandler = void (*)(int);
SignalHandler g_previous[64] = {};

void onCrash(int sig) {
  saveCurrent("fuzz_parsers_crash.bin");
  char msg[200];
  const int n = std::snprintf(msg, sizeof(msg),
                              "  CRASH signal %d in %s phase=%s case=%zu size=%zu -- "
                              "the input is in fuzz_parsers_crash.bin\n",
                              sig, g_target.c_str(), g_phase, g_case, g_current.size());
  if (n > 0) std::fputs(msg, stderr);
  std::fflush(stderr);
  const SignalHandler previous = (sig >= 0 && sig < 64) ? g_previous[sig] : nullptr;
  if (previous && previous != SIG_DFL && previous != SIG_IGN) {
    std::signal(sig, previous);
    std::raise(sig); // the sanitizer's report and stack, then its exit
  }
  std::_Exit(98);
}

void watchCrashes() {
  const int signals[] = {SIGSEGV, SIGABRT, SIGILL, SIGFPE};
  for (int sig : signals) {
    const SignalHandler previous = std::signal(sig, onCrash);
    if (sig >= 0 && sig < 64) g_previous[sig] = previous == SIG_ERR ? nullptr : previous;
  }
}

void watchdog() {
  while (!g_done.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const long long deadline = g_deadlineMs.load();
    if (deadline != 0 && nowMs() > deadline) {
      std::fprintf(stderr, "  HANG %s phase=%s case=%zu size=%zu -- the input is in "
                           "fuzz_parsers_hang.bin\n",
                   g_target.c_str(), g_phase, g_case, g_current.size());
      saveCurrent("fuzz_parsers_hang.bin");
      std::fflush(stderr);
      std::_Exit(99);
    }
  }
}

// ── Files ────────────────────────────────────────────────────────────────────

bool readAll(const std::string& path, std::vector<uint8_t>& out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  out.resize((size_t) (n > 0 ? n : 0));
  const size_t got = out.empty() ? 0 : std::fread(out.data(), 1, out.size(), f);
  std::fclose(f);
  return got == out.size();
}

std::string g_tempPath;

void writeTemp(const std::vector<uint8_t>& b) {
  if (FILE* f = std::fopen(g_tempPath.c_str(), "wb")) {
    if (!b.empty()) std::fwrite(b.data(), 1, b.size(), f);
    std::fclose(f);
  }
}

// ── Seeds the harness makes itself ───────────────────────────────────────────
//
// The audio and image formats have reference files under tests/data. The
// text and container formats are cheaper to synthesise here, with the SDK's
// OWN writers where one exists, so the seed is a file the parser is meant to
// accept and every mutation is one step away from validity.

std::vector<uint8_t> bytesOf(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

// A WAV as a recorder or a library tool writes one, not as writeWav does:
// WAVE_FORMAT_EXTENSIBLE, 24-bit stereo, the metadata chunks real material
// carries before and after its audio -- bext, LIST/INFO, an empty JUNK, cue,
// smpl, acid -- and a pad byte after an odd-sized chunk. Every chunk header is
// a size the walk trusts, so this is where its arithmetic gets exercised.
std::vector<uint8_t> seedWavChunks() {
  std::vector<uint8_t> v;
  auto tag = [&v](const char* t) { v.insert(v.end(), t, t + 4); };
  auto u32 = [&v](uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back((uint8_t) (x >> (8 * i)));
  };
  auto u16 = [&v](uint32_t x) {
    v.push_back((uint8_t) x);
    v.push_back((uint8_t) (x >> 8));
  };
  auto chunk = [&](const char* t, const std::vector<uint8_t>& body) {
    tag(t);
    u32((uint32_t) body.size());
    v.insert(v.end(), body.begin(), body.end());
    if (body.size() & 1) v.push_back(0);
  };
  tag("RIFF");
  u32(0); // patched below
  tag("WAVE");
  tag("fmt ");
  u32(40);
  u16(0xFFFE);
  u16(2);
  u32(48000);
  u32(48000 * 6);
  u16(6);
  u16(24);
  u16(22);     // cbSize
  u16(24);     // valid bits
  u32(3);      // front left | front right
  u16(1);      // KSDATAFORMAT_SUBTYPE_PCM's first two bytes...
  const uint8_t guidTail[] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00,
                              0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
  v.insert(v.end(), guidTail, guidTail + sizeof(guidTail));
  chunk("bext", std::vector<uint8_t>(602, 0x20));
  chunk("JUNK", {});
  chunk("LIST", {'I', 'N', 'F', 'O', 'I', 'N', 'A', 'M', 5, 0, 0, 0, 'k', 'i', 'c', 'k', 0});
  const uint32_t frames = 300;
  tag("data");
  u32(frames * 6);
  for (uint32_t i = 0; i < frames * 2; ++i) {
    const int32_t s = (int32_t) (std::sin(0.05 * (double) i) * 6000000.0);
    v.push_back((uint8_t) s);
    v.push_back((uint8_t) (s >> 8));
    v.push_back((uint8_t) (s >> 16));
  }
  std::vector<uint8_t> cue = {1, 0, 0, 0};
  for (uint32_t x : {1u, 100u}) for (int i = 0; i < 4; ++i) cue.push_back((uint8_t) (x >> (8 * i)));
  cue.insert(cue.end(), {'d', 'a', 't', 'a', 0, 0, 0, 0, 0, 0, 0, 0, 100, 0, 0, 0});
  chunk("cue ", cue);
  std::vector<uint8_t> smpl(36 + 24, 0);
  smpl[28] = 1; // one loop
  smpl[44] = 50; // start
  smpl[48] = 250; // end
  chunk("smpl", smpl);
  chunk("acid", std::vector<uint8_t>(23, 1)); // odd: a pad byte follows
  const uint32_t riff = (uint32_t) v.size() - 8;
  for (int i = 0; i < 4; ++i) v[4 + (size_t) i] = (uint8_t) (riff >> (8 * i));
  return v;
}

std::vector<uint8_t> seedJson() {
  return bytesOf(
      "{\"a\":[1,2.5,-3e10,true,null,\"x\\u00e9\\ud83d\\ude00\\\"\\\\/\\b\\f\\n\\r\\t\"],"
      "\"b\":{\"c\":{\"d\":[[[]]]},\"e\":\"\\u0041\"},\"n\":1e308,\"m\":-0.0,\"z\":\"\"}");
}

// What a MIDI cable delivers: notes under running status, a clock byte in
// the middle of a message and of a SysEx, a system common message, a plain
// SysEx and a MIDI-CI Discovery -- which the parser hands on whole and the CI
// decoder then reads field by field.
std::vector<uint8_t> seedMidiStream() {
  std::vector<uint8_t> out = {0x90, 60, 100, 64, 90, 0xF8, 67, 80, 0x80, 60, 0,
                              0xF2, 0x10, 0x20, 0xB0, 7, 100, 0xF0, 0x43, 0x10, 0xF8,
                              0x4C, 0x00, 0xF7, 0xE0, 0x00, 0x40, 0xFE};
  const auto ci = sonore::midici::encodeDiscovery(sonore::midici::makeIdentity(0x0123456));
  out.insert(out.end(), ci.begin(), ci.end());
  out.push_back(0xC0);
  out.push_back(5);
  return out;
}

// The same traffic as UMP: MIDI 1.0 packets, a MIDI 2.0 note-on and control
// change, a system message, and the CI Discovery as SysEx7 packets on two
// groups at once -- the interleaving the assembler keeps apart.
std::vector<uint8_t> seedUmp() {
  std::vector<uint32_t> words;
  words.push_back(sonore::ump::fromMidi1(sonore::MidiMessage::noteOn(0, 60, 100)));
  words.push_back(sonore::ump::fromMidi1(sonore::MidiMessage::controlChange(1, 7, 90), 3));
  words.push_back(0x40903C00u); // MIDI 2.0 note-on, group 0, channel 0, note 60
  words.push_back(0xFFFF0000u); // ...velocity 65535
  words.push_back(0x40B00700u); // MIDI 2.0 control change 7
  words.push_back(0x80000000u);
  words.push_back(sonore::ump::fromSystem(0xF8));
  const auto ci = sonore::midici::encodeDiscovery(sonore::midici::makeIdentity(0x0123456));
  std::vector<uint32_t> g0, g1;
  sonore::ump::toSysex7(ci.data(), ci.size(), [&](const uint32_t* w, size_t n) {
    g0.insert(g0.end(), w, w + n);
  }, 0);
  sonore::ump::toSysex7(ci.data(), ci.size(), [&](const uint32_t* w, size_t n) {
    g1.insert(g1.end(), w, w + n);
  }, 1);
  for (size_t i = 0; i < g0.size() || i < g1.size(); i += 2) {
    if (i + 1 < g0.size()) words.insert(words.end(), {g0[i], g0[i + 1]});
    if (i + 1 < g1.size()) words.insert(words.end(), {g1[i], g1[i + 1]});
  }
  std::vector<uint8_t> out(words.size() * 4);
  std::memcpy(out.data(), words.data(), out.size());
  return out;
}

std::vector<uint8_t> seedOsc() {
  sonore::osc::Message m1;
  m1.address = "/sonore/param/drive";
  m1.arguments.push_back(sonore::osc::Argument::makeInt(7));
  m1.arguments.push_back(sonore::osc::Argument::makeFloat(0.5f));
  m1.arguments.push_back(sonore::osc::Argument::makeString("hello"));
  m1.arguments.push_back(sonore::osc::Argument::makeBlob({1, 2, 3, 4, 5}));
  sonore::osc::Message m2;
  m2.address = "/x";
  m2.arguments.push_back(sonore::osc::Argument::makeString("abc"));
  const std::vector<uint8_t> a = sonore::osc::encode(m1), b = sonore::osc::encode(m2);
  // A bundle around them, by hand: "#bundle", a timetag, then size-prefixed
  // elements -- which is also how the decoder's bundle path gets exercised.
  std::vector<uint8_t> out = bytesOf(std::string("#bundle\0", 8));
  for (int i = 0; i < 8; ++i) out.push_back(i == 7 ? 1 : 0);
  auto element = [&](const std::vector<uint8_t>& e) {
    const uint32_t n = (uint32_t) e.size();
    out.push_back((uint8_t) (n >> 24));
    out.push_back((uint8_t) (n >> 16));
    out.push_back((uint8_t) (n >> 8));
    out.push_back((uint8_t) n);
    out.insert(out.end(), e.begin(), e.end());
  };
  element(a);
  element(b);
  return out;
}

std::vector<uint8_t> seedBag() {
  sonore::StateBag b;
  b.setString("samplePath", "/a/b/c.wav");
  b.setInt("rootNote", 60);
  b.setDouble("gain", 0.5);
  const uint8_t bytes[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
  b.setBytes("ir", bytes, sizeof(bytes));
  std::vector<uint8_t> out;
  b.serialise(out);
  return out;
}

std::vector<uint8_t> seedBase64() {
  std::vector<uint8_t> raw;
  for (int k = 0; k < 3; ++k)
    for (int i = 0; i < 256; ++i) raw.push_back((uint8_t) i);
  return bytesOf(sonore::Base64::encode(raw.data(), raw.size()));
}

std::vector<uint8_t> seedVstPreset() {
  std::vector<uint8_t> comp(300);
  for (size_t i = 0; i < comp.size(); ++i) comp[i] = (uint8_t) (i * 7);
  sonore::writeVstPreset(g_tempPath.c_str(), "0123456789ABCDEF0123456789ABCDEF", comp);
  std::vector<uint8_t> out;
  readAll(g_tempPath, out);
  return out;
}

std::vector<uint8_t> seedCache() {
  return bytesOf("SONORE-PLUGIN-CACHE-1\nSCANNING\t/x/y.clap\nBLACKLIST\t/x/bad.clap\n"
                 "STAMP\t/x/a.clap\t1234\t5678\n"
                 "PLUGIN\t/x/a.clap\tclap\tcom.x.a\tA\tX\t1.0.0\taudio-effect\t0\n"
                 "PLUGIN\t/x/b.vst3\tvst3\tcom.x.b\tB \"q\"\tX\t2.0\tinstrument\t1\n");
}

// An atom:Sequence as a host fills one in: the header, then events of one,
// three and twelve bytes -- each body padded to eight, which is the arithmetic
// a reader gets wrong. `wrap` makes the middle event claim 0xFFFFFFFF bytes,
// the size that padded in 32 bits came back as a step of sixteen and passed
// every bounds check with a four-gigabyte body.
std::vector<uint8_t> seedAtomSequence(bool wrap) {
  std::vector<uint8_t> out;
  auto put32 = [&](uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back((uint8_t) (v >> (8 * i)));
  };
  auto event = [&](int64_t frames, uint32_t type, uint32_t size, uint32_t bodyBytes) {
    for (int i = 0; i < 8; ++i) out.push_back((uint8_t) ((uint64_t) frames >> (8 * i)));
    put32(size);
    put32(type);
    for (uint32_t i = 0; i < bodyBytes; ++i) out.push_back((uint8_t) (0x90 + i));
    while (out.size() % 8 != 0) out.push_back(0);
  };
  put32(0); // atom.size, filled in below
  put32(7); // atom.type: whatever the host mapped atom:Sequence to
  put32(0); // body.unit
  put32(0); // body.pad
  event(0, 3, 1, 1);
  event(16, 3, wrap ? 0xFFFFFFFFu : 3u, 3);
  event(32, 3, 12, 12);
  const uint32_t size = (uint32_t) (out.size() - 8);
  for (int i = 0; i < 4; ++i) out[(size_t) i] = (uint8_t) (size >> (8 * i));
  return out;
}

// The shape lv2_wrapper.h's ttlgen writes, trimmed: prefixes, a plugin with a
// project block, control ports with scale points, audio ports, an atom port,
// and a preset -- every construct the host's loader asks the parser for.
std::vector<uint8_t> seedTurtle() {
  return bytesOf(
      "@prefix atom:  <http://lv2plug.in/ns/ext/atom#> .\n"
      "@prefix doap:  <http://usefulinc.com/ns/doap#> .\n"
      "@prefix lv2:   <http://lv2plug.in/ns/lv2core#> .\n"
      "@prefix foaf:  <http://xmlns.com/foaf/0.1/> .\n"
      "@prefix pset:  <http://lv2plug.in/ns/ext/presets#> .\n"
      "@prefix rdf:   <http://www.w3.org/1999/02/22-rdf-syntax-ns#> .\n"
      "@prefix rdfs:  <http://www.w3.org/2000/01/rdf-schema#> .\n\n"
      "<urn:sonorie:com.example.fuzz>\n"
      "    a lv2:Plugin , lv2:DistortionPlugin ;\n"
      "    doap:name \"Fuzz \\\"Quoted\\\" Name\" ;\n"
      "    lv2:minorVersion 2 ;\n    lv2:microVersion 0 ;\n"
      "    doap:license <https://example.com/licence> ;\n"
      "    lv2:project [\n        a doap:Project ;\n        doap:name \"Fuzz\" ;\n"
      "        doap:maintainer [\n            a foaf:Person ;\n"
      "            foaf:name \"Somebody\" ;\n            foaf:homepage <https://example.com> ;\n"
      "        ] ;\n    ] ;\n"
      "    lv2:binary <Fuzz.so> ;\n"
      "    lv2:requiredFeature <http://lv2plug.in/ns/ext/urid#map> ;\n"
      "    lv2:port [\n"
      "        a lv2:InputPort , lv2:ControlPort ;\n        lv2:index 0 ;\n"
      "        lv2:symbol \"drive\" ;\n        lv2:name \"Drive\" ;\n"
      "        lv2:default 2 ;\n        lv2:minimum 1 ;\n        lv2:maximum 20 ;\n"
      "        lv2:portProperty lv2:integer , lv2:enumeration ;\n"
      "        lv2:scalePoint [\n            rdfs:label \"Soft\" ;\n            rdf:value 0\n"
      "        ] ;\n"
      "    ] , [\n"
      "        a lv2:InputPort , lv2:AudioPort ;\n        lv2:index 1 ;\n"
      "        lv2:symbol \"in_l\" ;\n        lv2:name \"In L\" ;\n"
      "    ] , [\n"
      "        a lv2:OutputPort , atom:AtomPort ;\n        atom:bufferType atom:Sequence ;\n"
      "        lv2:index 2 ;\n        lv2:symbol \"uiNotify\" ;\n        lv2:name \"UI Notify\" ;\n"
      "    ] .\n\n"
      "<urn:sonorie:com.example.fuzz#preset0>\n"
      "    a pset:Preset ;\n    lv2:appliesTo <urn:sonorie:com.example.fuzz> ;\n"
      "    rdfs:label \"Warm\" ;\n"
      "    lv2:port [\n        lv2:symbol \"drive\" ;\n        pset:value 2.5\n    ] .\n");
}

// Turtle no generator writes, which a host scanning other people's bundles
// still has to come back from: nesting at and past the parser's limit, a
// literal and a URI of 64 KB, every kind of escape (\u and \U, short, not
// hexadecimal, a surrogate, a backslash with nothing after it), and blank
// nodes that name each other and themselves.
std::vector<uint8_t> seedTurtleHostile(int which) {
  const std::string head =
      "@prefix lv2: <http://lv2plug.in/ns/lv2core#> .\n"
      "@prefix doap: <http://usefulinc.com/ns/doap#> .\n";
  std::string s = head;
  if (which == 0) {
    for (int depth : {8, 63, 64, 65, 200}) {
      s += "<urn:deep:" + std::to_string(depth) + "> a lv2:Plugin ; lv2:port ";
      for (int i = 0; i < depth; ++i) s += (i % 2) ? "( " : "[ lv2:index ";
      s += "0";
      for (int i = depth; i-- > 0;) s += (i % 2) ? " )" : " ]";
      s += " .\n";
    }
  } else if (which == 1) {
    s += "<urn:big> a lv2:Plugin ;\n  doap:name \"\"\"" + std::string(65536, 'x') + "\"\"\" ;\n";
    s += "  lv2:binary <" + std::string(65536, 'b') + ".so> ;\n";
    s += "  doap:shortdesc \"" + std::string(4096, '\\') + "\" .\n";
  } else if (which == 2) {
    // The escapes a parser must read first, then the ones it must refuse,
    // each in a statement of its own so a mutation can reach every one.
    s += "<urn:esc\\u00e9\\U0001F600> a lv2:Plugin ;\n"
         "  doap:name \"\\u00e9 \\U0010FFFF \\U00110000 \\uD800\\uDC00 \\q \\\" \\\\ \\t \\\n\" ;\n"
         "  lv2:port [ lv2:index \"\\u0030\" ; lv2:symbol 'single \\' quote' ] .\n"
         "<urn:bad1> doap:name \"\\uZZZZ\" .\n"
         "<urn:bad2> doap:name \"\\u12\" .\n"
         "<urn:bad3> doap:name \"\\U0001F6\" .\n"
         "<urn:bad4> doap:license <urn:\\u> .\n"
         "<urn:tail> doap:name \"ends in a backslash \\";
  } else {
    s += "_:a lv2:port _:b . _:b lv2:port _:a .\n"
         "_:self lv2:port _:self ; lv2:index 0 .\n"
         "<urn:cyc> a lv2:Plugin ; lv2:port _:a , _:self , [ lv2:port _:a ; lv2:index 1 ] ;\n"
         "  lv2:project _:p .\n_:p doap:maintainer _:p ; lv2:project <urn:cyc> .\n"
         "<urn:cyc> lv2:port ( _:a _:b ( _:self ) ) .\n";
  }
  return bytesOf(s);
}

// Two SVGs of our own: every path command, an arc, a relative segment, a
// transform, a gradient fill and a nested group -- the constructs a skin
// actually uses, in a file the parser is meant to accept whole.
std::vector<uint8_t> seedSvg(int which) {
  if (which == 0)
    return bytesOf(
        "<svg viewBox=\"0 0 64 64\" xmlns=\"http://www.w3.org/2000/svg\">"
        "<defs><linearGradient id=\"g\" x1=\"0\" y1=\"0\" x2=\"1\" y2=\"1\">"
        "<stop offset=\"0\" stop-color=\"#f80\"/><stop offset=\"1\" stop-color=\"#08f\"/>"
        "</linearGradient></defs>"
        "<rect x=\"2\" y=\"2\" width=\"60\" height=\"60\" rx=\"8\" fill=\"url(#g)\"/>"
        "<g transform=\"translate(8 8) scale(0.75) rotate(15)\">"
        "<path d=\"M10 10 L50 10 Q60 30 50 50 C40 60 20 60 10 50 S0 30 10 10 Z\" "
        "fill=\"#fff\" fill-rule=\"evenodd\" stroke=\"#000\" stroke-width=\"2\"/>"
        "<path d=\"m5 30 a12 12 0 0 1 24 0 a12 12 0 1 0 -24 0z h30 v-5 l-3 -2 t4 4\" "
        "fill=\"none\" stroke=\"#333\"/>"
        "</g><circle cx=\"32\" cy=\"32\" r=\"6\" fill=\"#0a0\" opacity=\"0.5\"/>"
        "<ellipse cx=\"48\" cy=\"16\" rx=\"6\" ry=\"3\"/>"
        "<polygon points=\"4,60 12,52 20,60\"/><polyline points=\"40,4 44,8 48,4\"/>"
        "<line x1=\"0\" y1=\"63\" x2=\"63\" y2=\"0\" stroke=\"#f00\"/></svg>");
  if (which == 2)
    // What the tools write: a <style> sheet, <defs> at the end, a clip, a
    // mask, a symbol through <use>, a focal gradient, dashes, currentColor.
    return bytesOf(
        "<svg viewBox=\"0 0 64 64\" xmlns=\"http://www.w3.org/2000/svg\" "
        "xmlns:xlink=\"http://www.w3.org/1999/xlink\"><style>.a{fill:url(#r);stroke:#123;"
        "stroke-width:2;stroke-dasharray:3 1.5;stroke-linejoin:bevel}#k{fill:hsl(200,50%,40%)}"
        "</style><g clip-path=\"url(#c)\" opacity=\".8\"><rect class=\"a\" x=\"4\" y=\"4\" "
        "width=\"40\" height=\"30\" rx=\"6\" ry=\"3\"/><use xlink:href=\"#s\" x=\"30\" "
        "y=\"30\" width=\"30\" height=\"30\" color=\"#0a0\"/></g><circle id=\"k\" cx=\"12\" "
        "cy=\"50\" r=\"8\" mask=\"url(#m)\"/><defs><radialGradient id=\"r\" cx=\".5\" cy=\".5\" "
        "r=\".5\" fx=\".2\" fy=\".3\" spreadMethod=\"reflect\" gradientTransform=\"skewX(10)\">"
        "<stop offset=\"0\" stop-color=\"#fff\"/><stop offset=\"1\" stop-color=\"#06c\" "
        "stop-opacity=\".5\"/></radialGradient><clipPath id=\"c\"><circle cx=\"32\" cy=\"32\" "
        "r=\"30\"/><rect width=\"10\" height=\"10\"/></clipPath><mask id=\"m\"><rect "
        "width=\"64\" height=\"64\" fill=\"#888\"/></mask><symbol id=\"s\" viewBox=\"0 0 10 10\">"
        "<path d=\"M1 1h8v8H1zM3 3v4h4V3z\" fill=\"currentColor\" fill-rule=\"evenodd\"/></symbol>"
        "</defs></svg>");
  return bytesOf(
      "<svg fill=\"none\" viewBox=\"0 0 16 16\" xmlns=\"http://www.w3.org/2000/svg\">"
      "<path d=\"M14.5 13.5V5.41a1 1 0 0 0-.3-.7L9.8.29A1 1 0 0 0 9.08 0H1.5v13.5A2.5 2.5 0 0 0 4 "
      "16h8a2.5 2.5 0 0 0 2.5-2.5m-1.5 0v-7H8v-5H3v12a1 1 0 0 0 1 1h8a1 1 0 0 0 1-1M9.5 5V2.12L12.38 "
      "5zM5.13 5h-.62v1.25h2.12V5zm-.62 3h7.12v1.25H4.5zm.62 3h-.62v1.25h7.12V11z\" "
      "clip-rule=\"evenodd\" fill=\"#666\" fill-rule=\"evenodd\"/></svg>");
}

std::vector<uint8_t> seedFont(std::string* which) {
  const char* candidates[] = {
#if defined(_WIN32)
      "C:/Windows/Fonts/arial.ttf",
      "C:/Windows/Fonts/segoeui.ttf",
      "C:/Windows/Fonts/tahoma.ttf",
      "C:/Windows/Fonts/verdana.ttf",
#endif
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
      "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
      "/System/Library/Fonts/Supplemental/Arial.ttf",
  };
  for (const char* path : candidates) {
    std::vector<uint8_t> bytes;
    if (readAll(path, bytes) && !bytes.empty()) {
      if (which) *which = path;
      return bytes;
    }
  }
  return {};
}

// ── One call per target ──────────────────────────────────────────────────────

volatile long g_sink = 0; // so the optimiser cannot drop a parse

/** A run of text to shape with a fuzzed font. */
struct ShapedRunInput {
  const uint32_t* text;
  size_t n;
  bool rtl;
};

void runOnce(const std::string& target, const std::vector<uint8_t>& b) {
  const uint8_t* d = b.data();
  const size_t n = b.size();
  if (target == "png") {
    const sonore::gfx::PngImage img = sonore::gfx::PngDecoder::decode(d, n);
    g_sink += img.ok() ? img.bitmap.width() : -1;
  } else if (target == "ttf") {
    sonore::gfx::Typeface face;
    if (face.load(b)) {
      sonore::gfx::Path p;
      const int count = face.numGlyphs() < 400 ? face.numGlyphs() : 400;
      for (int g = 0; g < count; ++g) face.appendGlyph((uint16_t) g, p, 0.0f, 0.0f, 0.02f);
      for (uint32_t cp = 0x20; cp < 0x7f; ++cp) g_sink += face.glyphForChar(cp);
      g_sink += face.glyphForChar(0x20AC) + face.glyphForChar(0x1F600);
      g_sink += face.kerning(face.glyphForChar('A'), face.glyphForChar('V'));
      g_sink += face.advanceWidth(face.glyphForChar('W'));
      // Colour glyphs, as Font draws them: layers, and pictures from the
      // strikes for three sizes.
      if (face.hasColourGlyphs()) {
        for (int g = 0; g < count; ++g) {
          for (const sonore::gfx::ColourLayer& layer : face.colourLayers((uint16_t) g, g % 3))
            g_sink += layer.glyph + layer.red + layer.alpha;
          for (const float size : {12.0f, 40.0f, 0.0f}) {
            sonore::gfx::ImageLayer image;
            if (face.colourImage((uint16_t) g, size, &image))
              g_sink += (long) sonore::gfx::PngDecoder::decode(image.png, image.size).bitmap.width() + image.top;
          }
        }
      }
      // Shaped, as Arabic, Hebrew, Latin, Devanagari, Khmer, Myanmar and
      // Javanese text:
      // GSUB and GPOS, GDEF and the kern table are read only then, and they
      // are where a hostile font's lookups -- nested contexts, a sequence of
      // 65535 -- would do harm; the syllabic shapers ask lookups whether they
      // would apply, and reorder by what the font made.
      {
        static const uint32_t arabic[] = {0x644, 0x627, 0x20, 0x628, 0x650, 0x651, 0x645, 0x628, 0x64E, 0x627};
        static const uint32_t hebrew[] = {0x5E9, 0x5B8, 0x5C1, 0x5DC, 0x5D5, 0x5B9, 0x5DD, 0x20, 0x5D1, 0x5BC};
        static const uint32_t latin[] = {'o', 'f', 'f', 'i', 'c', 'e', ' ', 'A', 'V', 'e', 0x301, 'i', 'j'};
        static const uint32_t devanagari[] = {0x930, 0x94D, 0x915, 0x94D, 0x937, 0x93F, 0x902, 0x20, 0x915,
                                              0x94D, 0x200D, 0x930, 0x93F, 0x20, 0x93F, 0x94D};
        static const uint32_t khmer[] = {0x1780, 0x17D2, 0x179A, 0x17C1, 0x17BE, 0x20, 0x17D2, 0x17C6};
        static const uint32_t myanmar[] = {0x1004, 0x103A, 0x1039, 0x1000, 0x103C, 0x1031, 0x102F, 0x20, 0x1031};
        static const uint32_t javanese[] = {0xA98F, 0xA9C0, 0xA9A4, 0xA9BA, 0xA9B4, 0xA981, 0x20, 0xA9BA, 0xA9C0};
        for (const ShapedRunInput& run : {ShapedRunInput{arabic, sizeof(arabic) / 4, true},
                                          ShapedRunInput{hebrew, sizeof(hebrew) / 4, true},
                                          ShapedRunInput{latin, sizeof(latin) / 4, false},
                                          ShapedRunInput{devanagari, sizeof(devanagari) / 4, false},
                                          ShapedRunInput{khmer, sizeof(khmer) / 4, false},
                                          ShapedRunInput{myanmar, sizeof(myanmar) / 4, false},
                                          ShapedRunInput{javanese, sizeof(javanese) / 4, false}})
          for (const sonore::gfx::ShapedGlyph& g : sonore::gfx::shapeRun(face, run.text, run.n, run.rtl))
            g_sink += g.glyph + g.xAdvance;
        // And with features a caller chooses: alternates by value, ligatures
        // off, figures and fractions on.
        static const uint32_t figures[] = {'1', 0xFE0E, 0x2044, '2', 0xE0100, ' ', 'f', 'i', ' ', 'a', 0xFE00, '0'};
        const std::vector<std::pair<uint32_t, uint32_t>> features = {
            {sonore::gfx::Typeface::tagOf("tnum"), 1}, {sonore::gfx::Typeface::tagOf("salt"), 3},
            {sonore::gfx::Typeface::tagOf("liga"), 0}, {sonore::gfx::Typeface::tagOf("frac"), 1},
            {sonore::gfx::Typeface::tagOf("kern"), 0}};
        for (const sonore::gfx::ShapedGlyph& g :
             sonore::gfx::shapeRun(face, figures, sizeof(figures) / 4, false, nullptr, &features))
          g_sink += g.glyph + g.xAdvance;
      }
      // A variable font, drawn at the far end of every axis: its gvar, avar
      // and HVAR are read only then.
      if (face.isVariable()) {
        std::vector<std::pair<uint32_t, float>> settings;
        for (const sonore::gfx::FontAxis& a : face.variationAxes()) settings.push_back({a.tag, a.maximum});
        // Not `far`: windows.h defines it to nothing.
        const std::vector<int16_t> corner = face.locationFor(settings);
        for (int g = 0; g < count; ++g) {
          face.appendGlyph((uint16_t) g, p, corner, 0.0f, 0.0f, 0.02f);
          g_sink += (int) face.advanceWidth((uint16_t) g, corner);
        }
      }
    }
  } else if (target == "svg") {
    const sonore::gfx::Drawable dr = sonore::gfx::Drawable::parse(std::string((const char*) d, n));
    if (!dr.isEmpty()) {
      // Parsed AND drawn: the rasteriser is where hostile geometry lands.
      sonore::gfx::Bitmap bmp(96, 96);
      sonore::gfx::Graphics g(bmp);
      dr.draw(g, sonore::gfx::Rect(0.0f, 0.0f, 96.0f, 96.0f));
      g_sink += dr.numShapes();
    }
  } else if (target == "zip") {
    sonore::ZipFile z;
    if (z.openFromMemory(d, n)) {
      for (int i = 0; i < z.numEntries(); ++i) {
        std::vector<uint8_t> out;
        g_sink += z.extract(i, &out, 64u * 1024u * 1024u) ? (long) out.size() : -1;
      }
    }
  } else if (target == "mid") {
    writeTemp(b);
    sonore::MidiFileData f;
    g_sink += sonore::readMidiFile(g_tempPath.c_str(), &f) ? (long) f.tracks.size() : -1;
  } else if (target == "flac" || target == "wav" || target == "aiff" || target == "mp3" ||
             target == "ogg") {
    writeTemp(b);
    sonore::WavData w;
    bool ok = false;
    if (target == "flac") ok = sonore::readFlac(g_tempPath.c_str(), &w);
    else if (target == "wav") ok = sonore::readWav(g_tempPath.c_str(), &w);
    else if (target == "aiff") ok = sonore::readAiff(g_tempPath.c_str(), &w);
    else if (target == "mp3") ok = sonore::readMp3(g_tempPath.c_str(), &w);
    else ok = sonore::readOgg(g_tempPath.c_str(), &w);
    g_sink += ok ? (long) w.samples.size() : -1;
  } else if (target == "stream") {
    // The other way into the same files: AudioFileReader walks the chunk
    // headers on disk instead of loading the file, and everything built on
    // it trusts the frame count it derives. So reads at the positions that
    // count produces -- its start, middle, last frames, end and past it --
    // then the sample streamer's window primed, slid, pinned over a loop and
    // read, then a waveform built from the file.
    writeTemp(b);
    {
      sonore::AudioFileReader reader;
      if (reader.open(g_tempPath.c_str())) {
        const uint64_t frames = reader.numFrames();
        std::vector<float> buf(600 * (size_t) reader.numChannels());
        for (uint64_t at : {(uint64_t) 0, frames / 2, frames > 3 ? frames - 3 : (uint64_t) 0, frames,
                            frames + 100})
          g_sink += (long) reader.read(at, buf.data(), 600);
      }
    }
    {
      sonore::SampleStreamer streamer;
      if (streamer.open(g_tempPath.c_str())) {
        const uint64_t frames = streamer.numFrames();
        std::vector<float> buf(600 * (size_t) streamer.numChannels());
        streamer.setLoop(frames / 4, frames / 2 + 1);
        for (uint64_t at : {(uint64_t) 0, frames / 4 + 1, frames / 2, frames > 3 ? frames - 3 : (uint64_t) 0}) {
          streamer.service(at);
          g_sink += (long) streamer.read(at, buf.data(), 600);
        }
      }
    }
    sonore::WaveformPeaks peaks;
    g_sink += peaks.buildFromFile(g_tempPath.c_str(), 64) ? (long) peaks.numBuckets() : -1;
  } else if (target == "json") {
    std::string err;
    const sonore::JsonValue v = sonore::JsonValue::parse(std::string((const char*) d, n), &err);
    g_sink += (long) err.size();
  } else if (target == "osc") {
    std::vector<sonore::osc::Message> msgs;
    g_sink += sonore::osc::decodePacket(d, n, &msgs) ? (long) msgs.size() : -1;
  } else if (target == "bag") {
    sonore::StateBag bag;
    g_sink += bag.deserialise(d, n) ? (long) bag.size() : -1;
  } else if (target == "b64") {
    std::vector<uint8_t> out;
    g_sink += sonore::Base64::decode(std::string((const char*) d, n), out) ? (long) out.size() : -1;
  } else if (target == "ttl") {
    sonore::turtle::Document doc;
    g_sink += sonore::turtle::parse(std::string((const char*) d, n), "file:///x.lv2/", &doc)
                  ? (long) doc.triples.size()
                  : -1;
    // And what the LV2 host asks of a document once it has one: its plugins,
    // each one's name, maintainer and ports, and each port's index. A blank
    // node that names itself is no reason for any of it to loop.
    const std::string lv2 = "http://lv2plug.in/ns/lv2core#";
    const std::string doap = "http://usefulinc.com/ns/doap#";
    for (const std::string& plugin : doc.subjectsOfType(lv2 + "Plugin")) {
      g_sink += (long) doc.object(plugin, doap + "name").size();
      g_sink += (long) doc.object(doc.object(plugin, lv2 + "project"), doap + "maintainer").size();
      for (const std::string& port : doc.objects(plugin, lv2 + "port"))
        g_sink += (long) doc.object(port, lv2 + "index").size();
    }
  } else if (target == "lv2atom") {
    // The bytes are the buffer, and atom.size is the host's word for how much
    // of it there is -- held to what is actually there, as a host's own
    // buffer would be. Everything past that is the walk's job: every body it
    // hands over must lie inside, and is read to the last byte.
    if (n < sizeof(LV2_Atom_Sequence)) return;
    std::vector<uint64_t> aligned((n + 7) / 8);
    std::memcpy(aligned.data(), d, n);
    auto* seq = (LV2_Atom_Sequence*) aligned.data();
    if (seq->atom.size > n - sizeof(LV2_Atom)) seq->atom.size = (uint32_t) (n - sizeof(LV2_Atom));
    const uint8_t* end = (const uint8_t*) aligned.data() + n;
    sonore::lv2::forEachAtomEvent(seq, [&](const LV2_Atom_Event* ev) {
      const uint8_t* body = (const uint8_t*) ev + sizeof(LV2_Atom_Event);
      if (body > end || ev->body.size > (size_t) (end - body)) {
        std::fprintf(stderr, "  the walk handed over a %u-byte body with %zu bytes left\n",
                     ev->body.size, (size_t) (end - body));
        std::abort();
      }
      for (uint32_t i = 0; i < ev->body.size; ++i) g_sink += body[i];
      // Every body walked as an OBJECT too, as a time:Position is: each value
      // the property walk hands over must also lie inside.
      const uint8_t* objEnd = body + ev->body.size;
      sonore::lv2::forEachAtomProperty(&ev->body, [&](const LV2_Atom_Property_Body* p) {
        const uint8_t* value = (const uint8_t*) p + sizeof(LV2_Atom_Property_Body);
        if (value > objEnd || p->value.size > (size_t) (objEnd - value)) {
          std::fprintf(stderr, "  the property walk handed over a %u-byte value with %zu left\n",
                       p->value.size, (size_t) (objEnd - value));
          std::abort();
        }
        for (uint32_t i = 0; i < p->value.size; ++i) g_sink += value[i];
      });
    });
  } else if (target == "midi") {
    // A MIDI cable's bytes through the one parser every byte-stream backend
    // shares, and each SysEx it completes asked whether it is MIDI-CI and
    // decoded if it says so: a controller's bytes arrive from outside.
    sonore::midiin::StreamParser parser;
    for (size_t i = 0; i < n; ++i)
      parser.feed(
          d[i], [](uint8_t status, uint8_t a, uint8_t b) { g_sink += status + a + b; },
          [](const uint8_t* bytes, size_t length) {
            sonore::midici::Message m;
            if (sonore::midici::isCiMessage(bytes, length) &&
                sonore::midici::decode(bytes, length, &m))
              g_sink += (long) m.payload.size() + m.identity.categories;
          });
  } else if (target == "ump") {
    // The bytes as UMP words: each packet sized by its type nibble, narrowed
    // to MIDI 1.0 where it has a spelling, SysEx7 packets reassembled per
    // group, and a completed message handed to the CI decoder.
    std::vector<uint32_t> words(n / 4);
    if (!words.empty()) std::memcpy(words.data(), d, words.size() * 4);
    sonore::ump::Sysex7Assembler sysex;
    for (size_t at = 0; at < words.size();) {
      const size_t take = (size_t) sonore::ump::packetWords(words[at]);
      if (at + take > words.size()) break;
      sonore::MidiMessage m;
      uint8_t group = 0;
      if (sonore::ump::toMidi1(words[at], &m, &group)) g_sink += m.getRawData1() + group;
      if (sonore::ump::toSystem(words[at], &m, &group)) g_sink += m.getChannel();
      if (sonore::ump::toMidi1Narrowing(&words[at], take, &m, &group)) g_sink += m.getRawData2();
      if (sysex.push(&words[at], take)) {
        sonore::midici::Message ci;
        if (sonore::midici::decode(sysex.data(), sysex.size(), &ci)) g_sink += (long) ci.payload.size();
        g_sink += (long) sysex.size();
      }
      at += take;
    }
  } else if (target == "vstpreset") {
    writeTemp(b);
    sonore::Vst3PresetFile out;
    g_sink += sonore::readVstPreset(g_tempPath.c_str(), &out) ? (long) out.componentState.size()
                                                              : -1;
  } else if (target == "cache") {
    writeTemp(b);
    sonore::host::PluginCache cache;
    g_sink += cache.load(g_tempPath.c_str()) ? 1 : -1;
  }
}

// ── The mutation schedule ────────────────────────────────────────────────────

void guarded(const std::string& target, const std::vector<uint8_t>& b) {
  g_current = b;
  g_deadlineMs.store(nowMs() + 8000);
  runOnce(target, b);
  g_deadlineMs.store(0);
}

size_t fuzzOne(const std::string& target, const std::vector<uint8_t>& seed, size_t iterations,
               uint32_t rngSeed) {
  size_t cases = 0;
  g_target = target;

  // The seed itself first: a parser that fails on its reference input is a
  // different bug, and the mutations would say nothing about it.
  g_phase = "seed";
  g_case = 0;
  guarded(target, seed);
  ++cases;

  // Every truncation length, capped at 500 steps for a big input.
  g_phase = "truncate";
  const size_t step = seed.size() > 500 ? seed.size() / 500 : 1;
  for (size_t len = 0; len < seed.size(); len += step) {
    g_case = len;
    guarded(target, std::vector<uint8_t>(seed.begin(), seed.begin() + (long) len));
    ++cases;
  }

  std::mt19937 rng(rngSeed);
  // Random byte flips, one to eight per case.
  g_phase = "flip";
  for (size_t it = 0; it < iterations && !seed.empty(); ++it) {
    g_case = it;
    std::vector<uint8_t> b = seed;
    const int flips = 1 + (int) (rng() % 8);
    for (int k = 0; k < flips; ++k) b[rng() % b.size()] = (uint8_t) rng();
    guarded(target, b);
    ++cases;
  }
  // Runs of the bytes that break size arithmetic and the characters that
  // break text grammars.
  g_phase = "runs";
  static const uint8_t kPatterns[] = {0x00, 0xFF, 0x7F, 0x80, 0x01, 0xFE, '"', '\\', '<', '\n'};
  for (size_t it = 0; it < iterations && !seed.empty(); ++it) {
    g_case = it;
    std::vector<uint8_t> b = seed;
    const size_t at = rng() % b.size();
    const size_t len = 1 + rng() % 16;
    const uint8_t pat = kPatterns[rng() % (sizeof(kPatterns))];
    for (size_t k = 0; k < len && at + k < b.size(); ++k)
      b[at + k] = (rng() % 3 == 0) ? (uint8_t) rng() : pat;
    guarded(target, b);
    ++cases;
  }
  // Truncation and flips together.
  g_phase = "mixed";
  for (size_t it = 0; it < iterations / 2 && !seed.empty(); ++it) {
    g_case = it;
    std::vector<uint8_t> b(seed.begin(), seed.begin() + (long) (rng() % seed.size()));
    for (int k = 0; k < 3 && !b.empty(); ++k) b[rng() % b.size()] = (uint8_t) rng();
    guarded(target, b);
    ++cases;
  }
  return cases;
}

struct Job {
  std::string target;
  std::string label;
  std::vector<uint8_t> seed;
};

} // namespace

int main(int argc, char** argv) {
  size_t iterations = 120; // the ctest pass: seconds, not minutes
  std::string only;
  std::string dataDir = SONORE_TEST_DATA_DIR;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--iterations" && i + 1 < argc) iterations = (size_t) std::atol(argv[++i]);
    else if (a == "--target" && i + 1 < argc) only = argv[++i];
    else if (a == "--data-dir" && i + 1 < argc) dataDir = argv[++i];
    else {
      std::fprintf(stderr, "usage: fuzz_parsers [--iterations N] [--target name] [--data-dir dir]\n");
      return 2;
    }
  }
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  // One file per run: two fuzzers at once -- the ctest pass beside a deep
  // pass, or two sanitizer builds -- wrote one fixed name, and each parsed
  // the other's bytes.
  g_tempPath = (std::filesystem::temp_directory_path() /
                ("sonore-fuzz-parsers-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                 ".bin"))
                   .string();

  std::thread dog(watchdog);
  dog.detach();
  watchCrashes();

  std::vector<Job> jobs;
  auto fromFile = [&](const char* target, const char* rel) {
    Job j;
    j.target = target;
    j.label = rel;
    const std::string path = dataDir + "/" + rel;
    if (!readAll(path, j.seed) || j.seed.empty()) {
      std::printf("  SKIPPED %-9s %s is not there\n", target, path.c_str());
      return;
    }
    jobs.push_back(j);
  };
  fromFile("png", "png/filters5.png");
  fromFile("png", "png/grey16.png");
  fromFile("png", "png/interlaced.png");
  fromFile("zip", "pack.zip");
  fromFile("mid", "roundtrip.mid");
  fromFile("flac", "ref16_c8.flac");
  fromFile("flac", "encoded24.flac");
  fromFile("wav", "ref24.wav");
  fromFile("aiff", "ref16be.aiff");
  fromFile("mp3", "tonal.mp3");
  fromFile("ogg", "ref.ogg");
  // The same file with its comment header claiming 0x20000001 comments.
  // stb_vorbis multiplied that by sizeof(char*) in size_t and handed it to an
  // int: an 8-byte allocation, then a memset and a fill loop across eight
  // gigabytes of heap. A count, unlike a corrupted byte, is a field the
  // mutations are unlikely to land on exactly, so it is planted here.
  {
    Job j;
    j.target = "ogg";
    j.label = "ref.ogg, 0x20000001 comments";
    if (readAll(dataDir + "/ref.ogg", j.seed) && j.seed.size() > 140) {
      const char marker[] = "vorbis";
      auto at = std::search(j.seed.begin(), j.seed.end(), marker, marker + 7);
      if (at != j.seed.end()) {
        const size_t header = (size_t) (at - j.seed.begin());
        const size_t vendorLen = (size_t) j.seed[header + 7] | ((size_t) j.seed[header + 8] << 8) |
                                 ((size_t) j.seed[header + 9] << 16) |
                                 ((size_t) j.seed[header + 10] << 24);
        const size_t count = header + 11 + vendorLen;
        if (count + 4 <= j.seed.size()) {
          j.seed[count] = 0x01;
          j.seed[count + 1] = 0x00;
          j.seed[count + 2] = 0x00;
          j.seed[count + 3] = 0x20;
          jobs.push_back(j);
        }
      }
    }
  }
  {
    std::string which;
    Job j;
    j.target = "ttf";
    j.seed = seedFont(&which);
    j.label = which;
    if (j.seed.empty()) std::printf("  SKIPPED ttf       no system TrueType font was found\n");
    else jobs.push_back(j);
  }
  // CFF outlines: a name-keyed font with subroutines, and a CID-keyed one
  // with an FDSelect and a Font DICT per half of its glyphs.
  fromFile("ttf", "cff/tiny.otf");
  fromFile("ttf", "cff/tiny_cid.otf");
  // The old kern table, whose pairs follow a binary-search header; and glyf.
  fromFile("ttf", "cff/tiny_kern.otf");
  fromFile("ttf", "cff/tiny.ttf");
  // A variable font: fvar, avar, gvar with deltas listed and inferred, HVAR.
  fromFile("ttf", "variable/tiny_var.ttf");
  // Every kind of GSUB and GPOS lookup an Arabic, Hebrew or Latin font uses.
  fromFile("ttf", "shaping/tiny_shaping.ttf");
  // No layout tables, and GPOS for Latin alone: marks placed by class.
  fromFile("ttf", "shaping/tiny_shaping_plain.ttf");
  fromFile("ttf", "shaping/tiny_shaping_latn.ttf");
  // Every kind of Indic, Khmer and Myanmar reordering a font asks for.
  fromFile("ttf", "shaping/tiny_indic2.ttf");
  fromFile("ttf", "shaping/tiny_indic.ttf");
  fromFile("ttf", "shaping/tiny_khmer.ttf");
  fromFile("ttf", "shaping/tiny_myanmar.ttf");
  fromFile("ttf", "shaping/tiny_use.ttf");
  fromFile("ttf", "shaping/tiny_features.ttf");
  // A feature listed 4,000 times over, of 4,000 lookups.
  fromFile("ttf", "shaping/tiny_repeats.ttf");
  // Variation sequences: a cmap with a format 14 subtable.
  fromFile("ttf", "shaping/tiny_variants.ttf");
  // Colour glyphs: COLR and CPAL; sbix; CBDT and CBLC, with no outlines.
  fromFile("ttf", "colour/tiny_colr.ttf");
  fromFile("ttf", "colour/tiny_sbix.ttf");
  fromFile("ttf", "colour/tiny_cbdt.ttf");
  jobs.push_back({"svg", "seed 0", seedSvg(0)});
  jobs.push_back({"svg", "seed 1", seedSvg(1)});
  jobs.push_back({"svg", "seed 2", seedSvg(2)});
  jobs.push_back({"json", "seed", seedJson()});
  jobs.push_back({"osc", "seed", seedOsc()});
  jobs.push_back({"bag", "seed", seedBag()});
  jobs.push_back({"b64", "seed", seedBase64()});
  jobs.push_back({"ttl", "seed", seedTurtle()});
  jobs.push_back({"lv2atom", "seed", seedAtomSequence(false)});
  jobs.push_back({"lv2atom", "seed with a wrapping size", seedAtomSequence(true)});
  jobs.push_back({"vstpreset", "seed", seedVstPreset()});
  jobs.push_back({"midi", "seed", seedMidiStream()});
  jobs.push_back({"ump", "seed", seedUmp()});
  jobs.push_back({"cache", "seed", seedCache()});
  // Last, so every job above keeps the position its random seed comes from.
  jobs.push_back({"ttl", "hostile nesting", seedTurtleHostile(0)});
  jobs.push_back({"ttl", "hostile sizes", seedTurtleHostile(1)});
  jobs.push_back({"ttl", "hostile escapes", seedTurtleHostile(2)});
  jobs.push_back({"ttl", "hostile blank cycles", seedTurtleHostile(3)});
  // The chunks real WAVs carry, through the whole-file reader; then the
  // streaming reader, the streamer and the waveform over WAV and AIFF.
  jobs.push_back({"wav", "extensible, metadata chunks", seedWavChunks()});
  jobs.push_back({"stream", "extensible, metadata chunks", seedWavChunks()});
  fromFile("stream", "ref16.wav");
  fromFile("stream", "written.aiff");
  // A FLAC behind an ID3v2 tag, whose syncsafe size the reader now trusts to
  // find the stream.
  {
    Job j;
    j.target = "flac";
    j.label = "ref16_c0.flac behind ID3v2";
    std::vector<uint8_t> flac;
    if (readAll(dataDir + "/ref16_c0.flac", flac) && !flac.empty()) {
      j.seed = {'I', 'D', '3', 4, 0, 0, 0, 0, 1, 0}; // a 128-byte tag
      j.seed.resize(10 + 128, 0);
      j.seed.insert(j.seed.end(), flac.begin(), flac.end());
      jobs.push_back(j);
    }
  }

  std::printf("Sonore SDK parser fuzz: %zu iterations per phase\n", iterations);
  size_t total = 0;
  int ran = 0;
  for (size_t index = 0; index < jobs.size(); ++index) {
    const Job& j = jobs[index];
    // The seed is the job's POSITION, so `--target x` replays exactly the
    // cases the full run gave x -- a crash seen in the full run has to be
    // reproducible by name.
    const uint32_t rngSeed = 0x5EED0001u + (uint32_t) index;
    if (!only.empty() && j.target != only) continue;
    // A slow parser gets fewer iterations, so the quick pass stays quick: the
    // font walks four hundred glyphs per case, the lossy decoders a whole
    // file, and the streaming target opens each file three times and fills a
    // sample streamer's window.
    size_t n = iterations;
    if (j.target == "ttf") n = iterations / 5 + 1;
    if (j.target == "mp3" || j.target == "ogg" || j.target == "stream") n = iterations / 3 + 1;
    const auto t0 = std::chrono::steady_clock::now();
    const size_t cases = fuzzOne(j.target, j.seed, n, rngSeed);
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  ok   %-9s %-26s %6zu cases in %.1fs\n", j.target.c_str(),
                (j.label.size() > 26 ? j.label.substr(j.label.size() - 26) : j.label).c_str(),
                cases, secs);
    total += cases;
    ++ran;
  }
  g_done.store(true);
  std::remove(g_tempPath.c_str()); // named per run, so removed by it
  if (ran == 0) {
    std::printf("no target ran\n");
    return 1;
  }
  std::printf("\n%zu cases across %d targets, none crashed or hung\n", total, ran);
  std::printf("SONORE PARSER FUZZ PASSED\n");
  (void) g_sink;
  return 0;
}
