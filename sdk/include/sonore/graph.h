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
//   arpeggiator drive a synth, the canonical reason to want a graph at all,
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
  /** A node's place in the graph: 0, 1, 2... in the order addNode() made
   *  them, or one of the three values below. */
  using NodeId = int;
  static constexpr NodeId kInput = -1;   ///< The caller's audio, and its MIDI, coming in.
  static constexpr NodeId kOutput = -2;  ///< Where the graph's audio leaves; a source for nothing.
  static constexpr NodeId kInvalid = -3; ///< What addNode() returns for a null plugin.

  /** The most delay the graph compensates, in samples: for one node, and end
   *  to end. 2^22 is 87 s at 48 kHz and 22 s at 192 kHz, past any lookahead
   *  a plugin has. A latency is the plugin's word -- a uint32 it may fill
   *  with -1 for "not known yet" -- and each early branch's delay line was
   *  sized from it: 0xFFFFFFFF asked for 16 GB a channel, and two of 2^31 in
   *  a row summed to 0. Past this a node is taken at this, and the branches
   *  meet as late as this. */
  static constexpr uint32_t kMaxLatency = 1u << 22;

  /** Empty: no nodes, and no path from kInput to kOutput, so silent. */
  PluginGraph() = default;
  PluginGraph(const PluginGraph&) = delete;            ///< Not copyable: it owns its plugins.
  PluginGraph& operator=(const PluginGraph&) = delete; ///< Not copyable: it owns its plugins.

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

  /** How many nodes addNode() has made; the endpoints are not counted. */
  size_t size() const { return nodes_.size(); }
  /** The plugin at `id`, still owned by the graph, for its parameters, state
   *  and editor; null for an endpoint or an id out of range. */
  HostedPlugin* node(NodeId id) {
    return valid(id) ? nodes_[(size_t) id].plugin.get() : nullptr;
  }

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

  /** Route one node's output into another's input.
   *
   *  Refused if it would close a cycle. A graph that ate its own tail would
   *  either hang or, worse, produce whatever the previous block left in the
   *  buffers -- feedback nobody asked for and nobody can find. */
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
   *  must report onwards if it is itself a plugin. As of the last prepare(). */
  uint32_t latencySamples() const { return outputLatency_; }

  /**
   * [main-thread] Does the graph need prepare() again before it is right?
   *
   * True after a wiring change, when a node asked to be restarted, and when a
   * node's latency is no longer the one its branches were aligned for. The
   * latency was read in prepare() and nowhere else, so a plugin whose
   * oversampling switch moved it left a diamond misaligned -- a comb filter --
   * with latencySamples() still reporting the old total. Nothing here can
   * realign while audio runs (the delay lines are sized for the old answer),
   * so the owner is told, and prepares again with the audio stopped, as a
   * host restarts a plugin. Sticky until that prepare().
   */
  bool needsPrepare() {
    for (Node& n : nodes_) {
      if (n.plugin->takeRestartRequest()) restartPending_ = true;
      if (prepared_ && heldLatency(*n.plugin) != n.ownLatency) restartPending_ = true;
    }
    return !prepared_ || restartPending_;
  }

  /** [main-thread, audio stopped] Sort the nodes, prepare every plugin at
   *  this rate, block size and channel count (held to 1 to 8), work out the delay
   *  compensation and size every buffer. False, and the graph left
   *  unprepared so process() outputs silence, when the wiring has a cycle or
   *  a plugin refuses to prepare. */
  bool prepare(double sampleRate, uint32_t maxBlockSize, uint32_t numChannels = 2) {
    // Unprepared until every step below has succeeded. The block size and
    // channel count change first, but the buffers are only re-sized at the
    // end -- so a RE-prepare refused halfway (one plugin will not run at the
    // new size) used to leave the previous prepare's flag standing, and
    // process() then walked the old buffers at the new stride: 1024-frame
    // channels in a buffer allocated for two of 512, past the end even at the
    // old block size.
    prepared_ = false;
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
    restartPending_ = false;
    prepared_ = true;
    return true;
  }

  /** [main-thread, audio stopped] Release every plugin. The graph is
   *  unprepared afterwards, and outputs silence until prepare(). */
  void release() {
    for (Node& n : nodes_) n.plugin->release();
    prepared_ = false;
  }

  /** Reset every plugin and clear the delay lines, for a jump in the
   *  timeline. With audio stopped, or from the thread that calls process(),
   *  as HostedPlugin::reset() asks. */
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
   *
   * A graph that is not prepared -- never, or not since its wiring changed --
   * outputs SILENCE. It returned with `io` untouched, which sent the dry,
   * uncompensated input out as if it were the graph's answer. A block longer
   * than prepare() allowed is run in slices of at most that size, each with
   * the MIDI inside it, rather than refused the same way.
   */
  void process(AudioBlock<float>& io, const MidiBuffer* midi = nullptr) {
    const uint32_t frames = (uint32_t) io.getNumSamples();
    if (!prepared_) {
      for (size_t c = 0; c < io.getNumChannels(); ++c)
        std::memset(io.getChannelPointer(c), 0, frames * sizeof(float));
      return;
    }
    if (frames <= maxBlock_) {
      processSlice(io, midi);
      return;
    }
    constexpr size_t kMaxIo = 32;
    const size_t nch = io.getNumChannels();
    for (size_t c = kMaxIo; c < nch; ++c) // past what a slice can carry: silent
      std::memset(io.getChannelPointer(c), 0, frames * sizeof(float));
    float* pointers[kMaxIo];
    for (uint32_t start = 0; start < frames; start += maxBlock_) {
      const uint32_t n = (frames - start) < maxBlock_ ? (frames - start) : maxBlock_;
      for (size_t c = 0; c < nch && c < kMaxIo; ++c) pointers[c] = io.getChannelPointer(c) + start;
      AudioBlock<float> slice(pointers, nch < kMaxIo ? nch : kMaxIo, n);
      const MidiBuffer* notes = nullptr;
      if (midi) {
        // An event before the block or at or past its end is taken as on its
        // first or last frame, as every format here takes one in a block that
        // fits. Sliced by its own position, it fell in no slice and was lost:
        // a note-off at the block's end, and the note held on for good.
        const int last = (int) frames - 1;
        auto at = [last](int position) { return position < 0 ? 0 : (position > last ? last : position); };
        sliceMidi_.clear();
        for (const auto& e : *midi) {
          const int p = at(e.samplePosition);
          if (p >= (int) start && p < (int) (start + n))
            sliceMidi_.addEvent(e.getMessage(), p - (int) start);
        }
        for (const auto* s = midi->sysexBegin(); s != midi->sysexEnd(); ++s) {
          const int p = at(s->samplePosition);
          if (p >= (int) start && p < (int) (start + n))
            sliceMidi_.addSysex(midi->sysexData(*s), s->length, p - (int) start);
        }
        notes = &sliceMidi_;
      }
      processSlice(slice, notes);
    }
  }

