// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: sonore/host.h gives an LV2 plugin what the core spec requires.
//
// A bundle path ending in a directory separator, every port connected before
// run(), CV ports included, and a urid:map that holds up when several threads
// map at once. The fixture records all three in the process environment.
//
// And it survives a plugin that gets its atom output wrong: the same binary's
// second plugin, described by a bundle this test writes beside a copy of it,
// leaves its output unwritten or claims a sequence longer than its buffer.
//
// And its third, with an interface, talks to that interface in atoms both
// ways, through the host.

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include <sonore/host.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

static int g_checks = 0;
static int g_failures = 0;

static void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

static std::string var(const char* name) {
  char text[1024] = {0};
#if defined(_WIN32)
  GetEnvironmentVariableA(name, text, (DWORD) sizeof(text));
#else
  if (const char* v = std::getenv(name)) std::snprintf(text, sizeof(text), "%s", v);
#endif
  return text;
}

static void setVar(const char* name, const char* value) {
#if defined(_WIN32)
  SetEnvironmentVariableA(name, value);
#else
  setenv(name, value, 1);
#endif
}

static void clearVar(const char* name) { setVar(name, "0"); }

namespace fs = std::filesystem;

/** A bundle of the test's own: the fixture's binary copied from the build's
 *  bundle, and `ttl` as its manifest, with BINARY replaced by the binary's
 *  file name. Empty if it could not be made. */
static std::string writeBundle(const std::string& from, const char* leaf, std::string ttl) {
  std::error_code error;
  const fs::path dir = fs::temp_directory_path(error) /
                       (std::string(leaf) + "-" +
                        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                        ".lv2");
  fs::remove_all(dir, error);
  if (!fs::create_directories(dir, error)) return std::string();
  std::string binary;
  for (const auto& entry : fs::directory_iterator(fs::u8path(from), error)) {
    const std::string ext = entry.path().extension().u8string();
    if (ext != ".dll" && ext != ".so" && ext != ".dylib") continue;
    binary = entry.path().filename().u8string();
    fs::copy_file(entry.path(), dir / entry.path().filename(), error);
    if (error) return std::string();
  }
  if (binary.empty()) return std::string();
  for (size_t at = ttl.find("BINARY"); at != std::string::npos; at = ttl.find("BINARY", at))
    ttl.replace(at, 6, binary);
  std::ofstream(dir / "manifest.ttl", std::ios::binary) << ttl;
  return dir.u8string();
}

static void removeBundle(const std::string& dir) {
  std::error_code error;
  if (!dir.empty()) fs::remove_all(fs::u8path(dir), error);
}

/** The atom-port misbehaviours. A sequence is what the plugin WRITES into
 *  the buffer the host gave it, and the host trusted it: an output the
 *  plugin left alone still held the last block's events, read again as new,
 *  and a size past the buffer was walked, past the end of the host's heap
 *  block. */
static void checkAtomOutput(const std::string& fixtureBundle) {
  std::printf("── LV2 atom output a plugin got wrong ──\n");
  const std::string bundle = writeBundle(fixtureBundle, "sonore-lv2-atoms", R"TTL(
@prefix lv2: <http://lv2plug.in/ns/lv2core#> .
@prefix atom: <http://lv2plug.in/ns/ext/atom#> .
@prefix midi: <http://lv2plug.in/ns/ext/midi#> .
@prefix doap: <http://usefulinc.com/ns/doap#> .
<urn:sonore:test:lv2-atoms> a lv2:Plugin ;
  doap:name "Sonore LV2 Atoms Fixture" ;
  lv2:binary <BINARY> ;
  lv2:port [ a lv2:InputPort , atom:AtomPort ; atom:bufferType atom:Sequence ;
             atom:supports midi:MidiEvent ; lv2:index 0 ; lv2:symbol "midi_in" ; lv2:name "MIDI In" ] ,
    [ a lv2:OutputPort , atom:AtomPort ; atom:bufferType atom:Sequence ;
      atom:supports midi:MidiEvent ; lv2:index 1 ; lv2:symbol "midi_out" ; lv2:name "MIDI Out" ] ,
    [ a lv2:InputPort , lv2:ControlPort ; lv2:index 2 ; lv2:symbol "mode" ; lv2:name "Mode" ;
      lv2:default 0 ; lv2:minimum 0 ; lv2:maximum 2 ] .
)TTL");
  check(!bundle.empty(), "a bundle for the atom fixture is written");
  if (bundle.empty()) return;
  {
    const auto found = sonore::host::describeFile(bundle);
    std::unique_ptr<sonore::host::HostedPlugin> plugin =
        found.size() == 1 ? sonore::host::loadPlugin(found.front()) : nullptr;
    int mode = -1;
    for (int i = 0; plugin && i < plugin->numParameters(); ++i)
      if (plugin->parameter(i).name == "Mode") mode = i;
    const bool ready = plugin && mode >= 0 && plugin->acceptsMidi() && plugin->prepare(48000.0, 64, 2);
    check(ready, "...which loads and prepares, MIDI in and out and a mode control");
    if (ready) {
      std::vector<float> l(64, 0.0f), r(64, 0.0f);
      float* ch[2] = {l.data(), r.data()};
      sonore::MidiBuffer note;
      note.addEvent(sonore::MidiMessage::noteOn(0, 60, 100), 0);
      sonore::AudioBlock<float> io(ch, 2, 64);

      plugin->setParameterValue(mode, 0.0);
      plugin->process(io, &note);
      const int echoed = plugin->producedMidi().getNumEvents();
      check(echoed == 1, "a plugin echoing a note hands back one note");

      // A caller's MidiBuffer does not know how long the block is, and may
      // hold positions at or past its end. CLAP moved such an event inside;
      // LV2 handed the plugin a time past the frames run() was given.
      clearVar("SONORE_LV2_FIXTURE_LATE_EVENTS");
      sonore::MidiBuffer stray;
      stray.addEvent(sonore::MidiMessage::noteOn(0, 60, 100), 63);
      stray.addEvent(sonore::MidiMessage::noteOff(0, 60), 64);
      stray.addEvent(sonore::MidiMessage::noteOn(0, 62, 100), 1000);
      plugin->process(io, &stray);
      char late[160];
      std::snprintf(late, sizeof(late), "every event reaches the plugin inside its 64-frame block "
                    "(%s outside)", var("SONORE_LV2_FIXTURE_LATE_EVENTS").c_str());
      check(var("SONORE_LV2_FIXTURE_LATE_EVENTS") == "0", late);

      plugin->setParameterValue(mode, 1.0);
      plugin->process(io, nullptr);
      const int stale = plugin->producedMidi().getNumEvents();
      char line[200];
      std::snprintf(line, sizeof(line), "an output the plugin did not write this block hands back "
                    "nothing, not the last block's note again (%d)", stale);
      check(stale == 0, line);

      // A size of two gigabytes: walked as given, this reads off the end of
      // the host's buffer, which only a crash or a sanitizer reports.
      plugin->setParameterValue(mode, 2.0);
      plugin->process(io, nullptr);
      const int claimed = plugin->producedMidi().getNumEvents();
      std::snprintf(line, sizeof(line), "a sequence claiming more than its buffer is not walked "
                    "past it (%d events taken)", claimed);
      check(claimed <= 1, line);
      plugin->release();
    }
  }
  removeBundle(bundle);
}

