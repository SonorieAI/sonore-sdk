// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: a graph of hosted plugins.
//
// host.h loads one plugin. This wires several together: a chain, a parallel
// split, a diamond, whatever an application's routing needs: a processor
// graph.
//
// The part that makes it more than a for-loop is DELAY COMPENSATION. A plugin
// that oversamples or looks ahead answers late and says so. Put one on a
// parallel branch and the two branches no longer line up, and summing them is
// a comb filter: the exact failure this SDK has been measuring in individual
// plugins for weeks, one level up. So the graph works out how late every node
// is, and delays the branches that are early until they agree.
//
// WHAT IS HERE:
//
//   Arbitrary routing between nodes, cycles refused, and TWO kinds of edge.
//   Audio edges carry a stream. MIDI edges carry notes, which is what lets an
//   arpeggiator drive a synth: the canonical reason to want a graph at all,
//   and something a single edge type cannot express, because the arpeggiator
//   produces no audio and the synth consumes none.
//
//   A node with no MIDI edge into it receives whatever MIDI the CALLER passed
//   to process(), which keeps the simple case simple: a rack of effects fed by
//   a keyboard needs no MIDI wiring at all.
//
// WHAT IS NOT:
//
//   Per-edge channel mapping. An edge carries the whole stream; a graph that
//   needs channel 3 of one node on channel 1 of another wants a matrix, and
//   pretending an edge can do it would be a silent truncation.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

#include "audio.h"
#include "host.h"

namespace sonore {
namespace host {

/**
 * Plugins as nodes, audio as edges.
 *
 * Two endpoints exist without being added: kInput is where the caller's audio
 * enters and kOutput is where it leaves. A graph with no path between them is
 * legal and silent, which is what an empty rack should be.
 */
class PluginGraph {
public:
  using NodeId = int;
  static constexpr NodeId kInput = -1;
  static constexpr NodeId kOutput = -2;
  static constexpr NodeId kInvalid = -3;

  PluginGraph() = default;
  PluginGraph(const PluginGraph&) = delete;
  PluginGraph& operator=(const PluginGraph&) = delete;

  /** Take ownership of a plugin and give it a place in the graph. The id is
   *  stable: removing is not supported, because a graph that renumbers under
   *  an application's feet is a graph whose stored routing goes stale. */
  NodeId addNode(std::unique_ptr<HostedPlugin> plugin) {
    if (!plugin) return kInvalid;
    // Built in place rather than braced: Node has grown fields since this was
    // written, and an aggregate initialiser is a list that silently means
    // something different every time one is added.
    nodes_.emplace_back();
    nodes_.back().plugin = std::move(plugin);
    prepared_ = false;
    return (NodeId) (nodes_.size() - 1);
  }

  size_t size() const { return nodes_.size(); }
  HostedPlugin* node(NodeId id) {
    return valid(id) ? nodes_[(size_t) id].plugin.get() : nullptr;
  }

  /** Route one node's output into another's input.
   *
   *  Refused if it would close a cycle. A graph that ate its own tail would
   *  either hang or, worse, produce whatever the previous block left in the
   *  buffers -- feedback nobody asked for and nobody can find. */
  /** Route one node's emitted NOTES into another's input.
   *
   *  Separate from audio because the two do not travel together: an
   *  arpeggiator produces notes and no audio, a synth consumes notes and
   *  produces audio, and a graph with one edge type can express neither
   *  connection honestly.
   *
   *  Ordering follows the AUDIO topology. A MIDI edge that ran against it
   *  would need the graph sorted twice and reconciled, and every arrangement
   *  worth building has the note source upstream anyway -- so a MIDI edge from
   *  a node that runs later is refused rather than silently delivering last
   *  block's notes. */
  bool connectMidi(NodeId from, NodeId to) {
    if (!valid(from) && from != kInput) return false;
    if (!valid(to)) return false;
    if (from == to) return false;
    // Same rule as an audio edge: a loop would either hang the sort or deliver
    // last block's notes forever.
    if (wouldCycle(from, to)) return false;
    std::vector<NodeId>& sources = nodes_[(size_t) to].midiSources;
    for (NodeId existing : sources)
      if (existing == from) return true;
    sources.push_back(from);
    prepared_ = false;
    return true;
  }

