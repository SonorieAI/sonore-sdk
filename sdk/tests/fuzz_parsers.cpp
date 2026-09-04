// SPDX-License-Identifier: Apache-2.0
// Mutation fuzzing for every parser in the SDK that reads bytes it did not
// write: PNG, TrueType, SVG, zip, Standard MIDI files, FLAC, WAV, AIFF, MP3,
// Ogg, JSON, OSC, the state bag, base64, the Turtle a host reads from every
// LV2 bundle on the machine, the VST3 preset file and the plugin cache.
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
#include <sonore/gfx/bitmap.h>
#include <sonore/gfx/graphics.h>
#include <sonore/gfx/png.h>
#include <sonore/gfx/svg.h>
#include <sonore/gfx/truetype.h>
#include <sonore/json.h>
#include <sonore/midi_file.h>
#include <sonore/osc.h>
#include <sonore/plugin_cache.h>
#include <sonore/preset_file.h>
#include <sonore/state_bag.h>
#include <sonore/turtle.h>
#include <sonore/zip.h>

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

std::vector<uint8_t> seedJson() {
  return bytesOf(
      "{\"a\":[1,2.5,-3e10,true,null,\"x\\u00e9\\ud83d\\ude00\\\"\\\\/\\b\\f\\n\\r\\t\"],"
      "\"b\":{\"c\":{\"d\":[[[]]]},\"e\":\"\\u0041\"},\"n\":1e308,\"m\":-0.0,\"z\":\"\"}");
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
  g_tempPath = (std::filesystem::temp_directory_path() / "sonore-fuzz-parsers.bin").string();

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
  {
    std::string which;
    Job j;
    j.target = "ttf";
    j.seed = seedFont(&which);
    j.label = which;
    if (j.seed.empty()) std::printf("  SKIPPED ttf       no system TrueType font was found\n");
    else jobs.push_back(j);
  }
  jobs.push_back({"svg", "seed 0", seedSvg(0)});
  jobs.push_back({"svg", "seed 1", seedSvg(1)});
  jobs.push_back({"json", "seed", seedJson()});
  jobs.push_back({"osc", "seed", seedOsc()});
  jobs.push_back({"bag", "seed", seedBag()});
  jobs.push_back({"b64", "seed", seedBase64()});
  jobs.push_back({"ttl", "seed", seedTurtle()});
  jobs.push_back({"vstpreset", "seed", seedVstPreset()});
  jobs.push_back({"cache", "seed", seedCache()});

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
    // font walks four hundred glyphs per case and the lossy decoders a whole
    // file.
    size_t n = iterations;
    if (j.target == "ttf") n = iterations / 5 + 1;
    if (j.target == "mp3" || j.target == "ogg") n = iterations / 3 + 1;
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
  if (ran == 0) {
    std::printf("no target ran\n");
    return 1;
  }
  std::printf("\n%zu cases across %d targets, none crashed or hung\n", total, ran);
  std::printf("SONORE PARSER FUZZ PASSED\n");
  (void) g_sink;
  return 0;
}