/** A port index the core spec does not allow -- below zero, or past the
 *  ports the plugin declares -- in an otherwise good bundle. The host sized
 *  its control array by the highest index and wrote every control's default
 *  at its index: -1 landed four bytes BEFORE the array, in the heap's own
 *  bookkeeping, and two billion asked for 8 GB. A description like that is
 *  refused, as a missing binary is. */
static void checkPortIndices(const std::string& fixtureBundle) {
  std::printf("── LV2 port indices the spec does not allow ──\n");
  for (const char* index : {"-1", "2000000000", "3", "two"}) {
    std::string ttl = R"TTL(
@prefix lv2: <http://lv2plug.in/ns/lv2core#> .
@prefix atom: <http://lv2plug.in/ns/ext/atom#> .
@prefix midi: <http://lv2plug.in/ns/ext/midi#> .
@prefix doap: <http://usefulinc.com/ns/doap#> .
<urn:sonore:test:lv2-atoms> a lv2:Plugin ;
  doap:name "Sonore LV2 Atoms Fixture" ;
  lv2:binary <BINARY> ;
  lv2:port [ a lv2:InputPort , atom:AtomPort ; atom:bufferType atom:Sequence ;
             atom:supports midi:MidiEvent ; lv2:index 0 ; lv2:symbol "midi_in" ; lv2:name "MIDI In" ] ,
    [ a lv2:OutputPort , atom:AtomPort ; atom:bufferType atom:Sequence ;
      atom:supports midi:MidiEvent ; lv2:index 1 ; lv2:symbol "midi_out" ; lv2:name "MIDI Out" ] ,
    [ a lv2:InputPort , lv2:ControlPort ; lv2:index INDEX ; lv2:symbol "mode" ; lv2:name "Mode" ;
      lv2:default 0 ; lv2:minimum 0 ; lv2:maximum 2 ] .
)TTL";
    const std::string value = std::string(index) == "two" ? "\"two\"" : index;
    ttl.replace(ttl.find("INDEX"), 5, value);
    const std::string bundle = writeBundle(fixtureBundle, "sonore-lv2-index", ttl);
    bool refused = false;
    {
      const auto found = sonore::host::describeFile(bundle);
      refused = found.size() == 1 && !sonore::host::loadPlugin(found.front());
    }
    char line[160];
    std::snprintf(line, sizeof(line), "a control port at lv2:index %s, of three ports, is refused",
                  value.c_str());
    check(refused, line);
    removeBundle(bundle);
  }
}

/** An LV2 interface hears the plugin's output controls -- its meters, its
 *  gain reduction, whatever it reports -- through port_event, from the host,
 *  because it shares no memory with the plugin. This host sent the UI its
 *  INPUT controls only, so every meter in an LV2 editor it opened stood still:
 *  the SDK's own LV2 editors' among them. The fixture's interface records
 *  what it is sent; it draws nothing, so any non-null parent will do. */
static void checkEditorHearsOutputs(const std::string& fixtureBundle) {
  std::printf("── An LV2 editor hears the plugin's output controls ──\n");
#if defined(_WIN32)
  const char* uiType = "ui:WindowsUI";
#elif defined(__APPLE__)
  const char* uiType = "ui:CocoaUI";
#else
  const char* uiType = "ui:X11UI";
#endif
  std::string ttl = R"TTL(
@prefix lv2: <http://lv2plug.in/ns/lv2core#> .
@prefix atom: <http://lv2plug.in/ns/ext/atom#> .
@prefix midi: <http://lv2plug.in/ns/ext/midi#> .
@prefix doap: <http://usefulinc.com/ns/doap#> .
@prefix ui: <http://lv2plug.in/ns/extensions/ui#> .
<urn:sonore:test:lv2-atoms> a lv2:Plugin ;
  doap:name "Sonore LV2 Atoms Fixture" ;
  lv2:binary <BINARY> ;
  ui:ui <urn:sonore:test:lv2-atoms#ui> ;
  lv2:port [ a lv2:InputPort , atom:AtomPort ; atom:bufferType atom:Sequence ;
             atom:supports midi:MidiEvent ; lv2:index 0 ; lv2:symbol "midi_in" ; lv2:name "MIDI In" ] ,
    [ a lv2:OutputPort , atom:AtomPort ; atom:bufferType atom:Sequence ;
      atom:supports midi:MidiEvent ; lv2:index 1 ; lv2:symbol "midi_out" ; lv2:name "MIDI Out" ] ,
    [ a lv2:InputPort , lv2:ControlPort ; lv2:index 2 ; lv2:symbol "mode" ; lv2:name "Mode" ;
      lv2:default 0 ; lv2:minimum 0 ; lv2:maximum 2 ] ,
    [ a lv2:OutputPort , lv2:ControlPort ; lv2:index 3 ; lv2:symbol "level" ; lv2:name "Level" ] .
<urn:sonore:test:lv2-atoms#ui> a UITYPE ; ui:binary <BINARY> .
)TTL";
  ttl.replace(ttl.find("UITYPE"), 6, uiType);
  const std::string bundle = writeBundle(fixtureBundle, "sonore-lv2-ui", ttl);
  {
    const auto found = sonore::host::describeFile(bundle);
    std::unique_ptr<sonore::host::HostedPlugin> plugin =
        found.size() == 1 ? sonore::host::loadPlugin(found.front()) : nullptr;
    int mode = -1;
    for (int i = 0; plugin && i < plugin->numParameters(); ++i)
      if (plugin->parameter(i).name == "Mode") mode = i;
    int parent = 0;
    const bool open = plugin && mode >= 0 && plugin->hasEditor() &&
                      plugin->prepare(48000.0, 64, 2) && plugin->openEditor(&parent);
    check(open, "the fixture's interface opens, in any parent, as it draws nothing");
    if (open) {
      clearVar("SONORE_LV2_UI_PORT3");
      plugin->setParameterValue(mode, 1.0); // the plugin reports 0.25 + 1/8
      std::vector<float> l(64, 0.0f), r(64, 0.0f);
      float* ch[2] = {l.data(), r.data()};
      sonore::AudioBlock<float> io(ch, 2, 64);
      plugin->process(io, nullptr);
      plugin->idleEditor();
      char line[160];
      std::snprintf(line, sizeof(line), "the plugin's output control reaches its interface after "
                    "a block and an idle (0.%s, expected 0.375)",
                    var("SONORE_LV2_UI_PORT3").c_str());
      check(var("SONORE_LV2_UI_PORT3") == "375", line);
      std::snprintf(line, sizeof(line), "...and the mode it was set to, an input, as before (%s)",
                    var("SONORE_LV2_UI_PORT2").c_str());
      check(var("SONORE_LV2_UI_PORT2") == "1000", line);
      plugin->closeEditor();
      plugin->release();
    }
  }
  removeBundle(bundle);
}

