// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: PluginGraph against plugins whose behaviour the test controls.
//
// graph_test drives real plugin binaries, which is the truth about loading
// and the weakest way to make a plugin do one particular thing. These nodes
// are in-memory delay lines whose latency the test can move, so a review's
// findings about the graph stay found:
//   * a node's latency moving after prepare() left a diamond misaligned, and
//     nothing said so;
//   * a block the graph refused, or any block after a wiring change, went
//     out as the dry input;
//   * SysEx was dropped on an explicit MIDI edge.
#include <sonore/graph.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <memory>
#include <new>
#include <vector>

using namespace sonore;
using namespace sonore::host;

// The heap bytes handed out and not yet given back, for what the graph holds
// per node. Each block carries its size in front of it.
static std::atomic<long long> g_liveBytes{0};
void* operator new(std::size_t n) {
  void* p = std::malloc(n + 16);
  if (!p) throw std::bad_alloc();
  *static_cast<std::size_t*>(p) = n;
  g_liveBytes.fetch_add((long long) n, std::memory_order_relaxed);
  return static_cast<char*>(p) + 16;
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept {
  if (!p) return;
  char* block = static_cast<char*>(p) - 16;
  g_liveBytes.fetch_sub((long long) *reinterpret_cast<std::size_t*>(block), std::memory_order_relaxed);
  std::free(block);
}
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

static int g_checks = 0;
static int g_failures = 0;

static void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

/** A pure delay of `latency` samples that says so, and can be told to move. */
struct DelayNode : HostedPlugin {
  PluginDescription desc;
  HostedParam param;
  MidiBuffer produced;
  uint32_t latency = 0;
  bool restart = false;
  std::vector<std::deque<float>> lines;
  int sysexSeen = -1;
  explicit DelayNode(uint32_t lat) : latency(lat) {}
  const PluginDescription& description() const override { return desc; }
  bool isValid() const override { return true; }
  bool prepare(double, uint32_t, uint32_t ch) override {
    lines.assign(ch, std::deque<float>(latency, 0.0f));
    return true;
  }
  void release() override {}
  void reset() override {}
  void process(AudioBlock<float>& io, const MidiBuffer* midi) override {
    sysexSeen = midi ? midi->getNumSysex() : -2;
    for (size_t c = 0; c < io.getNumChannels() && c < lines.size(); ++c) {
      float* p = io.getChannelPointer(c);
      for (size_t i = 0; i < io.getNumSamples(); ++i) {
        if (lines[c].empty()) continue;
        lines[c].push_back(p[i]);
        p[i] = lines[c].front();
        lines[c].pop_front();
      }
    }
  }
  uint32_t latencySamples() const override { return latency; }
  bool takeRestartRequest() override {
    const bool was = restart;
    restart = false;
    return was;
  }
  uint32_t tailSamples() const override { return 0; }
  bool acceptsMidi() const override { return true; }
  bool hasAudioInput() const override { return true; }
  int numParameters() const override { return 0; }
  const HostedParam& parameter(int) const override { return param; }
  double parameterValue(int) const override { return 0; }
  bool setParameterValue(int, double) override { return false; }
  std::string parameterText(int, double) const override { return {}; }
  bool saveState(std::vector<uint8_t>&) const override { return false; }
  bool loadState(const uint8_t*, size_t) override { return false; }
  const MidiBuffer& producedMidi() const override { return produced; }
  bool setBypassed(bool) override { return false; }
  bool isBypassed() const override { return false; }
  bool hasBypass() const override { return false; }
  int numPresets() const override { return 0; }
  std::string presetName(int) const override { return {}; }
  bool loadPreset(int) override { return false; }
  size_t drainParameterEdits(std::vector<ParamEdit>&) override { return 0; }
  bool hasEditor() const override { return false; }
  bool openEditor(void*) override { return false; }
  void closeEditor() override {}
  void idleEditor() override {}
  bool editorSize(uint32_t&, uint32_t&) const override { return false; }
  bool setEditorScale(double) override { return false; }
};

/** An impulse through the graph in blocks of `block`: where the output is
 *  non-zero, as (sample, value) pairs. */
static std::vector<std::pair<int, float>> impulse(PluginGraph& g, uint32_t block, int blocks) {
  std::vector<std::pair<int, float>> hits;
  std::vector<float> l(block), r(block);
  float* ch[2] = {l.data(), r.data()};
  for (int b = 0; b < blocks; ++b) {
    std::fill(l.begin(), l.end(), 0.0f);
    std::fill(r.begin(), r.end(), 0.0f);
    if (b == 0) l[0] = r[0] = 1.0f;
    AudioBlock<float> io(ch, 2, block);
    g.process(io);
    for (uint32_t i = 0; i < block; ++i)
      if (l[i] != 0.0f) hits.push_back({(int) (b * block + i), l[i]});
  }
  return hits;
}

static void printHits(const char* label, const std::vector<std::pair<int, float>>& hits) {
  std::printf("  ---- %s:", label);
  for (const auto& h : hits) std::printf(" [%d]=%g", h.first, h.second);
  std::printf(" ----\n");
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("graph_unit_test\n");

  // ── A diamond: In -> A(10) -> Out, In -> B(0) -> Out ────────────────────
  PluginGraph g;
  auto* a = new DelayNode(10);
  auto* b = new DelayNode(0);
  const auto ida = g.addNode(std::unique_ptr<HostedPlugin>(a));
  const auto idb = g.addNode(std::unique_ptr<HostedPlugin>(b));
  g.connect(PluginGraph::kInput, ida);
  g.connect(PluginGraph::kInput, idb);
  g.connect(ida, PluginGraph::kOutput);
  g.connect(idb, PluginGraph::kOutput);

  {
    std::vector<float> l(64, 1.0f), r(64, 1.0f);
    float* ch[2] = {l.data(), r.data()};
    AudioBlock<float> io(ch, 2, 64);
    g.process(io);
    check(l[0] == 0.0f && l[63] == 0.0f,
          "a graph never prepared outputs silence, not its dry input");
  }

  check(g.needsPrepare(), "an unprepared graph says it needs prepare()");
  check(g.prepare(48000.0, 64, 2), "the diamond prepares");
  check(!g.needsPrepare(), "and then says it does not");
  auto hits = impulse(g, 64, 4);
  printHits("aligned", hits);
  check(g.latencySamples() == 10 && hits.size() == 1 && hits[0].first == 10 && hits[0].second == 2.0f,
        "both branches arrive together, ten samples late, summed once");

  // ── A's latency moves after prepare(), as an oversampling switch does ───
  a->latency = 30;
  a->prepare(48000.0, 64, 2); // the node itself now delays by 30
  check(g.needsPrepare(), "a node whose latency moved makes the graph ask for prepare()");
  check(g.prepare(48000.0, 64, 2), "which realigns it");
  hits = impulse(g, 64, 4);
  printHits("after 10 -> 30", hits);
  check(g.latencySamples() == 30 && hits.size() == 1 && hits[0].first == 30 && hits[0].second == 2.0f,
        "and the branches meet again, at thirty, with the new total reported");

  // ── A node asking to be restarted ───────────────────────────────────────
  b->restart = true;
  check(g.needsPrepare(), "a node that asks to be restarted makes the graph ask too");
  check(g.needsPrepare(), "and the request stays until prepare() runs");
  g.prepare(48000.0, 64, 2);
  check(!g.needsPrepare(), "prepare() answers it");

  // ── A block longer than prepare() allowed ───────────────────────────────
  g.reset();
  hits = impulse(g, 200, 2); // 200 > 64: run as 64 + 64 + 64 + 8
  printHits("200-frame blocks", hits);
  check(hits.size() == 1 && hits[0].first == 30 && hits[0].second == 2.0f,
        "a 200-frame block into a 64-frame graph is processed in slices, not sent out dry");

  // ── A wiring change unprepares: silence until prepared again ────────────
  auto* c = new DelayNode(0);
  const auto idc = g.addNode(std::unique_ptr<HostedPlugin>(c));
  g.connect(PluginGraph::kInput, idc);
  {
    std::vector<float> l(64, 1.0f), r(64, 1.0f);
    float* ch[2] = {l.data(), r.data()};
    AudioBlock<float> io(ch, 2, 64);
    g.process(io);
    check(l[0] == 0.0f && l[63] == 0.0f, "after a wiring change, silence rather than the dry input");
  }

  // ── SysEx on an explicit MIDI edge ──────────────────────────────────────
  check(g.connectMidi(PluginGraph::kInput, idc), "a MIDI edge from the input to a node");
  g.prepare(48000.0, 64, 2);
  {
    MidiBuffer midi;
    const uint8_t sysex[] = {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7};
    midi.addSysex(sysex, sizeof(sysex), 5);
    midi.addEvent(MidiMessage::noteOn(0, 60, 100), 3);
    std::vector<float> l(64), r(64);
    float* ch[2] = {l.data(), r.data()};
    AudioBlock<float> io(ch, 2, 64);
    g.process(io, &midi);
    check(c->sysexSeen == 1 && a->sysexSeen == 1,
          "the wired node gets the SysEx as the unwired ones do");
  }

  // ── A node that reports a latency no plugin has ─────────────────────────
  //
  // A latency is the plugin's word, a uint32 it may fill with anything: -1
  // for "not known yet", an uninitialised field. The graph sized every early
  // branch's delay line from it, so 0xFFFFFFFF in a diamond asked for a
  // 4-billion-sample line per channel -- 32 GB committed before prepare()
  // threw std::bad_alloc into the host -- and two nodes of 2^31 in a row
  // summed to a latency of 0. Each is held to PluginGraph::kMaxLatency, the
  // total too, and stays held: the same lie at the next needsPrepare() is
  // not news.
  {
    struct Liar : DelayNode {
      uint32_t reported;
      explicit Liar(uint32_t r) : DelayNode(0), reported(r) {}
      uint32_t latencySamples() const override { return reported; }
    };
    for (const uint32_t lie : {0xFFFFFFFFu, 0x80000000u}) {
      PluginGraph h;
      const auto il = h.addNode(std::unique_ptr<HostedPlugin>(new Liar(lie)));
      const auto ip = h.addNode(std::unique_ptr<HostedPlugin>(new DelayNode(0)));
      const auto iq = h.addNode(std::unique_ptr<HostedPlugin>(new Liar(lie)));
      h.connect(PluginGraph::kInput, il);
      h.connect(il, iq); // two in a row: their sum must not wrap either
      h.connect(PluginGraph::kInput, ip);
      h.connect(iq, PluginGraph::kOutput);
      h.connect(ip, PluginGraph::kOutput);
      bool threw = false, prepared = false;
      try {
        prepared = h.prepare(48000.0, 64, 2);
      } catch (const std::exception&) {
        threw = true;
      }
      char line[200];
      std::snprintf(line, sizeof(line), "two nodes reporting %u samples each prepare%s, and the "
                    "graph reports %u", lie, threw ? " -- NO, it threw" : "", h.latencySamples());
      check(!threw && prepared && h.latencySamples() == PluginGraph::kMaxLatency, line);
      check(!threw && !h.needsPrepare(), "...and is not asked to prepare again for the same lie");
    }
  }

  // ── The caller's notes at the edges of an over-long block ───────────────
  //
  // A MidiBuffer can hold an event at or past the end of its block, or
  // before its start, and every format here clamps such an event into the
  // block rather than lose it -- so a block that fits is heard whole. Run
  // in slices, the graph handed each slice the events inside it and no
  // others: a note-off at frame 256 of a 256-frame block, past every slice,
  // was dropped, and the note it ended held on for good.
  {
    struct NoteCounter : DelayNode {
      int notesOn = 0, notesOff = 0, outside = 0;
      NoteCounter() : DelayNode(0) {}
      void process(AudioBlock<float>& io, const MidiBuffer* midi) override {
        if (midi)
          for (const auto& e : *midi) {
            if (e.getMessage().isNoteOn()) ++notesOn;
            if (e.getMessage().isNoteOff()) ++notesOff;
            if (e.samplePosition < 0 || e.samplePosition >= (int) io.getNumSamples()) ++outside;
          }
        DelayNode::process(io, midi);
      }
    };
    PluginGraph h;
    auto* counter = new NoteCounter();
    const auto id = h.addNode(std::unique_ptr<HostedPlugin>(counter));
    h.connect(PluginGraph::kInput, id);
    h.connect(id, PluginGraph::kOutput);
    h.prepare(48000.0, 64, 2);
    MidiBuffer midi;
    midi.addEvent(MidiMessage::noteOn(0, 60, 100), -3); // before the block
    midi.addEvent(MidiMessage::noteOn(0, 64, 100), 100);
    midi.addEvent(MidiMessage::noteOff(0, 64), 256); // at its end
    midi.addEvent(MidiMessage::noteOff(0, 60), 300); // past it
    std::vector<float> l(256), r(256);
    float* ch[2] = {l.data(), r.data()};
    AudioBlock<float> io(ch, 2, 256); // four slices of 64
    h.process(io, &midi);
    char line[200];
    std::snprintf(line, sizeof(line), "a 256-frame block in slices hands on every note, those at "
                  "its edges included (%d on, %d off, of 2 and 2; %d outside their slice)",
                  counter->notesOn, counter->notesOff, counter->outside);
    check(counter->notesOn == 2 && counter->notesOff == 2 && counter->outside == 0, line);
  }

  // ── What the graph itself holds per node ────────────────────────────────
  //
  // A rack of 50 effects, input to each and each to the output, prepared for
  // 512 frames of stereo: the graph's own bytes, the nodes made beforehand
  // (their prepare's few hundred bytes are counted). Each node's 4 KB buffer
  // is what it needs. It held 28 KB a node: an 18 KB MidiBuffer in every
  // one, for MIDI edges none of them had, and the vector's spare room
  // multiplied it. Now a node with a MIDI edge has one, and the SysEx check
  // above runs through it.
  {
    constexpr int kNodes = 50;
    std::vector<std::unique_ptr<HostedPlugin>> made;
    for (int i = 0; i < kNodes; ++i) made.push_back(std::unique_ptr<HostedPlugin>(new DelayNode(0)));
    const long long before = g_liveBytes.load();
    long long held = 0;
    {
      PluginGraph rack;
      for (auto& p : made) {
        const auto id = rack.addNode(std::move(p));
        rack.connect(PluginGraph::kInput, id);
        rack.connect(id, PluginGraph::kOutput);
      }
      rack.prepare(48000.0, 512, 2);
      held = g_liveBytes.load() - before;
    }
    char line[200];
    std::snprintf(line, sizeof(line), "the graph holds %lld bytes a node for a rack of %d at 512 frames "
                  "(at most 8192)", held / kNodes, kNodes);
    check(held > 0 && held / kNodes <= 8192, line);
  }

  std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
