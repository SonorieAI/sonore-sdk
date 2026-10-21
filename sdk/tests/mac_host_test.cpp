// SPDX-License-Identifier: Apache-2.0
// The macOS hosting work, the half that runs anywhere.
//
// host_au.h, audio_workgroup.h, the realtime ThreadPool and
// gfx/nsview_component.h are mostly Apple API calls, and there is no Mac in
// this loop: that half is compiled for macOS (mac_syntax_mac_host.cpp) and
// run on a Mac by mac_host_selftest.cpp. What is here is everything those
// calls are fed through -- the AU ids a session stores, the parameter
// mapping, the channel negotiation, the MIDI a unit emits, the scheduling
// arithmetic -- and the promise every non-Apple build makes: a workgroup
// token that is inactive and SAYS why, a pool that runs its jobs anyway, a
// loader and a component that refuse with a reason instead of a crash.
#include <sonore/audio_workgroup.h>
#include <sonore/gfx/nsview_component.h>
#include <sonore/host.h>
#include <sonore/thread_pool.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
  std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++failures;
}

bool approx(double a, double b, double tolerance = 1e-9) { return std::fabs(a - b) <= tolerance; }

bool contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

using namespace sonore;
namespace au = sonore::host::detail;

// ── Workgroups ─────────────────────────────────────────────────────────────

void testWorkgroupOffApple() {
  std::printf("\n-- AudioWorkgroup / WorkgroupToken, off macOS --\n");
#if !defined(__APPLE__)
  check(!AudioWorkgroup::isSupported(), "no os_workgroup on this platform");
  const char* reason = AudioWorkgroup::unavailableReason();
  check(reason && contains(reason, "Apple"), "and the reason says so");

  AudioWorkgroup empty;
  check(!empty.isValid() && !empty, "a default workgroup is empty");
  check(empty.maxParallelThreadCount() == 0, "an empty one has no parallel threads");
  check(!empty.isCancelled(), "and is not 'cancelled'");

  // Something that would be an os_workgroup_t on a Mac: nothing here can
  // retain it, so nothing is held.
  int notAWorkgroup = 0;
  AudioWorkgroup retained = AudioWorkgroup::retaining(&notAWorkgroup);
  check(!retained.isValid(), "retaining() holds nothing where the OS cannot retain");

  WorkgroupToken token;
  check(token.status() == WorkgroupToken::Status::kNotJoined, "a new token has not joined");
  check(!empty.join(token), "joining fails");
  check(!token.isActive(), "the token is inactive");
  check(token.status() == WorkgroupToken::Status::kUnsupported, "because the platform has none");
  check(contains(token.describe(), "Apple"), "and describe() says that");
  check(!token.leave(), "leaving a token that never joined does nothing");
  check(!token.isAttachedTo(empty), "it is attached to nothing");

  AudioWorkgroup copy = empty;
  AudioWorkgroup moved = std::move(copy);
  check(!moved.isValid() && moved == empty, "copies and moves of an empty one stay empty");
  check(moved.detachHandle() == nullptr, "and there is no handle to detach");
#else
  check(true, "(built for macOS: see mac_host_selftest.cpp)");
#endif
}