/** An interface that talks to its plugin in atoms -- a file browser, a
 *  state view, the SDK's own web interface -- writes them through
 *  write_function as atom:eventTransfer, and hears the plugin's atom
 *  outputs through port_event the same way. This host gave such an
 *  interface no urid:map to name either with, dropped every atom it wrote,
 *  and forwarded nothing the plugin sent: an LV2 editor never had its
 *  plugin's state, its held notes, or the answer to a file it loaded. */
static std::string writeEchoBundle(const std::string& fixtureBundle, const char* leaf) {
#if defined(_WIN32)
  const char* uiType = "ui:WindowsUI";
#elif defined(__APPLE__)
  const char* uiType = "ui:CocoaUI";
#else
  const char* uiType = "ui:X11UI";
#endif
  std::string ttl = R"TTL(
@prefix lv2: <http://lv2plug.in/ns/lv2core#> .
@prefix atom: <http://lv2plug.in/ns/ext/atom#> .
@prefix midi: <http://lv2plug.in/ns/ext/midi#> .
@prefix doap: <http://usefulinc.com/ns/doap#> .
@prefix ui: <http://lv2plug.in/ns/extensions/ui#> .
<urn:sonore:test:lv2-echo> a lv2:Plugin ;
  doap:name "Sonore LV2 Echo Fixture" ;
  lv2:binary <BINARY> ;
  ui:ui <urn:sonore:test:lv2-echo#ui> ;
  lv2:port [ a lv2:InputPort , atom:AtomPort ; atom:bufferType atom:Sequence ;
             atom:supports <urn:sonore:test:echo#message> ;
             lv2:index 0 ; lv2:symbol "control" ; lv2:name "Control" ] ,
    [ a lv2:OutputPort , atom:AtomPort ; atom:bufferType atom:Sequence ;
      atom:supports <urn:sonore:test:echo#message> ;
      lv2:index 1 ; lv2:symbol "notify" ; lv2:name "Notify" ] ,
    [ a lv2:InputPort , atom:AtomPort ; atom:bufferType atom:Sequence ;
      atom:supports midi:MidiEvent ; lv2:index 2 ; lv2:symbol "midi_in" ; lv2:name "MIDI In" ] ,
    [ a lv2:OutputPort , atom:AtomPort ; atom:bufferType atom:Sequence ;
      atom:supports midi:MidiEvent ; lv2:index 3 ; lv2:symbol "midi_out" ; lv2:name "MIDI Out" ] .
<urn:sonore:test:lv2-echo#ui> a UITYPE ; ui:binary <BINARY> ;
  ui:portNotification [ ui:plugin <urn:sonore:test:lv2-echo> ; lv2:symbol "notify" ;
                        ui:protocol atom:eventTransfer ] .
)TTL";
  ttl.replace(ttl.find("UITYPE"), 6, uiType);
  return writeBundle(fixtureBundle, leaf, ttl);
}

static void checkUiAtoms(const std::string& fixtureBundle) {
  std::printf("── An LV2 interface and its plugin talk in atoms ──\n");
  const std::string bundle = writeEchoBundle(fixtureBundle, "sonore-lv2-echo");
  for (const char* name :
       {"SONORE_LV2_ECHO_SEND", "SONORE_LV2_ECHO_BYTES", "SONORE_LV2_ECHO_HEARD",
        "SONORE_LV2_ECHO_INTACT", "SONORE_LV2_ECHO_LATE", "SONORE_LV2_ECHO_NO_ROOM",
        "SONORE_LV2_ECHO_UNCONNECTED", "SONORE_LV2_ECHO_UI_MAP", "SONORE_LV2_ECHO_UI_GOT",
        "SONORE_LV2_ECHO_UI_INTACT", "SONORE_LV2_ECHO_UI_LAST", "SONORE_LV2_ECHO_UI_MIDI",
        "SONORE_LV2_ECHO_UI_ODD", "SONORE_LV2_ECHO_UI_DISORDER"})
    clearVar(name);
  {
    const auto found = sonore::host::describeFile(bundle);
    std::unique_ptr<sonore::host::HostedPlugin> plugin =
        found.size() == 1 ? sonore::host::loadPlugin(found.front()) : nullptr;
    int parent = 0;
    const bool open = plugin && plugin->hasEditor() && plugin->prepare(48000.0, 64, 2) &&
                      plugin->openEditor(&parent);
    check(open, "the echo fixture loads, prepares, and its interface opens");
    if (open) {
      auto n = [](const char* name) { return std::atoi(var(name).c_str()); };
      check(var("SONORE_LV2_ECHO_UI_MAP") == "1",
            "the interface is given urid:map, to name what it sends with");
      std::vector<float> l(64, 0.0f), r(64, 0.0f);
      float* ch[2] = {l.data(), r.data()};
      sonore::AudioBlock<float> io(ch, 2, 64);

      // A short message and a long one, written on two ticks of the
      // interface, then a block.
      setVar("SONORE_LV2_ECHO_BYTES", "40");
      setVar("SONORE_LV2_ECHO_SEND", "1");
      plugin->idleEditor();
      setVar("SONORE_LV2_ECHO_BYTES", "3000");
      setVar("SONORE_LV2_ECHO_SEND", "1");
      plugin->idleEditor();
      plugin->process(io, nullptr);
      char line[240];
      std::snprintf(line, sizeof(line), "both atoms the interface wrote reach the plugin's atom "
                    "input at the next block, whole (%d heard, %d whole, %d outside the block)",
                    n("SONORE_LV2_ECHO_HEARD"), n("SONORE_LV2_ECHO_INTACT"),
                    n("SONORE_LV2_ECHO_LATE"));
      check(n("SONORE_LV2_ECHO_HEARD") == 2 && n("SONORE_LV2_ECHO_INTACT") == 2 &&
                n("SONORE_LV2_ECHO_LATE") == 0,
            line);
      plugin->idleEditor();
      std::snprintf(line, sizeof(line), "the plugin's echo on its notify port reaches the "
                    "interface at the next idle, an atom per event, whole and in order (%d got, "
                    "%d whole, last %d, %d malformed)", n("SONORE_LV2_ECHO_UI_GOT"),
                    n("SONORE_LV2_ECHO_UI_INTACT"), n("SONORE_LV2_ECHO_UI_LAST"),
                    n("SONORE_LV2_ECHO_UI_ODD"));
      check(n("SONORE_LV2_ECHO_UI_GOT") == 2 && n("SONORE_LV2_ECHO_UI_INTACT") == 2 &&
                n("SONORE_LV2_ECHO_UI_LAST") == 2 && n("SONORE_LV2_ECHO_UI_ODD") == 0,
            line);

      // MIDI the plugin plays is the caller's, through producedMidi(), and
      // nothing is delivered twice.
      sonore::MidiBuffer note;
      note.addEvent(sonore::MidiMessage::noteOn(0, 60, 100), 0);
      plugin->process(io, &note);
      const int midi = plugin->producedMidi().getNumEvents();
      plugin->idleEditor();
      plugin->process(io, nullptr);
      plugin->idleEditor();
      std::snprintf(line, sizeof(line), "the plugin's MIDI goes to producedMidi() (%d) and not to "
                    "the interface (%d), and no atom arrives twice (%d heard, %d got)", midi,
                    n("SONORE_LV2_ECHO_UI_MIDI"), n("SONORE_LV2_ECHO_HEARD"),
                    n("SONORE_LV2_ECHO_UI_GOT"));
      check(midi == 1 && n("SONORE_LV2_ECHO_UI_MIDI") == 0 && n("SONORE_LV2_ECHO_HEARD") == 2 &&
                n("SONORE_LV2_ECHO_UI_GOT") == 2,
            line);

      // Three hundred kilobyte messages in one tick, with no block to take
      // them: the queue is bounded, so the ones that do not fit are dropped
      // and counted -- never waited for, never torn -- and the rest arrive,
      // in order, over the blocks that follow. Twice, so each queue has
      // wrapped round its end by the second.
      for (int burst = 0; burst < 2; ++burst) {
        setVar("SONORE_LV2_ECHO_BYTES", "1000");
        setVar("SONORE_LV2_ECHO_SEND", "300");
        plugin->idleEditor();
        for (int i = 0; i < 40; ++i) {
          plugin->process(io, nullptr);
          plugin->idleEditor();
        }
      }
      const int heard = n("SONORE_LV2_ECHO_HEARD") - 2, got = n("SONORE_LV2_ECHO_UI_GOT") - 2;
      const auto* lv2 = dynamic_cast<const sonore::host::Lv2Plugin*>(plugin.get());
      const long long dropped = lv2 ? (long long) lv2->droppedUiEvents() : -1;
      std::snprintf(line, sizeof(line), "two bursts of 300 are bounded: %d reach the plugin and %d "
                    "come back, whole and in order (%d torn, %d out of order), %lld counted as "
                    "dropped", heard, got, n("SONORE_LV2_ECHO_UI_GOT") - n("SONORE_LV2_ECHO_UI_INTACT"),
                    n("SONORE_LV2_ECHO_UI_DISORDER"), dropped);
      check(heard > 0 && heard < 600 && got == heard && heard + dropped == 600 &&
                n("SONORE_LV2_ECHO_INTACT") == n("SONORE_LV2_ECHO_HEARD") &&
                n("SONORE_LV2_ECHO_UI_INTACT") == n("SONORE_LV2_ECHO_UI_GOT") &&
                n("SONORE_LV2_ECHO_UI_DISORDER") == 0 && n("SONORE_LV2_ECHO_LATE") == 0,
            line);
      plugin->closeEditor();
      plugin->release();
    }
  }
  removeBundle(bundle);
}