  bool connect(NodeId from, NodeId to) {
    if (!endpointOrNode(from) || !endpointOrNode(to)) return false;
    if (from == to) return false;
    if (to == kInput || from == kOutput) return false; // the endpoints have a direction
    if (wouldCycle(from, to)) return false;

    std::vector<NodeId>& sources = (to == kOutput) ? outputSources_ : nodes_[(size_t) to].sources;
    for (NodeId existing : sources)
      if (existing == from) return true; // already there; connecting twice is not an error
    sources.push_back(from);
    prepared_ = false;
    return true;
  }

  /** Total delay from the graph's input to its output, which the APPLICATION
   *  must report onwards if it is itself a plugin. */
  uint32_t latencySamples() const { return outputLatency_; }

  bool prepare(double sampleRate, uint32_t maxBlockSize, uint32_t numChannels = 2) {
    maxBlock_ = maxBlockSize > 0 ? maxBlockSize : 1;
    channels_ = numChannels > 0 ? (numChannels > 8 ? 8 : numChannels) : 1;

    // A cycle -- through audio edges, MIDI edges, or a mixture -- leaves nodes
    // the sort can never reach, and it says so by producing fewer than there
    // are.
    if (!topologicalOrder()) return false;

    for (Node& n : nodes_)
      if (!n.plugin->prepare(sampleRate, maxBlock_, channels_)) return false;

    computeLatencies();
    allocate();
    prepared_ = true;
    return true;
  }

  void release() {
    for (Node& n : nodes_) n.plugin->release();
    prepared_ = false;
  }

  void reset() {
    for (Node& n : nodes_) n.plugin->reset();
    for (auto& line : delays_)
      for (auto& channel : line.buffer) std::fill(channel.begin(), channel.end(), 0.0f);
    for (auto& line : delays_) line.write = 0;
  }

  /**
   * Run one block through the whole graph.
   *
   * Allocates nothing: every buffer was sized in prepare(). `io` is read and
   * written, so a caller can put the graph anywhere it would put one plugin.
   */
  void process(AudioBlock<float>& io, const MidiBuffer* midi = nullptr) {
    if (!prepared_) return;
    const uint32_t frames = (uint32_t) io.getNumSamples();
    if (frames == 0 || frames > maxBlock_) return;

    // The graph's input, copied once so a node that rewrites its buffer in
    // place cannot corrupt what a sibling branch has yet to read.
    const uint32_t ioChannels = (uint32_t) io.getNumChannels();
    for (uint32_t c = 0; c < channels_; ++c) {
      float* dst = channelOf(inputBuffer_, c);
      if (c < ioChannels) std::memcpy(dst, io.getChannelPointer(c), frames * sizeof(float));
      else std::memset(dst, 0, frames * sizeof(float));
    }

    for (NodeId id : order_) {
      Node& n = nodes_[(size_t) id];
      float* scratch[8];
      gather(id, n.sources, scratch, frames);
      for (uint32_t c = 0; c < channels_; ++c)
        std::memcpy(channelOf(n.buffer, c), scratch[c], frames * sizeof(float));

      // Notes: from upstream nodes where they were wired, and from the caller
      // where they were not. A node wired to a note source must NOT also hear
      // the caller's keyboard, or every arpeggiated pattern plays alongside
      // the raw notes that produced it.
      const MidiBuffer* notes = midi;
      if (!n.midiSources.empty()) {
        n.midiIn.clear();
        for (NodeId src : n.midiSources) {
          const MidiBuffer* from =
              (src == kInput) ? midi : (valid(src) ? &nodes_[(size_t) src].plugin->producedMidi()
                                                   : nullptr);
          if (!from) continue;
          for (const auto& e : *from) n.midiIn.addEvent(e.getMessage(), e.samplePosition);
        }
        notes = &n.midiIn;
      }

      float* pointers[8];
      for (uint32_t c = 0; c < channels_; ++c) pointers[c] = channelOf(n.buffer, c);
      AudioBlock<float> block(pointers, channels_, frames);
      n.plugin->process(block, notes);
    }

    float* summed[8];
    gather(kOutput, outputSources_, summed, frames);
    for (uint32_t c = 0; c < ioChannels; ++c) {
      float* dst = io.getChannelPointer(c);
      if (c < channels_) std::memcpy(dst, summed[c], frames * sizeof(float));
      else std::memset(dst, 0, frames * sizeof(float));
    }
  }

private:
  struct Node {
    std::unique_ptr<HostedPlugin> plugin;
    std::vector<NodeId> sources;
    /** Nodes whose emitted notes feed this one. Empty means "whatever the
     *  caller passed to process()", which is what a plain effect rack wants. */
    std::vector<NodeId> midiSources;
    /** This block's notes, gathered from midiSources. A member rather than a
     *  local so process() allocates nothing. */
    MidiBuffer midiIn;
    uint32_t latencyIn = 0;  // how late this node's INPUT is, once aligned
    uint32_t latencyOut = 0; // …and its output, after the plugin's own delay
    std::vector<float> buffer;
  };