private:
  void processSlice(AudioBlock<float>& io, const MidiBuffer* midi) {
    const uint32_t frames = (uint32_t) io.getNumSamples();
    if (frames == 0) return;

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
      float* pointers[8];
      for (uint32_t c = 0; c < channels_; ++c) pointers[c] = channelOf(n.buffer, c);
      // Straight into the node's own buffer, which no source of its own can
      // be: it was gathered into the mix buffer and copied across.
      gather(n.sources, n.sourceDelays, pointers, frames);

      // Notes: from upstream nodes where they were wired, and from the caller
      // where they were not. A node wired to a note source must NOT also hear
      // the caller's keyboard, or every arpeggiated pattern plays alongside
      // the raw notes that produced it.
      const MidiBuffer* notes = midi;
      if (!n.midiSources.empty()) {
        MidiBuffer& in = *n.midiIn; // made by allocate() for every node with a source
        in.clear();
        for (NodeId src : n.midiSources) {
          const MidiBuffer* from =
              (src == kInput) ? midi : (valid(src) ? &nodes_[(size_t) src].plugin->producedMidi()
                                                   : nullptr);
          if (!from) continue;
          for (const auto& e : *from) in.addEvent(e.getMessage(), e.samplePosition);
          // SysEx too: an explicitly wired node lost every SysEx message the
          // same source gave an unwired one.
          for (const auto* s = from->sysexBegin(); s != from->sysexEnd(); ++s)
            in.addSysex(from->sysexData(*s), s->length, s->samplePosition);
        }
        notes = &in;
      }

      AudioBlock<float> block(pointers, channels_, frames);
      n.plugin->process(block, notes);
    }

    float* summed[8];
    for (uint32_t c = 0; c < channels_; ++c) summed[c] = channelOf(mixBuffer_, c);
    gather(outputSources_, outputSourceDelays_, summed, frames);
    for (uint32_t c = 0; c < ioChannels; ++c) {
      float* dst = io.getChannelPointer(c);
      if (c < channels_) std::memcpy(dst, summed[c], frames * sizeof(float));
      else std::memset(dst, 0, frames * sizeof(float));
    }
  }

  /** One plugin in the graph and its edges: the plugin it owns, the nodes
   *  that feed it audio and notes, its delay-compensation figures in samples,
   *  and its own output buffer, sized at prepare() so process() allocates
   *  nothing. Private: callers reach a node through its NodeId. */
  struct Node {
    std::unique_ptr<HostedPlugin> plugin; ///< The plugin, owned here.
    std::vector<NodeId> sources;          ///< The nodes, or kInput, whose audio is summed into it.
    /** Each source's alignment delay, as an index into delays_, or -1 for
     *  none. Worked out in allocate(), beside the lines themselves: process()
     *  looked every edge up in edgeIndex_, a map, per source per block. */
    std::vector<int> sourceDelays;
    /** Nodes whose emitted notes feed this one. Empty means "whatever the
     *  caller passed to process()", which is what a plain effect rack wants. */
    std::vector<NodeId> midiSources;
    /** This block's notes, gathered from midiSources: made by allocate() for
     *  a node that has any, so process() allocates nothing. Held by pointer
     *  because most nodes have none and a MidiBuffer is 18 KB: in the node
     *  itself, a rack of 50 effects held 1.2 MB of them, the vector's spare
     *  room included, and copied every one each time the vector grew. */
    std::unique_ptr<MidiBuffer> midiIn;
    uint32_t latencyIn = 0;  ///< How late this node's INPUT is, once aligned.
    uint32_t latencyOut = 0; ///< ...and its output, after the plugin's own delay.
    uint32_t ownLatency = 0; ///< The plugin's own, as aligned for at prepare().
    std::vector<float> buffer; ///< Its channels, maxBlock samples each, end to end.
  };

  /** One edge's alignment delay. Sized at prepare, never reallocated. */
  struct DelayLine {
    NodeId from = kInvalid; ///< The edge's source.
    NodeId to = kInvalid;   ///< Its destination, or kOutput.
    uint32_t samples = 0;   ///< The delay, in samples.
    uint32_t write = 0;     ///< Where the next sample goes in each channel's ring.
    std::vector<std::vector<float>> buffer; ///< One ring per channel.
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

  /** Every incoming edge of a node, audio and MIDI alike. */
  static bool dependsOn(const Node& n, NodeId source) {
    for (NodeId s : n.sources)
      if (s == source) return true;
    for (NodeId s : n.midiSources)
      if (s == source) return true;
    return false;
  }

  /** Kahn's algorithm. Producing fewer nodes than exist means a cycle, which
   *  connect() should have refused -- checked anyway, because a graph built
   *  by a caller that reached in some other way must still not hang. */
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
      n.ownLatency = heldLatency(*n.plugin);
      // Summed wide: a chain of nodes each at the limit must not wrap.
      n.latencyOut = (uint32_t) (std::min)((uint64_t) latest + n.ownLatency, (uint64_t) kMaxLatency);
    }
    outputLatency_ = 0;
    for (NodeId s : outputSources_) outputLatency_ = (std::max)(outputLatency_, latencyOf(s));
  }

  uint32_t latencyOf(NodeId id) const {
    if (id == kInput) return 0;
    return valid(id) ? nodes_[(size_t) id].latencyOut : 0;
  }

  /** A plugin's latency as the graph aligns for it: its own word, held to
   *  kMaxLatency. needsPrepare() compares the same, so a plugin repeating
   *  an impossible answer is not taken for one whose latency moved. */
  static uint32_t heldLatency(const HostedPlugin& plugin) {
    const uint32_t reported = plugin.latencySamples();
    return reported < kMaxLatency ? reported : kMaxLatency;
  }

  void allocate() {
    inputBuffer_.assign((size_t) channels_ * maxBlock_, 0.0f);
    mixBuffer_.assign((size_t) channels_ * maxBlock_, 0.0f);
    for (Node& n : nodes_) {
      n.buffer.assign((size_t) channels_ * maxBlock_, 0.0f);
      // An edge is never taken away, so a buffer made once is kept.
      if (!n.midiSources.empty() && !n.midiIn) n.midiIn.reset(new MidiBuffer());
    }

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

    auto resolve = [&](NodeId dest, const std::vector<NodeId>& sources, std::vector<int>& lines) {
      lines.assign(sources.size(), -1);
      for (size_t i = 0; i < sources.size(); ++i) {
        const auto found = edgeIndex_.find(key(sources[i], dest));
        if (found != edgeIndex_.end()) lines[i] = (int) found->second;
      }
    };
    for (NodeId id : order_) resolve(id, nodes_[(size_t) id].sources, nodes_[(size_t) id].sourceDelays);
    resolve(kOutput, outputSources_, outputSourceDelays_);
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

  /** Sum every source into `out`, delaying the early ones. `delays` is the
   *  sources' lines, as allocate() resolved them.
   *
   *  The first source is WRITTEN, as 0 + x, and the rest added: the same sums
   *  in the same order as zeroing the buffer and adding every source, which
   *  is what this did -- 0 + x is x but for turning -0 into +0, and that is
   *  kept. Zeroing first was a pass over every channel of every node per
   *  block for nothing, and the copy from the mix buffer into the node's own
   *  another: three passes where one does, 200 ns of each node of a chain at
   *  512 frames. A delay line runs in straight stretches up to its wrap
   *  instead of testing for it every sample. */
  void gather(const std::vector<NodeId>& sources, const std::vector<int>& delays, float** out,
              uint32_t frames) {
    bool written = false;
    for (size_t k = 0; k < sources.size(); ++k) {
      const NodeId s = sources[k];
      std::vector<float>* storage =
          (s == kInput) ? &inputBuffer_ : (valid(s) ? &nodes_[(size_t) s].buffer : nullptr);
      if (!storage) continue;

      DelayLine* line = k < delays.size() && delays[k] >= 0 ? &delays_[(size_t) delays[k]] : nullptr;
      for (uint32_t c = 0; c < channels_; ++c) {
        const float* src = storage->data() + (size_t) c * maxBlock_;
        float* dst = out[c];
        if (!line || line->samples == 0) {
          if (!written) {
            for (uint32_t i = 0; i < frames; ++i) dst[i] = 0.0f + src[i];
          } else {
            for (uint32_t i = 0; i < frames; ++i) dst[i] += src[i];
          }
        } else {
          float* ring = line->buffer[c].data();
          const uint32_t size = line->samples;
          uint32_t w = line->write;
          for (uint32_t i = 0; i < frames;) {
            const uint32_t run = (std::min)(frames - i, size - w);
            float* const at = ring + w;
            if (!written) {
              for (uint32_t j = 0; j < run; ++j) {
                const float delayed = at[j];
                at[j] = src[i + j];
                dst[i + j] = 0.0f + delayed;
              }
            } else {
              for (uint32_t j = 0; j < run; ++j) {
                const float delayed = at[j];
                at[j] = src[i + j];
                dst[i + j] += delayed;
              }
            }
            i += run;
            w += run;
            if (w >= size) w = 0;
          }
          if (c + 1 == channels_) line->write = w; // advance once per block
        }
      }
      written = true;
    }
    if (!written)
      for (uint32_t c = 0; c < channels_; ++c) std::memset(out[c], 0, frames * sizeof(float));
  }

  std::vector<Node> nodes_;
  std::vector<NodeId> outputSources_;
  std::vector<int> outputSourceDelays_; // as Node::sourceDelays, for kOutput
  std::vector<NodeId> order_;
  std::vector<DelayLine> delays_;
  std::map<uint64_t, size_t> edgeIndex_;

  std::vector<float> inputBuffer_, mixBuffer_;
  /** The caller's MIDI for one slice of an over-long block. */
  MidiBuffer sliceMidi_;
  uint32_t maxBlock_ = 0, channels_ = 2, outputLatency_ = 0;
  bool prepared_ = false;
  bool restartPending_ = false;
};

} // namespace host
} // namespace sonore