/** An LV2 interface's idle() answers non-zero when it has closed itself --
 *  a close box of its own, a renderer that died -- and the host is to stop
 *  calling it. This host went on calling idle() and port_event() on it at
 *  every tick, and since the interface was still "open" the caller could not
 *  open it again: an editor gone from the screen and impossible to bring
 *  back. Now it is cleaned up like one the caller closed, and opens again. */
static void checkUiClosesItself(const std::string& fixtureBundle) {
  std::printf("── An LV2 interface that closes itself ──\n");
  const std::string bundle = writeEchoBundle(fixtureBundle, "sonore-lv2-echo-close");
  for (const char* name : {"SONORE_LV2_ECHO_CLOSE", "SONORE_LV2_ECHO_SEND",
                           "SONORE_LV2_ECHO_IDLE_AFTER_CLOSE", "SONORE_LV2_ECHO_EVENTS_AFTER_CLOSE",
                           "SONORE_LV2_ECHO_UI_CLEANUPS", "SONORE_LV2_ECHO_UI_GOT"})
    clearVar(name);
  {
    const auto found = sonore::host::describeFile(bundle);
    std::unique_ptr<sonore::host::HostedPlugin> plugin =
        found.size() == 1 ? sonore::host::loadPlugin(found.front()) : nullptr;
    int parent = 0;
    const bool open = plugin && plugin->prepare(48000.0, 64, 2) && plugin->openEditor(&parent);
    check(open, "the echo fixture's interface opens");
    if (open) {
      auto n = [](const char* name) { return std::atoi(var(name).c_str()); };
      std::vector<float> l(64, 0.0f), r(64, 0.0f);
      float* ch[2] = {l.data(), r.data()};
      sonore::AudioBlock<float> io(ch, 2, 64);
      // A message on its way back to the interface when it closes, and the
      // ticks and blocks a host goes on running.
      setVar("SONORE_LV2_ECHO_SEND", "1");
      plugin->idleEditor();
      plugin->process(io, nullptr);
      setVar("SONORE_LV2_ECHO_CLOSE", "1");
      plugin->idleEditor();
      for (int i = 0; i < 5; ++i) {
        plugin->process(io, nullptr);
        plugin->idleEditor();
      }
      char line[240];
      std::snprintf(line, sizeof(line), "after its idle() says it has closed, the interface is "
                    "called no more (%d idle, %d port_event) and is cleaned up once (%d)",
                    n("SONORE_LV2_ECHO_IDLE_AFTER_CLOSE"), n("SONORE_LV2_ECHO_EVENTS_AFTER_CLOSE"),
                    n("SONORE_LV2_ECHO_UI_CLEANUPS"));
      check(n("SONORE_LV2_ECHO_IDLE_AFTER_CLOSE") == 0 &&
                n("SONORE_LV2_ECHO_EVENTS_AFTER_CLOSE") == 0 && n("SONORE_LV2_ECHO_UI_CLEANUPS") == 1,
            line);
      // Closed is closed: the caller can bring it back as it would one it
      // had closed itself, and it works.
      const bool reopened = plugin->openEditor(&parent);
      clearVar("SONORE_LV2_ECHO_UI_GOT");
      setVar("SONORE_LV2_ECHO_SEND", "1");
      plugin->idleEditor();
      plugin->process(io, nullptr);
      plugin->idleEditor();
      std::snprintf(line, sizeof(line), "the editor opens again with no closeEditor() between "
                    "(%s), and talks to its plugin (%d back)", reopened ? "yes" : "no",
                    n("SONORE_LV2_ECHO_UI_GOT"));
      check(reopened && n("SONORE_LV2_ECHO_UI_GOT") == 1, line);
      plugin->closeEditor();
      plugin->closeEditor();
      std::snprintf(line, sizeof(line), "...and the caller's close cleans that one up, once "
                    "(%d in all)", n("SONORE_LV2_ECHO_UI_CLEANUPS"));
      check(n("SONORE_LV2_ECHO_UI_CLEANUPS") == 2, line);
      plugin->release();
    }
  }
  removeBundle(bundle);
}

/** A blank node belongs to the file it is written in. A MOD device's
 *  modgui.ttl describes the plugin's ports again, as blank nodes with an
 *  lv2:index and an lv2:symbol each, and a bundle lists it beside the file
 *  with the real ports. The parser names each file's blank nodes _:sonore1,
 *  _:sonore2 and on, and the host merged the files' triples into one
 *  document -- so the GUI's second blank node and the second port were one
 *  node, with two indices, and whichever file came first said which. */