  /** One edge's alignment delay. Sized at prepare, never reallocated. */
  struct DelayLine {
    NodeId from = kInvalid;
    NodeId to = kInvalid;
    uint32_t samples = 0;
    uint32_t write = 0;
    std::vector<std::vector<float>> buffer;
  };

  bool valid(NodeId id) const { return id >= 0 && (size_t) id < nodes_.size(); }
  bool endpointOrNode(NodeId id) const { return id == kInput || id == kOutput || valid(id); }

  float* channelOf(std::vector<float>& storage, uint32_t c) {
    return storage.data() + (size_t) c * maxBlock_;
  }

  /** Would connecting `from` to `to` close a loop? Walk backwards from `from`
   *  and see whether `to` is already upstream of it. */
  bool wouldCycle(NodeId from, NodeId to) const {
    if (from == kInput || to == kOutput) return false;
    std::vector<NodeId> stack{from};
    std::vector<char> seen(nodes_.size(), 0);
    while (!stack.empty()) {
      const NodeId id = stack.back();
      stack.pop_back();
      if (id == to) return true;
      if (!valid(id) || seen[(size_t) id]) continue;
      seen[(size_t) id] = 1;
      for (NodeId s : nodes_[(size_t) id].sources) stack.push_back(s);
      for (NodeId s : nodes_[(size_t) id].midiSources) stack.push_back(s);
    }
    return false;
  }

  /** Kahn's algorithm. Producing fewer nodes than exist means a cycle, which
   *  connect() should have refused -- checked anyway, because a graph built
   *  by a caller that reached in some other way must still not hang. */
  /** Every incoming edge of a node, audio and MIDI alike. */
  static bool dependsOn(const Node& n, NodeId source) {
    for (NodeId s : n.sources)
      if (s == source) return true;
    for (NodeId s : n.midiSources)
      if (s == source) return true;
    return false;
  }

  bool topologicalOrder() {
    order_.clear();
    // MIDI edges count as dependencies, exactly like audio ones.
    //
    // The first version sorted on audio alone and then CHECKED the MIDI edges
    // against the result, which rejected the most obvious graph there is: an
    // arpeggiator feeding a synth has no audio edge between them, so nothing
    // ordered the two and the check fired on a perfectly good arrangement. An
    // edge is an edge; where the notes go is as much a dependency as where the
    // samples go.
    std::vector<int> remaining(nodes_.size(), 0);
    for (size_t i = 0; i < nodes_.size(); ++i) {
      for (NodeId s : nodes_[i].sources)
        if (valid(s)) ++remaining[i];
      for (NodeId s : nodes_[i].midiSources)
        if (valid(s)) ++remaining[i];
    }

    std::vector<NodeId> ready;
    for (size_t i = 0; i < nodes_.size(); ++i)
      if (remaining[i] == 0) ready.push_back((NodeId) i);

    while (!ready.empty()) {
      const NodeId id = ready.back();
      ready.pop_back();
      order_.push_back(id);
      for (size_t i = 0; i < nodes_.size(); ++i) {
        for (NodeId s : nodes_[i].sources)
          if (s == id && --remaining[i] == 0) ready.push_back((NodeId) i);
        for (NodeId s : nodes_[i].midiSources)
          if (s == id && --remaining[i] == 0) ready.push_back((NodeId) i);
      }
    }
    return order_.size() == nodes_.size();
  }