void testRealtimeHints() {
  std::printf("\n-- RealtimeThreadHints and Mach's time-constraint arithmetic --\n");
  const RealtimeThreadHints hints = RealtimeThreadHints::forBlock(48000.0, 512);
  check(hints.isValid(), "512 frames at 48 kHz is a valid hint");
  check(approx(hints.periodMs, 512.0 / 48.0), "period = one block, 10.667 ms");
  check(approx(hints.computationMs, 256.0 / 48.0), "computation = half the block by default");
  check(approx(hints.constraintMs, hints.periodMs), "constraint = the period");
  check(!RealtimeThreadHints::forBlock(0.0, 512).isValid(), "no rate, no hint");
  check(!RealtimeThreadHints::forBlock(48000.0, 0).isValid(), "no block, no hint");
  check(approx(RealtimeThreadHints::forBlock(1000.0, 1000, 0.001).computationMs, 50.0),
        "a busy fraction under 5% is raised to 5%");
  check(approx(RealtimeThreadHints::forBlock(1000.0, 1000, 3.0).computationMs, 1000.0),
        "and one over 100% is held at 100%");

  // Known answers: Intel's timebase is 1/1 (ticks are nanoseconds); Apple
  // silicon's is 125/3 (a 24 MHz clock).
  check(au::aucode::fourCC('a', 'b', 'c', 'd') == 0x61626364u, "fourCC packs big-endian");
  check(sonore::detail::machTicksFromMs(1.0, 1, 1) == 1000000u, "1 ms = 1,000,000 ticks at 1/1");
  check(sonore::detail::machTicksFromMs(1.0, 125, 3) == 24000u, "1 ms = 24,000 ticks at 125/3");
  check(sonore::detail::machTicksFromMs(0.0, 1, 1) == 0u, "0 ms is 0 ticks");
  check(sonore::detail::machTicksFromMs(1.0, 0, 3) == 0u, "a broken timebase gives 0, not a crash");
  check(sonore::detail::machTicksFromMs(1.0e9, 1, 1) == 0xFFFFFFFFu, "and huge values saturate");

  const sonore::detail::RealtimePolicyTicks silicon = sonore::detail::realtimePolicyTicks(hints, 125, 3);
  check(silicon.period == 256000u, "48k/512 on Apple silicon: period 256,000 ticks");
  check(silicon.computation == 128000u, "computation 128,000 ticks");
  check(silicon.constraint == 256000u, "constraint 256,000 ticks");

  // 8 frames at 192 kHz is 42 us: under the kernel's 50 us floor, so
  // computation, constraint and period are all lifted to it.
  const sonore::detail::RealtimePolicyTicks tiny =
      sonore::detail::realtimePolicyTicks(RealtimeThreadHints::forBlock(192000.0, 8), 1, 1);
  check(tiny.computation == 50000u, "a tiny block's computation is lifted to 50 us");
  check(tiny.constraint >= tiny.computation, "the constraint is never under the computation");
  check(tiny.period >= tiny.constraint, "and the period never under the constraint");

  RealtimeThreadHints huge;
  huge.periodMs = 200.0;
  huge.computationMs = 150.0;
  huge.constraintMs = 200.0;
  const sonore::detail::RealtimePolicyTicks capped = sonore::detail::realtimePolicyTicks(huge, 1, 1);
  check(capped.computation == 50000000u, "computation is capped at the kernel's 50 ms");
  check(capped.constraint == 200000000u, "the constraint is left where it was");

  std::string why;
#if !defined(__APPLE__)
  check(!promoteCurrentThreadToRealtime(hints, &why), "promotion fails off macOS");
  check(contains(why, "macOS"), "and says it is a macOS mechanism");
#endif
  why.clear();
  check(!promoteCurrentThreadToRealtime(RealtimeThreadHints(), &why), "empty hints are refused");
  check(contains(why, "empty"), "with the reason");
}

// ── The realtime pool ──────────────────────────────────────────────────────