static void checkBlankNodesStayInTheirFile(const std::string& fixtureBundle) {
  std::printf("── Blank nodes in two files of one bundle ──\n");
  const std::string bundle = writeBundle(fixtureBundle, "sonore-lv2-modgui", R"TTL(
@prefix lv2: <http://lv2plug.in/ns/lv2core#> .
@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .
<urn:sonore:test:lv2-ports> a lv2:Plugin ;
  lv2:binary <BINARY> ;
  rdfs:seeAlso <modgui.ttl> , <ports.ttl> .
)TTL");
  std::error_code error;
  fs::copy_file(fs::u8path(fixtureBundle) / "ports.ttl", fs::u8path(bundle) / "ports.ttl", error);
  std::ofstream(fs::u8path(bundle) / "modgui.ttl", std::ios::binary) << R"TTL(
@prefix lv2: <http://lv2plug.in/ns/lv2core#> .
@prefix modgui: <http://moddevices.com/ns/modgui#> .
<urn:sonore:test:lv2-ports> modgui:gui [
    modgui:brand "Sonorie" ;
    modgui:port [ lv2:index 4 ; lv2:symbol "gain" ; lv2:name "Gain" ] ,
                [ lv2:index 2 ; lv2:symbol "mod" ; lv2:name "Mod" ]
  ] .
)TTL";
  {
    clearVar("SONORE_LV2_FIXTURE_UNCONNECTED");
    const auto found = sonore::host::describeFile(bundle);
    auto plugin = found.size() == 1 ? sonore::host::loadPlugin(found.front()) : nullptr;
    const bool ready = plugin && plugin->prepare(48000.0, 64, 2);
    if (ready) {
      std::vector<float> l(64, 0.25f), r(64, 0.25f);
      float* ch[2] = {l.data(), r.data()};
      sonore::AudioBlock<float> io(ch, 2, 64);
      plugin->process(io, nullptr);
    }
    char line[200];
    std::snprintf(line, sizeof(line), "a bundle whose modgui.ttl names ports as blank nodes loads "
                  "(%s), with its one parameter (%d) and every port connected (%s unconnected)",
                  ready ? "yes" : "no", plugin ? plugin->numParameters() : -1,
                  var("SONORE_LV2_FIXTURE_UNCONNECTED").c_str());
    check(ready && plugin->numParameters() == 1 &&
              var("SONORE_LV2_FIXTURE_UNCONNECTED") == "0",
          line);
  }
  removeBundle(bundle);
}

/** An LV2 latency is a float output port, and a plugin can write anything
 *  in it. The host converted it to uint32 as it found it: 1e10, which no
 *  uint32 holds, is undefined behaviour, and came back from latencySamples()
 *  as 1,410,065,408. It read the port the plugin was writing, too, from the
 *  main thread. A latency is now a number of samples below a billion, as
 *  the LADSPA side has it, read from the copy the audio thread leaves. */
/** What a plugin built with the DPF framework needs to exist: options:options,
 *  carrying its block size and rate. It refused to instantiate without them,
 *  and the host gave a plugin urid:map and nothing else. */
static void checkHostOptions(const std::string& fixtureBundle) {
  std::printf("── An LV2 plugin that will not instantiate without options:options ──\n");
  const std::string bundle = writeBundle(fixtureBundle, "sonore-lv2-options", R"TTL(
@prefix lv2: <http://lv2plug.in/ns/lv2core#> .
@prefix doap: <http://usefulinc.com/ns/doap#> .
@prefix opts: <http://lv2plug.in/ns/ext/options#> .
@prefix bufsz: <http://lv2plug.in/ns/ext/buf-size#> .
@prefix urid: <http://lv2plug.in/ns/ext/urid#> .
<urn:sonore:test:lv2-options> a lv2:Plugin ;
  doap:name "Sonore LV2 Options Fixture" ;
  lv2:binary <BINARY> ;
  lv2:requiredFeature opts:options , urid:map , bufsz:boundedBlockLength ;
  lv2:port [ a lv2:AudioPort , lv2:InputPort ; lv2:index 0 ; lv2:symbol "in" ; lv2:name "In" ] ,
    [ a lv2:AudioPort , lv2:OutputPort ; lv2:index 1 ; lv2:symbol "out" ; lv2:name "Out" ] .
)TTL");
  for (const char* name : {"SONORE_LV2_OPTIONS_MAX", "SONORE_LV2_OPTIONS_MIN", "SONORE_LV2_OPTIONS_RATE",
                           "SONORE_LV2_OPTIONS_SEQUENCE", "SONORE_LV2_OPTIONS_BOUNDED",
                           "SONORE_LV2_OPTIONS_UNMAP"})
    clearVar(name);
  {
    const auto found = sonore::host::describeFile(bundle);
    auto plugin = found.size() == 1 ? sonore::host::loadPlugin(found.front()) : nullptr;
    const bool ready = plugin && plugin->prepare(44100.0, 96, 2);
    check(ready, "a plugin that requires options:options instantiates and prepares");
    char line[240];
    std::snprintf(line, sizeof(line),
                  "...told its block size and rate (max %s, min %s, %s Hz, a %s-byte sequence)",
                  var("SONORE_LV2_OPTIONS_MAX").c_str(), var("SONORE_LV2_OPTIONS_MIN").c_str(),
                  var("SONORE_LV2_OPTIONS_RATE").c_str(), var("SONORE_LV2_OPTIONS_SEQUENCE").c_str());
    check(var("SONORE_LV2_OPTIONS_MAX") == "96" && var("SONORE_LV2_OPTIONS_MIN") == "1" &&
              var("SONORE_LV2_OPTIONS_RATE") == "44100" && std::atoi(var("SONORE_LV2_OPTIONS_SEQUENCE").c_str()) >= 8192,
          line);
    check(var("SONORE_LV2_OPTIONS_BOUNDED") == "1",
          "...and promised bounded block lengths, which process() keeps to");
    check(var("SONORE_LV2_OPTIONS_UNMAP") == "1", "...and given urid:unmap, answering for the same map");
    if (ready) {
      std::vector<float> l(96, 0.5f), r(96, 0.5f);
      float* ch[2] = {l.data(), r.data()};
      sonore::AudioBlock<float> io(ch, 2, 96);
      plugin->process(io, nullptr);
      check(l[95] == 0.5f, "...and runs");
      plugin->release();
    }
  }
  removeBundle(bundle);
}

/** What a sampler needs to load a file without stopping the audio: the
 *  worker extension. Without worker:schedule such a plugin does not
 *  instantiate, and the host gave none. */