  /** How late everything is.
   *
   *  A node's input is as late as its LATEST source, because that is when the
   *  last of its inputs can arrive; its output is that plus its own delay.
   *  Branches that arrive early are held back to match, which is the whole
   *  point -- summing an early branch with a late one is a comb filter. */
  void computeLatencies() {
    for (NodeId id : order_) {
      Node& n = nodes_[(size_t) id];
      uint32_t latest = 0;
      for (NodeId s : n.sources) latest = (std::max)(latest, latencyOf(s));
      n.latencyIn = latest;
      n.latencyOut = latest + n.plugin->latencySamples();
    }
    outputLatency_ = 0;
    for (NodeId s : outputSources_) outputLatency_ = (std::max)(outputLatency_, latencyOf(s));
  }

  uint32_t latencyOf(NodeId id) const {
    if (id == kInput) return 0;
    return valid(id) ? nodes_[(size_t) id].latencyOut : 0;
  }

  void allocate() {
    inputBuffer_.assign((size_t) channels_ * maxBlock_, 0.0f);
    mixBuffer_.assign((size_t) channels_ * maxBlock_, 0.0f);
    for (Node& n : nodes_) n.buffer.assign((size_t) channels_ * maxBlock_, 0.0f);

    delays_.clear();
    auto addEdges = [&](NodeId dest, const std::vector<NodeId>& sources, uint32_t target) {
      for (NodeId s : sources) {
        const uint32_t have = latencyOf(s);
        if (have >= target) continue; // already the latest; nothing to hold back
        DelayLine line;
        line.from = s;
        line.to = dest;
        line.samples = target - have;
        line.buffer.assign(channels_, std::vector<float>(line.samples, 0.0f));
        delays_.push_back(std::move(line));
        edgeIndex_[key(s, dest)] = delays_.size() - 1;
      }
    };
    edgeIndex_.clear();
    for (NodeId id : order_)
      addEdges(id, nodes_[(size_t) id].sources, nodes_[(size_t) id].latencyIn);
    addEdges(kOutput, outputSources_, outputLatency_);
  }

  /** Keyed by the EDGE, not by (source, delay).
   *
   *  Two destinations can want the same source delayed by the same amount --
   *  a diamond where both branches are equally late. Sharing one line between
   *  them would advance its write cursor twice per block, so the second reader
   *  would get samples from half a block ago and the delay would be wrong for
   *  both. */
  static uint64_t key(NodeId from, NodeId to) {
    return ((uint64_t) (uint32_t) from << 32) | (uint32_t) to;
  }

  /** Sum every source into the mix buffer, delaying the early ones. */
  void gather(NodeId dest, const std::vector<NodeId>& sources, float** out, uint32_t frames) {
    for (uint32_t c = 0; c < channels_; ++c) {
      out[c] = channelOf(mixBuffer_, c);
      std::memset(out[c], 0, frames * sizeof(float));
    }
    for (NodeId s : sources) {
      std::vector<float>* storage =
          (s == kInput) ? &inputBuffer_ : (valid(s) ? &nodes_[(size_t) s].buffer : nullptr);
      if (!storage) continue;

      auto found = edgeIndex_.find(key(s, dest));
      DelayLine* line = found == edgeIndex_.end() ? nullptr : &delays_[found->second];
      for (uint32_t c = 0; c < channels_; ++c) {
        const float* src = storage->data() + (size_t) c * maxBlock_;
        float* dst = out[c];
        if (!line || line->samples == 0) {
          for (uint32_t i = 0; i < frames; ++i) dst[i] += src[i];
        } else {
          std::vector<float>& ring = line->buffer[c];
          uint32_t w = line->write;
          for (uint32_t i = 0; i < frames; ++i) {
            const float delayed = ring[w];
            ring[w] = src[i];
            if (++w >= line->samples) w = 0;
            dst[i] += delayed;
          }
          if (c + 1 == channels_) line->write = w; // advance once per block
        }
      }
    }
  }

  std::vector<Node> nodes_;
  std::vector<NodeId> outputSources_;
  std::vector<NodeId> order_;
  std::vector<DelayLine> delays_;
  std::map<uint64_t, size_t> edgeIndex_;

  std::vector<float> inputBuffer_, mixBuffer_;
  uint32_t maxBlock_ = 0, channels_ = 2, outputLatency_ = 0;
  bool prepared_ = false;
};

} // namespace host
} // namespace sonore