bool waitUntil(const std::function<bool()>& done, int ms = 3000) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (!done()) {
    if (std::chrono::steady_clock::now() > deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

void testThreadPool() {
  std::printf("\n-- ThreadPool: realtime workers and the workgroup --\n");
  {
    ThreadPool pool(3, RealtimeThreadHints::forBlock(48000.0, 256));
    check(pool.isRealtime() && pool.numThreads() == 3, "a realtime pool of three");
    std::atomic<int> ran{0};
    for (int i = 0; i < 20; ++i) pool.addJob([&ran](const std::atomic<bool>&) { ran.fetch_add(1); });
    check(pool.waitForAll(5000) && ran.load() == 20, "it runs its jobs like any pool");
    check(waitUntil([&pool]() {
            const ThreadPool::WorkgroupState s = pool.workgroupState();
            return s.settled == s.workers;
          }),
          "every worker settles its hints");
    ThreadPool::WorkgroupState state = pool.workgroupState();
    check(state.workers == 3, "three workers report");
    check(!state.hasWorkgroup && state.joined == 0, "no workgroup, none joined");
#if !defined(__APPLE__)
    check(state.realtime == 0, "none promoted off macOS");
    check(contains(state.message, "macOS"), "and the pool says why");
#endif
    const uint64_t before = state.generation;
    check(pool.setAudioWorkgroup(AudioWorkgroup()), "setAudioWorkgroup on a realtime pool is taken");
    check(pool.workgroupState().generation == before + 1, "a new generation");
    check(waitUntil([&pool]() {
            const ThreadPool::WorkgroupState s = pool.workgroupState();
            return s.settled == s.workers;
          }),
          "which every worker settles");
    check(pool.setRealtimeHints(RealtimeThreadHints::forBlock(44100.0, 128)), "new hints are taken");
    // The audio thread's way in: no lock, no allocation, applied before the
    // next job. Offering what the pool already has changes nothing.
    pool.offerAudioWorkgroup(AudioWorkgroup());
    ran.store(0);
    pool.addJob([&ran](const std::atomic<bool>&) { ran.fetch_add(1); });
    check(pool.waitForAll(5000) && ran.load() == 1, "a job after an offer runs");
    check(pool.workgroupState().message.size() > 0 || AudioWorkgroup::isSupported(),
          "the state still explains itself");
  }
  {
    ThreadPool plain(2);
    check(!plain.isRealtime(), "an ordinary pool is not realtime");
    check(!plain.setAudioWorkgroup(AudioWorkgroup()), "and refuses a workgroup");
    check(contains(plain.workgroupState().message, "not a realtime pool"), "saying why");
    plain.offerAudioWorkgroup(AudioWorkgroup());
    check(!plain.setRealtimeHints(RealtimeThreadHints::forBlock(48000.0, 64)), "and hints");
    std::atomic<int> ran{0};
    plain.addJob([&ran](const std::atomic<bool>&) { ran.fetch_add(1); });
    check(plain.waitForAll(5000) && ran.load() == 1, "and still runs its jobs");
  }
  {
    ThreadPool wide(64, RealtimeThreadHints::forBlock(48000.0, 64));
    check(wide.numThreads() == ThreadPool::kMaxRealtimeThreads, "a realtime pool is capped at 16");
  }
}

// ── Audio Unit ids, statuses, names ────────────────────────────────────────

void testAuIds() {
  std::printf("\n-- Audio Unit ids --\n");
  using namespace au::aucode;
  check(kTypeEffect == 0x61756678u && kTypeMusicDevice == 0x61756D75u, "'aufx' and 'aumu' as Apple numbers them");
  const uint32_t dely = fourCC('d', 'e', 'l', 'y'), appl = fourCC('a', 'p', 'p', 'l');
  check(au::auCodeText(kTypeEffect) == "aufx", "a code as text");
  check(au::auIdFor(kTypeEffect, dely, appl) == "aufx:dely:appl", "an id: type:subtype:manufacturer");
  uint32_t t = 0, s = 0, m = 0;
  check(au::auParseId("aufx:dely:appl", &t, &s, &m) && t == kTypeEffect && s == dely && m == appl,
        "and back");
  const uint32_t dls = fourCC('d', 'l', 's', ' ');
  check(au::auIdFor(kTypeMusicDevice, dls, appl) == "aumu:dls :appl", "a trailing space stands for itself");
  check(au::auParseId("aumu:dls :appl", &t, &s, &m) && s == dls, "and parses back");
  const uint32_t odd = fourCC(':', '%', '\x01', 'z');
  const std::string oddText = au::auCodeText(odd);
  check(oddText == "%3A%25%01z", "':', '%' and control bytes are escaped");
  uint32_t back = 0;
  check(au::auParseCode(oddText, &back) && back == odd, "and unescaped");
  const uint32_t high = 0xA9FF0080u;
  check(au::auParseCode(au::auCodeText(high), &back) && back == high, "every byte value round-trips");
  check(!au::auParseId("aufx:dely", &t, &s, &m), "two fields is not an id");
  check(!au::auParseId("aufx:dely:appl:x", &t, &s, &m), "four is not either");
  check(!au::auParseId("auf:dely:appl", &t, &s, &m), "a three-byte code is refused");
  check(!au::auParseId("aufxx:dely:appl", &t, &s, &m), "and a five-byte one");
  check(!au::auParseCode("ab%G1", &back), "a bad escape is refused");
  check(!au::auParseCode("abc%4", &back), "and a cut-off one");

  std::printf("\n-- statuses, versions, names --\n");
  check(contains(au::auStatusText(-10868), "kAudioUnitErr_FormatNotSupported"), "-10868 by name");
  check(contains(au::auStatusText(-10874), "TooManyFramesToProcess"), "-10874 by name");
  check(contains(au::auStatusText(-66754), "InstanceTimedOut"), "-66754 by name");
  check(au::auStatusText(0x666D743F) == "'fmt?' (1718449215)", "a four-character status as one");
  check(au::auStatusText(-12345) == "OSStatus -12345", "anything else as a number");
  check(au::auVersionText(0x00010203u) == "1.2.3", "a version: 0x00010203 is 1.2.3");
  check(au::auVersionText(65536u) == "1.0.0", "65536 is 1.0.0");
  std::string vendor, name;
  au::auSplitName("Apple: AUDelay", &vendor, &name);
  check(vendor == "Apple" && name == "AUDelay", "'Apple: AUDelay' splits at the colon");
  au::auSplitName("Plain", &vendor, &name);
  check(vendor.empty() && name == "Plain", "a name with no vendor is all name");
  au::auSplitName("A: B: C", &vendor, &name);
  check(vendor == "A" && name == "B: C", "only the first ': ' splits");
  check(std::string(au::auTypeWord(kTypeMusicDevice)) == "Instrument", "aumu is an instrument");
  check(au::auTypeTakesMidi(kTypeMusicEffect) && au::auTypeTakesMidi(kTypeMidiProcessor) &&
            !au::auTypeTakesMidi(kTypeEffect),
        "music effects and MIDI processors take MIDI, effects do not");
}

void testParameterMapping() {
  std::printf("\n-- AU parameters as HostedParam --\n");
  using namespace au::aucode;
  // Apple's documented bit positions, the ones the test shim had wrong.
  check(kParamIsWritable == 0x80000000u && kParamIsReadable == 0x40000000u, "readable/writable bits");
  check(kParamHasCFNameString == 0x08000000u, "HasCFNameString is bit 27");
  check(kUnitHertz == 8 && kUnitDecibels == 13 && kUnitMilliseconds == 24, "unit numbers");

  au::AuParamFacts cutoff;
  cutoff.id = 7;
  cutoff.name = "Cutoff";
  cutoff.unit = kUnitHertz;
  cutoff.minValue = 20.0f;
  cutoff.maxValue = 20000.0f;
  cutoff.defaultValue = 1000.0f;
  cutoff.flags = kParamIsReadable | kParamIsWritable;
  cutoff.clump = "Filter";
  const host::HostedParam p = au::auHostedParam(cutoff);
  check(p.id == 7 && p.name == "Cutoff" && p.module == "Filter", "id, name and clump carry over");
  check(approx(p.minValue, 20.0) && approx(p.maxValue, 20000.0) && approx(p.defaultValue, 1000.0),
        "plain range and default, as AU gives them");
  check(!p.stepped, "hertz is continuous");

  au::AuParamFacts mode = cutoff;
  mode.unit = kUnitIndexed;
  mode.name.clear();
  mode.minValue = 3.0f;
  mode.maxValue = 0.0f; // reversed
  mode.defaultValue = 9.0f;
  const host::HostedParam q = au::auHostedParam(mode);
  check(q.stepped, "indexed is stepped");
  check(approx(q.minValue, 0.0) && approx(q.maxValue, 3.0), "a reversed range is put right");
  check(approx(q.defaultValue, 3.0), "a default outside it is pulled in");
  check(q.name == "Parameter 7", "a nameless parameter gets one");
  mode.unit = kUnitBoolean;
  check(au::auHostedParam(mode).stepped, "boolean is stepped");

  check(au::auParamIsSettable(kParamIsWritable | kParamIsReadable), "writable is settable");
  check(!au::auParamIsSettable(kParamIsReadable), "read-only is not");
  check(!au::auParamIsSettable(kParamIsWritable | kParamMeterReadOnly), "nor is a meter");
  check(std::string(au::auUnitLabel(kUnitHertz)) == "Hz" &&
            std::string(au::auUnitLabel(kUnitDecibels)) == "dB" &&
            std::string(au::auUnitLabel(kUnitMilliseconds)) == "ms" &&
            std::string(au::auUnitLabel(kUnitGeneric)).empty(),
        "unit labels");
}

void testChannels() {
  std::printf("\n-- channel negotiation (kAudioUnitProperty_SupportedNumChannels) --\n");
  using Pair = au::AuChannelPair;
  check(au::auChannelEntryAdmits({-1, -1}, 2, 2) && !au::auChannelEntryAdmits({-1, -1}, 1, 2),
        "{-1,-1}: any count, the same both ways");
  check(au::auChannelEntryAdmits({-1, -2}, 1, 6) && au::auChannelEntryAdmits({-2, -1}, 3, 2),
        "{-1,-2} and {-2,-1}: any and any");
  check(au::auChannelEntryAdmits({-4, 2}, 3, 2) && !au::auChannelEntryAdmits({-4, 2}, 5, 2) &&
            !au::auChannelEntryAdmits({-4, 2}, 0, 2),
        "-4 is one to four");
  check(au::auChannelEntryAdmits({2, 2}, 2, 2) && !au::auChannelEntryAdmits({2, 2}, 1, 1),
        "a number is exactly that");

  auto first = [](const std::vector<Pair>& entries, int wanted, bool hasInput) {
    const std::vector<Pair> c = au::auChannelCandidates(entries, wanted, hasInput);
    return c.empty() ? Pair{-99, -99} : c.front();
  };
  Pair p = first({}, 2, true);
  check(p.in == 2 && p.out == 2, "no list means {-1,-1}: stereo in, stereo out");
  p = first({{1, 1}}, 2, true);
  check(p.in == 1 && p.out == 1, "a mono-only unit is run mono");
  p = first({{-1, -2}}, 6, true);
  check(p.in == 6 && p.out == 6, "a flexible one at the width asked for");
  p = first({{0, 2}}, 2, false);
  check(p.in == 0 && p.out == 2, "an instrument has no input");
  p = first({{0, -1}}, 4, false);
  check(p.in == 0 && p.out == 4, "{0,-1}: any output width");
  p = first({{2, 2}, {1, 1}}, 6, true);
  check(p.in == 2 && p.out == 2, "six asked, stereo is the best on offer");
  p = first({{1, 2}}, 2, true);
  check(p.in == 1 && p.out == 2, "a mono-to-stereo unit is offered as it is");
  const std::vector<Pair> all = au::auChannelCandidates({{-1, -1}}, 2, true);
  check(all.size() == 8 && all[1].in == 1, "every admissible width, best first, no repeats");
}

void testMidiPackets() {
  std::printf("\n-- the MIDI a unit emits, from CoreMIDI packets --\n");
  MidiBuffer out;
  const uint8_t running[] = {0x90, 0x3C, 0x64, 0x3C, 0x00};
  check(au::auParseMidiPacket(running, sizeof(running), 5, out) == 2, "running status: two events");
  auto at = [&out](int i) { return (out.begin() + i)->getMessage(); };
  check(at(0).isNoteOn() && at(0).getNoteNumber() == 60 && at(1).isNoteOff(),
        "a note on, then its note off by velocity 0");
  check(out.begin()->samplePosition == 5, "at the packet's sample offset");

  out.clear();
  const uint8_t clock[] = {0xF8, 0x90, 0x40, 0x7F};
  check(au::auParseMidiPacket(clock, sizeof(clock), 0, out) == 2, "a realtime byte and a note");
  check(at(0).getRawStatus() == 0xF8, "the clock comes through");

  out.clear();
  const uint8_t sysex[] = {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7, 0xB0, 0x07, 0x64};
  check(au::auParseMidiPacket(sysex, sizeof(sysex), 3, out) == 2, "a SysEx and a controller");
  check(out.getNumSysex() == 1 && out.sysexBegin()->length == 6, "the SysEx whole, F0 to F7");
  check(out.getNumEvents() == 1 && at(0).isController(), "the controller after it");

  out.clear();
  const uint8_t unterminated[] = {0xF0, 0x01, 0x02};
  check(au::auParseMidiPacket(unterminated, sizeof(unterminated), 0, out) == 0 &&
            out.getNumSysex() == 0,
        "half a SysEx is dropped, not delivered");

  out.clear();
  const uint8_t shortOnes[] = {0xC0, 0x05, 0xD0, 0x40};
  check(au::auParseMidiPacket(shortOnes, sizeof(shortOnes), 0, out) == 2, "two-byte messages");
  check(at(0).getRawData1() == 5 && at(1).isChannelPressure(), "program change and pressure");

  out.clear();
  const uint8_t truncated[] = {0x80, 0x3C};
  check(au::auParseMidiPacket(truncated, sizeof(truncated), 0, out) == 0, "a truncated message is not delivered");
  const uint8_t orphan[] = {0x3C, 0x64};
  check(au::auParseMidiPacket(orphan, sizeof(orphan), 0, out) == 0, "data with no status is ignored");
  const uint8_t broken[] = {0x90, 0x3C, 0x90, 0x3E, 0x50};
  check(au::auParseMidiPacket(broken, sizeof(broken), 0, out) == 1 && at(0).getNoteNumber() == 62,
        "a status where data belongs starts the next message");
  check(au::auParseMidiPacket(nullptr, 4, 0, out) == 0, "no data, no events");
}

// ── NSViewComponentWithParent, off macOS ───────────────────────────────────

void testNsViewComponent() {
  std::printf("\n-- NSViewComponentWithParent --\n");
  gfx::Component root;
  root.setBounds({100.0f, 100.0f, 400.0f, 300.0f});
  gfx::Component panel;
  panel.setBounds({10.0f, 20.0f, 200.0f, 100.0f});
  root.addChild(&panel);
  gfx::NSViewComponentWithParent embed;
  embed.setBounds({5.0f, 5.0f, 50.0f, 40.0f});
  panel.addChild(&embed);

  gfx::detail::NativeEmbedPlacement placed = gfx::detail::nativeEmbedPlacement(embed);
  check(placed.frame.x == 15.0f && placed.frame.y == 25.0f, "placed in root coordinates");
  check(placed.frame.w == 50.0f && placed.frame.h == 40.0f, "at its own size");
  check(placed.visible, "and shown");
  panel.setVisible(false);
  check(!gfx::detail::nativeEmbedPlacement(embed).visible, "hidden with a hidden ancestor");
  panel.setVisible(true);
  embed.setSize(0.0f, 40.0f);
  check(!gfx::detail::nativeEmbedPlacement(embed).visible, "and when it has no area");

#if !defined(__APPLE__)
  int fakeView = 0;
  check(!embed.setView(&fakeView), "embedding an NSView is refused off macOS");
  check(contains(embed.lastError(), "macOS"), "with the reason");
  check(embed.getView() == nullptr && embed.parentView() == nullptr && !embed.isAttached(),
        "and nothing is held");
  check(embed.setView(nullptr), "asking for no view is not an error");
  check(!embed.resizeToFitView(), "there is no view to fit");
#endif
  check(embed.followsViewSize(), "the component follows its view's size by default");
}

// ── host.h's view of Audio Units, off macOS ────────────────────────────────

void testHostOffApple() {
  std::printf("\n-- host.h and AudioUnitPluginFormat --\n");
  check(std::string(host::AudioUnitPluginFormat::formatName()) == "AU", "the format is called AU");
#if !defined(__APPLE__)
  std::string why;
  check(!host::AudioUnitPluginFormat::isAvailable(&why) && contains(why, "macOS"),
        "no AU runtime off macOS, and it says so");
  std::string error;
  check(host::AudioUnitPluginFormat::findAllComponents(&error).empty() && contains(error, "macOS"),
        "the registry scan finds nothing, and says why");
  host::PluginDescription d;
  d.format = "AU";
  d.id = "aufx:dely:appl";
  error.clear();
  check(!host::AudioUnitPluginFormat::createInstance(d, &error) && contains(error, "macOS"),
        "createInstance refuses with a reason");
  check(!host::loadPlugin(d), "loadPlugin routes AU and gets nothing");
  check(host::describeFile("C:\\nowhere\\Thing.component").empty(), "a .component is not described here");
  error.clear();
  check(host::AudioUnitPluginFormat::describeBundle("Thing.component", &error).empty() &&
            contains(error, "macOS"),
        "describeBundle says why");
  check(host::AudioUnitPluginFormat::defaultSearchPaths().empty(), "no AU folders off macOS");
  check(!host::AudioUnitPluginFormat::createsAsynchronously(d), "and nothing is created at all");
#endif
}

} // namespace

int main() {
  std::printf("mac-host: the portable half of AU hosting, workgroups and NSView embedding\n");
  testWorkgroupOffApple();
  testRealtimeHints();
  testThreadPool();
  testAuIds();
  testParameterMapping();
  testChannels();
  testMidiPackets();
  testNsViewComponent();
  testHostOffApple();
  std::printf("\n%s\n", failures == 0 ? "MAC HOST TESTS PASSED" : "MAC HOST TESTS FAILED");
  return failures == 0 ? 0 : 1;
}