static void checkHostWorker(const std::string& fixtureBundle) {
  std::printf("── An LV2 plugin that does its slow work on the host's worker ──\n");
  const std::string bundle = writeBundle(fixtureBundle, "sonore-lv2-worker", R"TTL(
@prefix lv2: <http://lv2plug.in/ns/lv2core#> .
@prefix doap: <http://usefulinc.com/ns/doap#> .
@prefix work: <http://lv2plug.in/ns/ext/worker#> .
<urn:sonore:test:lv2-worker> a lv2:Plugin ;
  doap:name "Sonore LV2 Worker Fixture" ;
  lv2:binary <BINARY> ;
  lv2:requiredFeature work:schedule ;
  lv2:extensionData work:interface ;
  lv2:port [ a lv2:AudioPort , lv2:InputPort ; lv2:index 0 ; lv2:symbol "in" ; lv2:name "In" ] ,
    [ a lv2:AudioPort , lv2:OutputPort ; lv2:index 1 ; lv2:symbol "out" ; lv2:name "Out" ] .
)TTL");
  {
    const auto found = sonore::host::describeFile(bundle);
    auto plugin = found.size() == 1 ? sonore::host::loadPlugin(found.front()) : nullptr;
    const bool ready = plugin && plugin->prepare(48000.0, 64, 2);
    check(ready, "a plugin that requires worker:schedule instantiates and prepares");
    if (ready) {
      std::vector<float> l(64, 0.0f), r(64, 0.0f);
      float* ch[2] = {l.data(), r.data()};
      sonore::AudioBlock<float> io(ch, 2, 64);
      // A block every millisecond for up to two seconds, until twenty jobs
      // have come back -- the worker answers on its own thread, in its time.
      int blocks = 0;
      for (; blocks < 2000 && std::atoi(var("SONORE_LV2_WORKER_RESPONSES").c_str()) < 20; ++blocks) {
        plugin->process(io, nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      char line[240];
      std::snprintf(line, sizeof(line),
                    "...its jobs are done and answered in order (%s answers to %s runs, %s wrong, "
                    "work() off the audio thread: %s)",
                    var("SONORE_LV2_WORKER_RESPONSES").c_str(), var("SONORE_LV2_WORKER_RUNS").c_str(),
                    var("SONORE_LV2_WORKER_BAD").c_str(), var("SONORE_LV2_WORKER_OFF_THREAD").c_str());
      check(std::atoi(var("SONORE_LV2_WORKER_RESPONSES").c_str()) >= 20 &&
                var("SONORE_LV2_WORKER_BAD") == "0" && var("SONORE_LV2_WORKER_OFF_THREAD") == "1",
            line);
      check(var("SONORE_LV2_WORKER_END_RUNS") == var("SONORE_LV2_WORKER_RUNS"),
            "...and end_run() follows every run()");
      plugin->release();
    }
    // Destroyed here with work possibly still queued: the worker stops
    // before the instance goes.
  }
  removeBundle(bundle);

  // The ring between the two threads, round its end many times: messages of
  // every length from 0 to 40 bytes through 100 bytes of room, each one
  // checked whole, and one too long for the room refused.
  {
    sonore::host::HostLv2WorkRing ring;
    ring.allocate(100);
    std::vector<uint8_t> out(ring.capacity());
    int whole = 0, total = 0;
    uint8_t pattern[64];
    for (int n = 0; n < 2000; ++n) {
      const uint32_t length = (uint32_t) (n % 41);
      for (uint32_t i = 0; i < length; ++i) pattern[i] = (uint8_t) (n * 7 + i);
      if (!ring.write(length, pattern)) continue;
      ++total;
      uint32_t got = 0;
      if (ring.read(out.data(), &got) && got == length && std::memcmp(out.data(), pattern, length) == 0)
        ++whole;
    }
    char line[160];
    std::snprintf(line, sizeof(line), "the worker's ring keeps every message whole round its end "
                  "(%d of %d)", whole, total);
    check(total == 2000 && whole == total, line);
    check(!ring.write(97, pattern), "...and refuses one longer than its room");
  }
}

static void checkLatencyPort(const std::string& fixtureBundle) {
  std::printf("── An LV2 latency port that holds no number of samples ──\n");
  const std::string bundle = writeBundle(fixtureBundle, "sonore-lv2-latency", R"TTL(
@prefix lv2: <http://lv2plug.in/ns/lv2core#> .
@prefix atom: <http://lv2plug.in/ns/ext/atom#> .
@prefix midi: <http://lv2plug.in/ns/ext/midi#> .
@prefix doap: <http://usefulinc.com/ns/doap#> .
<urn:sonore:test:lv2-atoms> a lv2:Plugin ;
  doap:name "Sonore LV2 Atoms Fixture" ;
  lv2:binary <BINARY> ;
  lv2:port [ a lv2:InputPort , atom:AtomPort ; atom:bufferType atom:Sequence ;
             atom:supports midi:MidiEvent ; lv2:index 0 ; lv2:symbol "midi_in" ; lv2:name "MIDI In" ] ,
    [ a lv2:OutputPort , atom:AtomPort ; atom:bufferType atom:Sequence ;
      atom:supports midi:MidiEvent ; lv2:index 1 ; lv2:symbol "midi_out" ; lv2:name "MIDI Out" ] ,
    [ a lv2:InputPort , lv2:ControlPort ; lv2:index 2 ; lv2:symbol "mode" ; lv2:name "Mode" ;
      lv2:default 0 ; lv2:minimum 0 ; lv2:maximum 3 ] ,
    [ a lv2:OutputPort , lv2:ControlPort ; lv2:index 3 ; lv2:symbol "latency" ;
      lv2:name "Latency" ] .
)TTL");
  {
    const auto found = sonore::host::describeFile(bundle);
    auto plugin = found.size() == 1 ? sonore::host::loadPlugin(found.front()) : nullptr;
    int mode = -1;
    for (int i = 0; plugin && i < plugin->numParameters(); ++i)
      if (plugin->parameter(i).name == "Mode") mode = i;
    const bool ready = plugin && mode >= 0 && plugin->prepare(48000.0, 64, 2);
    check(ready, "the fixture with a latency port loads and prepares");
    if (ready) {
      plugin->setParameterValue(mode, 3.0);
      std::vector<float> l(64, 0.0f), r(64, 0.0f);
      float* ch[2] = {l.data(), r.data()};
      sonore::AudioBlock<float> io(ch, 2, 64);
      plugin->process(io, nullptr);
      char line[160];
      std::snprintf(line, sizeof(line), "a latency port reading 1e10 is no latency (%u samples "
                    "reported)", plugin->latencySamples());
      check(plugin->latencySamples() == 0, line);
      plugin->release();
    }
  }
  removeBundle(bundle);
}

/** The host's own ports, named by what they ARE -- lv2:designation -- and not
 *  by their symbols, which the bundle chooses. The DPF framework calls the
 *  first two "lv2_latency" and "lv2_enabled"; the host looked for "latency",
 *  "enabled" and "freewheel" alone, so such a plugin reported no latency --
 *  a graph did not compensate it -- had no bypass, and listed the host's
 *  switches among its user's parameters. The fixture's port 3 reports
 *  0.25 + mode / 8: mode 512 is 64 samples. */
static void checkDesignatedPorts(const std::string& fixtureBundle) {
  std::printf("── LV2 ports the host drives, known by their designation ──\n");
  const std::string bundle = writeBundle(fixtureBundle, "sonore-lv2-designated", R"TTL(
@prefix lv2: <http://lv2plug.in/ns/lv2core#> .
@prefix atom: <http://lv2plug.in/ns/ext/atom#> .
@prefix midi: <http://lv2plug.in/ns/ext/midi#> .
@prefix doap: <http://usefulinc.com/ns/doap#> .
<urn:sonore:test:lv2-atoms> a lv2:Plugin ;
  doap:name "Sonore LV2 Designated Fixture" ;
  lv2:binary <BINARY> ;
  lv2:port [ a lv2:InputPort , atom:AtomPort ; atom:bufferType atom:Sequence ;
             atom:supports midi:MidiEvent ; lv2:index 0 ; lv2:symbol "midi_in" ; lv2:name "MIDI In" ] ,
    [ a lv2:OutputPort , atom:AtomPort ; atom:bufferType atom:Sequence ;
      atom:supports midi:MidiEvent ; lv2:index 1 ; lv2:symbol "midi_out" ; lv2:name "MIDI Out" ] ,
    [ a lv2:InputPort , lv2:ControlPort ; lv2:index 2 ; lv2:symbol "mode" ; lv2:name "Mode" ;
      lv2:default 0 ; lv2:minimum 0 ; lv2:maximum 1000 ] ,
    [ a lv2:OutputPort , lv2:ControlPort ; lv2:index 3 ; lv2:symbol "lv2_latency" ;
      lv2:name "Latency" ; lv2:designation lv2:latency ;
      lv2:portProperty lv2:reportsLatency , lv2:integer ] ,
    [ a lv2:InputPort , lv2:ControlPort ; lv2:index 4 ; lv2:symbol "lv2_enabled" ;
      lv2:name "Enabled" ; lv2:default 1 ; lv2:minimum 0 ; lv2:maximum 1 ;
      lv2:designation lv2:enabled ; lv2:portProperty lv2:toggled ] ,
    [ a lv2:InputPort , lv2:ControlPort ; lv2:index 5 ; lv2:symbol "lv2_freewheel" ;
      lv2:name "Freewheel" ; lv2:default 0 ; lv2:minimum 0 ; lv2:maximum 1 ;
      lv2:designation lv2:freeWheeling ; lv2:portProperty lv2:toggled ] .
)TTL");
  check(!bundle.empty(), "a bundle whose host ports are named as DPF names them is written");
  if (bundle.empty()) return;
  {
    const auto found = sonore::host::describeFile(bundle);
    auto plugin = found.size() == 1 ? sonore::host::loadPlugin(found.front()) : nullptr;
    int mode = -1;
    std::string names;
    for (int i = 0; plugin && i < plugin->numParameters(); ++i) {
      if (plugin->parameter(i).name == "Mode") mode = i;
      names += (names.empty() ? "" : ", ") + plugin->parameter(i).name;
    }
    const bool ready = plugin && mode >= 0 && plugin->prepare(48000.0, 64, 2);
    check(ready, "...which loads and prepares");
    char line[240];
    std::snprintf(line, sizeof(line), "...and lists only the plugin's own control (%s)",
                  names.c_str());
    check(plugin && plugin->numParameters() == 1, line);
    if (ready) {
      plugin->setParameterValue(mode, 512.0);
      std::vector<float> l(64, 0.0f), r(64, 0.0f);
      float* ch[2] = {l.data(), r.data()};
      sonore::AudioBlock<float> io(ch, 2, 64);
      plugin->process(io, nullptr);
      std::snprintf(line, sizeof(line), "a latency port designated lv2:latency is the latency "
                    "(%u samples, 64 written)", plugin->latencySamples());
      check(plugin->latencySamples() == 64, line);
      const bool bypassed = plugin->setBypassed(true) && plugin->isBypassed();
      std::snprintf(line, sizeof(line), "a port designated lv2:enabled is the bypass (has one: %s, "
                    "bypassed: %s)", plugin->hasBypass() ? "yes" : "no", bypassed ? "yes" : "no");
      check(plugin->hasBypass() && bypassed, line);
      plugin->setBypassed(false);
      check(!plugin->isBypassed(), "...and switching it back un-bypasses");
      plugin->release();
    }
  }
  removeBundle(bundle);
}

/** A state restored while the audio runs. LV2's restore() "MUST NOT be
 *  called concurrently with any other function on the same plugin
 *  instance" -- unlike CLAP's state load and VST3's setState, which a plugin
 *  must take beside its process() -- and the host called it from the main
 *  thread straight into a plugin the audio thread was running, and its
 *  worker working: a sampler that swaps its sample in restore() swapped it
 *  under its own run(). The fixture counts every meeting. */
static void checkRestoreAlone(const std::string& fixtureBundle) {
  std::printf("── An LV2 state restored while the audio runs ──\n");
  const std::string bundle = writeBundle(fixtureBundle, "sonore-lv2-state", R"TTL(
@prefix lv2: <http://lv2plug.in/ns/lv2core#> .
@prefix doap: <http://usefulinc.com/ns/doap#> .
@prefix work: <http://lv2plug.in/ns/ext/worker#> .
@prefix state: <http://lv2plug.in/ns/ext/state#> .
<urn:sonore:test:lv2-state> a lv2:Plugin ;
  doap:name "Sonore LV2 State Fixture" ;
  lv2:binary <BINARY> ;
  lv2:requiredFeature work:schedule ;
  lv2:extensionData state:interface , work:interface ;
  lv2:port [ a lv2:AudioPort , lv2:InputPort ; lv2:index 0 ; lv2:symbol "in" ; lv2:name "In" ] ,
    [ a lv2:AudioPort , lv2:OutputPort ; lv2:index 1 ; lv2:symbol "out" ; lv2:name "Out" ] .
)TTL");
  check(!bundle.empty(), "a bundle for the state fixture is written");
  if (bundle.empty()) return;
  {
    const auto found = sonore::host::describeFile(bundle);
    auto plugin = found.size() == 1 ? sonore::host::loadPlugin(found.front()) : nullptr;
    const bool ready = plugin && plugin->prepare(48000.0, 64, 2);
    std::vector<uint8_t> blob;
    const bool saved = ready && plugin->saveState(blob);
    check(ready && saved, "...which loads, prepares and saves its state");
    if (ready && saved) {
      std::atomic<bool> stop{false};
      std::atomic<int> blocks{0};
      std::thread audio([&] {
        std::vector<float> l(64, 0.5f), r(64, 0.5f);
        float* ch[2] = {l.data(), r.data()};
        sonore::AudioBlock<float> io(ch, 2, 64);
        while (!stop.load()) {
          plugin->process(io, nullptr);
          // A reset now and then, from the thread that calls process(), as
          // a host's transport does: its deactivate() and activate() are no
          // more allowed beside a restore() or a work() than run() is.
          if (blocks.fetch_add(1) % 50 == 49) plugin->reset();
        }
      });
      setVar("SONORE_LV2_STATE_ACTIVATIONS", "0");
      int loaded = 0;
      double longest = 0.0;
      for (int i = 0; i < 40; ++i) {
        const auto start = std::chrono::steady_clock::now();
        if (plugin->loadState(blob.data(), blob.size())) ++loaded;
        const std::chrono::duration<double, std::milli> took = std::chrono::steady_clock::now() - start;
        if (took.count() > longest) longest = took.count();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      const int before = blocks.load();
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      stop.store(true);
      audio.join();
      const int after = blocks.load();
      plugin->release();
      plugin.reset(); // its last count, from cleanup()
      char line[240];
      std::snprintf(line, sizeof(line), "40 restores while the audio runs and resets (%d taken, %s seen, "
                    "%s activations), and none of them beside a run(), a work() or a reset's "
                    "deactivate() and activate() (%s overlaps)", loaded,
                    var("SONORE_LV2_STATE_RESTORES").c_str(), var("SONORE_LV2_STATE_ACTIVATIONS").c_str(),
                    var("SONORE_LV2_STATE_OVERLAPS").c_str());
      check(loaded == 40 && var("SONORE_LV2_STATE_RESTORES") == "40" &&
                std::atoi(var("SONORE_LV2_STATE_ACTIVATIONS").c_str()) > 1 &&
                var("SONORE_LV2_STATE_OVERLAPS") == "0",
            line);
      // And each waits for the block and the job under way, no more, with an
      // audio thread that never pauses -- an offline render -- and a worker
      // always busy. Waiting its turn for the instance and the worker's lock,
      // a restore lost every race to a thread that took them straight back:
      // seconds a load, 168 s for these 40 on a busy machine.
      std::snprintf(line, sizeof(line), "...and none of them waits on the audio thread or the "
                    "worker (the longest took %.1f ms; its own restore() takes 2)", longest);
      check(longest < 100.0, line);
      std::snprintf(line, sizeof(line), "...and the audio went on, before and after them (%d blocks, "
                    "%d after the last; %s run by the plugin)", after, after - before,
                    var("SONORE_LV2_STATE_RUNS").c_str());
      check(before > 0 && after > before && std::atoi(var("SONORE_LV2_STATE_RUNS").c_str()) > 1000, line);
    }
  }
  removeBundle(bundle);
}

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 2) {
    std::printf("usage: lv2_ports_test <SonorePortsFixture.lv2>\n");
    return 2;
  }
  std::printf("── LV2 bundle path and port connection: %s ──\n", argv[1]);

  const auto found = sonore::host::describeFile(argv[1]);
  check(found.size() == 1 && found.front().format == "LV2", "the fixture bundle is described");
  if (found.empty()) return 1;
  auto plugin = sonore::host::loadPlugin(found.front());
  check(plugin != nullptr, "...and loads");
  if (!plugin) return 1;
  check(plugin->prepare(48000.0, 256, 2), "...and prepares");

  std::vector<float> l(256, 0.25f), r(256, 0.75f);
  float* ch[2] = {l.data(), r.data()};
  sonore::AudioBlock<float> io(ch, 2, 256);
  plugin->process(io, nullptr);

  char line[1200];
  // The fixture is MONO -- one audio input, one output, at a gain of 1 --
  // and the caller stereo. The plugin's one output is what both sides hear,
  // as a mono effect is heard on a stereo track in the other three formats
  // here. The right channel was connected to nothing and came back silent.
  std::snprintf(line, sizeof(line), "a mono plugin on a stereo caller is heard on both sides "
                "(left %g, right %g; 0.25 each)", (double) l[100], (double) r[100]);
  check(l[100] == 0.25f && r[100] == 0.25f && r[255] == 0.25f, line);
  std::fill(r.begin(), r.end(), 0.25f);
  const std::string bundle = var("SONORE_LV2_FIXTURE_BUNDLE");
  const char last = bundle.empty() ? '\0' : bundle.back();
  std::snprintf(line, sizeof(line), "instantiate() is given the bundle path WITH its trailing "
                "separator (\"%s\")", bundle.c_str());
  check(last == '/' || last == '\\', line);

  const std::string unconnected = var("SONORE_LV2_FIXTURE_UNCONNECTED");
  std::snprintf(line, sizeof(line), "run() finds every port connected, CV ports included "
                "(%s unconnected)", unconnected.empty() ? "0" : unconnected.c_str());
  check(unconnected.empty() || unconnected == "0", line);

  const std::string mapErrors = var("SONORE_LV2_FIXTURE_MAP_ERRORS");
  std::snprintf(line, sizeof(line), "urid:map from four threads at once answers consistently "
                "(%s inconsistent ids)", mapErrors.c_str());
  check(mapErrors == "0", line);

  // ── A state blob from a session file it did not write ──────────────────
  //
  // The host's LV2 state is its own format: the control values, then the
  // state extension's properties. It wrote each value straight into its
  // port as it read it, so a NaN or 1e30 in a session file went to the
  // plugin as the gain -- setParameterValue() clamps, this did not -- and a
  // blob found broken further on returned false with its controls already
  // applied: half a restore, reported as none. The fixture's one control is
  // its gain, 0..2, read every sample.
  {
    plugin->setParameterValue(0, 1.0);
    std::vector<uint8_t> good;
    check(plugin->saveState(good) && good.size() >= 12, "the fixture's state saves");
    auto withGain = [&](float gain, bool truncate) {
      std::vector<uint8_t> blob(good.begin(), good.begin() + 8);
      std::memcpy(blob.data() + 4, &gain, sizeof(gain));
      const uint32_t properties = truncate ? 5u : 0u; // five promised, none there
      for (int i = 0; i < 4; ++i) blob.push_back((uint8_t) (properties >> (8 * i)));
      return blob;
    };
    const std::vector<uint8_t> nan = withGain(std::numeric_limits<float>::quiet_NaN(), false);
    const bool tookNan = plugin->loadState(nan.data(), nan.size());
    std::fill(l.begin(), l.end(), 0.25f);
    std::fill(r.begin(), r.end(), 0.25f);
    plugin->process(io, nullptr);
    std::snprintf(line, sizeof(line), "a NaN gain in a blob is refused (%s), the gain stays %g "
                  "and the output finite (%g)", tookNan ? "taken" : "refused",
                  plugin->parameterValue(0), (double) l[0]);
    check(!tookNan && plugin->parameterValue(0) == 1.0 && std::isfinite(l[0]), line);

    const std::vector<uint8_t> huge = withGain(1e30f, false);
    const bool tookHuge = plugin->loadState(huge.data(), huge.size());
    std::snprintf(line, sizeof(line), "a gain of 1e30 is held to the port's maximum (%g)",
                  plugin->parameterValue(0));
    check(tookHuge && plugin->parameterValue(0) == 2.0, line);

    plugin->setParameterValue(0, 1.0);
    const std::vector<uint8_t> cut = withGain(0.5f, true);
    const bool tookCut = plugin->loadState(cut.data(), cut.size());
    std::snprintf(line, sizeof(line), "a blob cut short after its controls is refused whole: "
                  "%s, gain %g", tookCut ? "taken" : "refused", plugin->parameterValue(0));
    check(!tookCut && plugin->parameterValue(0) == 1.0, line);
  }

  plugin->release();
  plugin.reset();

  checkAtomOutput(argv[1]);
  checkPortIndices(argv[1]);
  checkEditorHearsOutputs(argv[1]);
  checkUiAtoms(argv[1]);
  checkUiClosesItself(argv[1]);
  checkBlankNodesStayInTheirFile(argv[1]);
  checkLatencyPort(argv[1]);
  checkDesignatedPorts(argv[1]);
  checkHostOptions(argv[1]);
  checkHostWorker(argv[1]);
  checkRestoreAlone(argv[1]);

  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  if (g_failures == 0) std::printf("SONORE LV2 PORTS PASSED\n");
  return g_failures == 0 ? 0 : 1;
}
