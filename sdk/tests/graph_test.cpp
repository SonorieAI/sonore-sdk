// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the plugin graph, and the delay compensation that justifies it.
//
// Chaining plugins is a for-loop. What makes a graph worth having is what
// happens when the branches are not the same length: a plugin that oversamples
// or looks ahead answers late, and summing its output against an untouched
// parallel branch is a comb filter. This whole SDK has spent weeks measuring
// that failure inside individual plugins; the graph is where it happens
// between them.
//
// So the test that matters is not "does audio come out". It is: put a latent
// plugin on one branch of a split, sum both, and require the result to be the
// same as if the delay were not there.

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include <algorithm>

#include <sonore/graph.h>

static int g_checks = 0;
static int g_failures = 0;

static void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

// The same allocation counter the RT audit and the host test use, for the same
// reason: process() promises not to allocate, and a promise about the audio
// thread is worth what it is measured to be worth.
static std::atomic<bool> g_armed{false};
static std::atomic<long> g_allocs{0};

void* operator new(std::size_t n) {
  if (g_armed.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
  void* p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using sonore::host::PluginGraph;

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: graph_test <directory-of-clap-plugins>\n");
    return 2;
  }
  const std::string directory = argv[1];
  std::printf("── plugin graph ────────────────────────────────────────\n");

  const auto found = sonore::host::scanDirectory(directory);
  check(!found.empty(), "the scan finds plugins to build a graph from");
  if (found.empty()) return 1;

  // A latent effect and a transparent one. The reverb declares 512 samples of
  // look-ahead and trim declares none, which is exactly the asymmetry a graph
  // has to reconcile.
  const sonore::host::PluginDescription* latent = nullptr;
  const sonore::host::PluginDescription* plain = nullptr;
  for (const auto& d : found) {
    if (d.isInstrument) continue;
    if (!latent && d.name.find("Reverb") != std::string::npos) latent = &d;
    if (!plain && d.name.find("Trim") != std::string::npos) plain = &d;
  }
  check(latent != nullptr && plain != nullptr,
        "…including one that delays and one that does not");
  if (!latent || !plain) return 1;

  const uint32_t block = 256;
  std::vector<float> l(block), r(block);
  float* channels[2] = {l.data(), r.data()};

  auto fill = [&](uint32_t seed) {
    uint32_t lcg = seed;
    for (uint32_t i = 0; i < block; ++i) {
      lcg = lcg * 1664525u + 1013904223u;
      l[i] = r[i] = (float) ((int32_t) (lcg >> 8) % 20001 - 10000) / 40000.0f;
    }
  };

  // ── A cycle is refused ────────────────────────────────────────────────────
  {
    PluginGraph graph;
    const auto a = graph.addNode(sonore::host::loadPlugin(*plain));
    const auto b = graph.addNode(sonore::host::loadPlugin(*plain));
    check(a >= 0 && b >= 0, "two nodes go into a graph");
    check(graph.connect(a, b), "…and one connects to the other");
    check(!graph.connect(b, a),
          "…but closing the loop is refused, not left to hang or feed back");
    check(!graph.connect(a, a), "…and neither is a node feeding itself");
    check(graph.connect(a, b), "…while connecting the same edge twice is harmless");
  }

  // ── A chain sums its latencies ────────────────────────────────────────────
  {
    PluginGraph graph;
    const auto one = graph.addNode(sonore::host::loadPlugin(*latent));
    const auto two = graph.addNode(sonore::host::loadPlugin(*latent));
    graph.connect(PluginGraph::kInput, one);
    graph.connect(one, two);
    graph.connect(two, PluginGraph::kOutput);
    check(graph.prepare(48000.0, block, 2), "a chain of two latent plugins prepares");
    if (!graph.node(one) || !graph.node(two)) {
      // A plugin that failed to load is a FAILED check, not a null pointer
      // dereferenced into a signal: the first sanitized run of this test found
      // no plugins in the folder it was pointed at and died here instead of
      // saying so.
      check(false, "both latent plugins loaded into the graph");
      return 1;
    }

    const uint32_t single = graph.node(one)->latencySamples();
    std::printf("  ---- chain latency: %u, one plugin: %u ----\n", graph.latencySamples(),
                single);
    check(graph.latencySamples() == single * 2,
          "…and the graph's latency is the sum, which is what the host must be told");
  }

  // ── The one that matters: a parallel split stays aligned ──────────────────
  //
  // Two branches from the same input, summed at the output. One branch has a
  // plugin that answers 512 samples late. Without compensation the sum is the
  // signal added to a delayed copy of itself, which is a comb filter with a
  // notch every 94 Hz. With it, the early branch is held back and the two
  // agree.
  {
    // The reference: the SAME graph with the latent branch removed, so the
    // comparison is against what the aligned sum should be rather than
    // against a number worked out by hand.
    PluginGraph split;
    const auto through = split.addNode(sonore::host::loadPlugin(*plain));
    const auto delayed = split.addNode(sonore::host::loadPlugin(*latent));
    split.connect(PluginGraph::kInput, through);
    split.connect(PluginGraph::kInput, delayed);
    split.connect(through, PluginGraph::kOutput);
    split.connect(delayed, PluginGraph::kOutput);
    check(split.prepare(48000.0, block, 2), "a parallel split prepares");

    const uint32_t latency = split.latencySamples();
    check(latency == split.node(delayed)->latencySamples(),
          "…and reports the LATEST branch, not the sum of both");

    // Turn the latent plugin's WET signal off, which turns this into a null
    // test instead of an approximate one.
    //
    // At its default mix the reverb's own output is as loud as the dry path,
    // and the first version of this check drowned in it: compensated 0.2223
    // against uncompensated 0.2494, two numbers that prove nothing because
    // one is "the reverb is noisy" and the other is "nothing has arrived
    // yet". With the wet at zero the reverb is a pure 512-sample delay -- and
    // since it compensates its own dry path, both branches then carry the
    // SAME sample and the sum is exactly twice the input.
    //
    // Found by name, not by index: which parameter is the mix is the
    // plugin's business, and a test that assumed position would silently
    // measure the wrong control the day one is inserted before it.
    for (int i = 0; i < split.node(delayed)->numParameters(); ++i) {
      if (split.node(delayed)->parameter(i).name != std::string("Mix")) continue;
      split.node(delayed)->setParameterValue(i, split.node(delayed)->parameter(i).minValue);
    }

    // Drive both graphs with identical audio and compare the branch that
    // should be untouched. The trim branch alone, delayed by the graph, has to
    // arrive at the same time as the reverb branch -- so subtracting a
    // reference chain that contains only the trim, given the same alignment,
    // must leave only the reverb's contribution and no comb.
    PluginGraph reference;
    const auto refThrough = reference.addNode(sonore::host::loadPlugin(*plain));
    reference.connect(PluginGraph::kInput, refThrough);
    reference.connect(refThrough, PluginGraph::kOutput);
    check(reference.prepare(48000.0, block, 2), "the reference chain prepares");
    check(reference.latencySamples() == 0, "…and has no latency of its own");

    // Feed silence into both for long enough that the delay lines are primed,
    // then a burst, and compare where the burst comes out.
    std::vector<float> splitOut, refOut;
    for (int b = 0; b < 32; ++b) {
      fill(b == 8 ? 12345u : 0u);
      if (b != 8) {
        std::fill(l.begin(), l.end(), 0.0f);
        std::fill(r.begin(), r.end(), 0.0f);
      }
      std::vector<float> keepL = l, keepR = r;

      sonore::AudioBlock<float> a(channels, 2, block);
      split.process(a, nullptr);
      for (uint32_t i = 0; i < block; ++i) splitOut.push_back(l[i]);
      (void) keepR;

      l = keepL;
      r = keepR;
      sonore::AudioBlock<float> c(channels, 2, block);
      reference.process(c, nullptr);
      for (uint32_t i = 0; i < block; ++i) refOut.push_back(l[i]);
    }

    // The trim branch inside the split must appear `latency` samples late,
    // exactly where the reference's own output sits once shifted by the same
    // amount. Anything else means the compensation is wrong.
    // Two measurements, not one. A threshold on its own could be met by
    // accident; what proves the compensation is that reading the SAME samples
    // at the uncompensated position is far worse. If the graph were not
    // holding the early branch back, the second number would be the small one.
    // Both branches now carry the same sample: the trim held back by the
    // graph, and the reverb's dry path delayed by its own reported latency.
    // So the sum is exactly TWICE the reference, and any deviation is the
    // compensation being wrong.
    double aligned = 0.0, unaligned = 0.0;
    size_t compared = 0;
    for (size_t i = 0; i + latency < refOut.size() && i < splitOut.size(); ++i) {
      if (refOut[i] == 0.0f) continue;
      const double want = 2.0 * (double) refOut[i];
      aligned = (std::max)(aligned, std::fabs((double) splitOut[i + latency] - want));
      unaligned = (std::max)(unaligned, std::fabs((double) splitOut[i] - want));
      ++compared;
    }
    std::printf("  ---- branch alignment over %zu samples: compensated %.4f, "
                "uncompensated would be %.4f ----\n",
                compared, aligned, unaligned);
    check(compared > 100, "there is a burst to compare at all");
    // A null, not a threshold: the two branches carry the same sample, so the
    // sum is twice the input to within float rounding.
    check(aligned < 1e-4, "the early branch is held back until the two branches NULL");
    check(unaligned > 0.1,
          "…while reading the same samples uncompensated is nowhere near, which is the proof");
  }

  // ── An arpeggiator driving a synth ────────────────────────────────────────
  //
  // The canonical reason to want a graph. The arpeggiator produces no audio
  // and the synth consumes none, so the connection between them cannot be an
  // audio edge, and until this commit the host dropped a plugin's emitted
  // notes entirely, which meant hosting an arpeggiator without using it.
  //
  // The check is a comparison, not a threshold: the SAME graph without the
  // MIDI edge has to be silent. One held note reaching the synth directly
  // would also make sound, so "it made sound" on its own proves nothing.
  {
    const sonore::host::PluginDescription* arp = nullptr;
    const sonore::host::PluginDescription* synth = nullptr;
    for (const auto& d : found) {
      if (!arp && d.name.find("Arp") != std::string::npos) arp = &d;
      if (!synth && d.name.find("Synth") != std::string::npos) synth = &d;
    }
    check(arp != nullptr && synth != nullptr, "there is an arpeggiator and a synth to wire up");

    if (arp && synth) {
      // `pressKey` rather than "wire the graph": the graph is identical in
      // both runs and only the caller's input differs.
      auto energyOf = [&](bool pressKey) {
        PluginGraph graph;
        const auto a = graph.addNode(sonore::host::loadPlugin(*arp));
        const auto sy = graph.addNode(sonore::host::loadPlugin(*synth));
        graph.connect(PluginGraph::kInput, a);
        graph.connect(sy, PluginGraph::kOutput);
        // The arpeggiator hears the caller's keyboard; the synth hears the
        // arpeggiator and NOT the keyboard, or the held note would sound
        // alongside the pattern it produced.
        graph.connectMidi(PluginGraph::kInput, a);
        graph.connectMidi(a, sy);
        if (!graph.prepare(48000.0, block, 2)) return -1.0;

        sonore::MidiBuffer held;
        if (pressKey) held.addEvent(sonore::MidiMessage::noteOn(0, 60, 100), 0);

        double energy = 0.0;
        for (int b = 0; b < 200; ++b) {
          std::fill(l.begin(), l.end(), 0.0f);
          std::fill(r.begin(), r.end(), 0.0f);
          sonore::AudioBlock<float> io(channels, 2, block);
          graph.process(io, b == 0 ? &held : nullptr);
          for (uint32_t i = 0; i < block; ++i) energy += (double) l[i] * l[i];
        }
        return energy;
      };

      const double played = energyOf(true);
      const double idle = energyOf(false);
      std::printf("  ---- arp to synth: %.4g with a key held, %.4g with none ----\n",
                  played, idle);
      check(played > 0.0, "an arpeggiator's notes reach the synth and it SOUNDS");
      // The SAME graph with the SAME edges, differing only in whether the
      // caller pressed a key. That isolates the note path. The first version
      // compared a wired graph against an UNwired one and proved nothing:
      // 145.1 either way, because a synth with no MIDI edge simply hears the
      // keyboard itself, which is the correct default and a useless control.
      check(idle == 0.0, "…and with no key held it is silent, so the notes came from the arp");
    }
  }

  // ── A MIDI edge that runs backwards is refused ────────────────────────────
  //
  // Notes have to arrive in the block they were made. An edge from a node that
  // runs later would deliver last block's, which reads as an arpeggiator
  // subtly behind the beat and is very hard to see.
  {
    const sonore::host::PluginDescription* arp = nullptr;
    for (const auto& d : found)
      if (!arp && d.name.find("Arp") != std::string::npos) arp = &d;
    if (arp) {
      PluginGraph graph;
      const auto first = graph.addNode(sonore::host::loadPlugin(*arp));
      const auto second = graph.addNode(sonore::host::loadPlugin(*arp));
      graph.connect(PluginGraph::kInput, first);
      graph.connect(first, second);
      graph.connect(second, PluginGraph::kOutput);
      check(graph.connectMidi(first, second), "a MIDI edge that follows the audio order is fine");
      check(graph.prepare(48000.0, block, 2), "…and prepares");

      PluginGraph backwards;
      const auto a = backwards.addNode(sonore::host::loadPlugin(*arp));
      const auto b = backwards.addNode(sonore::host::loadPlugin(*arp));
      backwards.connect(PluginGraph::kInput, a);
      backwards.connect(a, b);
      backwards.connect(b, PluginGraph::kOutput);
      // Refused at connect() rather than at prepare(), which is the better
      // place: the caller learns immediately which edge was the problem
      // instead of being told the whole graph is bad later. Once MIDI edges
      // became real dependencies, an edge running against the audio order is
      // simply a cycle, and the same check catches it.
      check(!backwards.connectMidi(b, a),
            "…while one that runs against it is refused, and refused where it is made");
      check(backwards.prepare(48000.0, block, 2),
            "…leaving a graph that is still perfectly good without it");
    }
  }

  // ── An empty graph is silent, not broken ──────────────────────────────────
  {
    PluginGraph empty;
    check(empty.prepare(48000.0, block, 2), "a graph with no nodes prepares");
    fill(7u);
    sonore::AudioBlock<float> io(channels, 2, block);
    empty.process(io, nullptr);
    bool silent = true;
    for (uint32_t i = 0; i < block; ++i)
      if (l[i] != 0.0f) silent = false;
    check(silent, "…and passes nothing, because nothing connects its input to its output");
  }

  // ── process() allocates nothing ───────────────────────────────────────────
  {
    PluginGraph graph;
    const auto a = graph.addNode(sonore::host::loadPlugin(*latent));
    const auto b = graph.addNode(sonore::host::loadPlugin(*plain));
    graph.connect(PluginGraph::kInput, a);
    graph.connect(PluginGraph::kInput, b);
    graph.connect(a, PluginGraph::kOutput);
    graph.connect(b, PluginGraph::kOutput);
    graph.prepare(48000.0, block, 2);

    g_armed.store(true);
    { std::vector<double> deliberate; deliberate.resize(2048); if (deliberate[0] != 0.0) return 9; }
    const bool counterWorks = g_allocs.load() > 0;
    g_armed.store(false);
    g_allocs.store(0);
    check(counterWorks, "the allocation counter can see an allocation (self-check)");

    fill(3u);
    sonore::AudioBlock<float> warm(channels, 2, block);
    graph.process(warm, nullptr);

    g_allocs.store(0);
    g_armed.store(true);
    for (int i = 0; i < 200; ++i) {
      sonore::AudioBlock<float> io(channels, 2, i % 3 == 0 ? block : 64);
      graph.process(io, nullptr);
    }
    g_armed.store(false);
    const long n = g_allocs.load();
    std::printf("  ---- allocations inside 200 graph blocks: %ld ----\n", n);
    check(n == 0, "the graph's process() allocates nothing, like the plugins in it");
  }

  std::printf("\n%d checks, %d failure(s)\n", g_checks, g_failures);
  if (g_failures == 0) std::printf("SONORE GRAPH PASSED\n");
  return g_failures == 0 ? 0 : 1;
}
