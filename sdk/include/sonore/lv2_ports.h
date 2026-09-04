// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the LV2 port map, in one place.
//
// An LV2 UI is a separate module from the plugin and receives port EVENTS by
// index. It therefore has to agree with the plugin about which index is what
// -- and until this file existed it did not have to, because it only ever
// looked at ports 0..numParams-1 and those happen to come first.
//
// The first port that broke that assumption was a meter. lv2_wrapper.h
// includes lv2_ui.h, so the UI could not see the wrapper's numbering even
// though it depended on it; the numbering has moved HERE, where both include
// it and neither can drift from the other.
//
// Everything below is derived from kDesc and the DSP's traits. No state, no
// ordering hazard, and one answer to "which port is that".
#pragma once

#include "clap_wrapper.h" // the traits the layout is derived from
#include "plugin.h"

namespace sonore {
namespace lv2 {

// ── Walking an atom sequence ───────────────────────────────────
//
// The plugin reads one and so does the interface, and they are separate
// modules that must agree exactly about the layout -- so the walk lives here,
// where both include it, rather than being written out on each side.
//
// The padding is the part worth centralising. An atom event is followed by
// its body rounded UP to eight bytes, and a reader that forgets the rounding
// walks into the middle of the next event and reads a size out of somebody's
// payload. That arithmetic appeared six times before this.

/** An atom body's size, padded to the alignment the spec requires. */
inline uint32_t atomPad(uint32_t size) { return (size + 7u) & ~7u; }

/** How far the next event is from this one. */
inline uint32_t atomStep(const LV2_Atom_Event* ev) {
  return (uint32_t) sizeof(LV2_Atom_Event) + atomPad(ev->body.size);
}

/**
 * Call `fn(const LV2_Atom_Event*)` for every event in a sequence.
 *
 * Tolerant of rubbish on purpose: a sequence is memory a HOST filled in, and
 * a plugin that trusts it crashes in somebody's session rather than in a
 * test. A size that runs past the buffer is where the walk stops rather than
 * where it reads.
 *
 * The copies this replaces each carried a `step == 0` guard against an
 * infinite loop. It could never fire -- a step is the event header plus a
 * padded body, so it is at least sixteen -- and it is gone rather than
 * carried forward, because a guard that cannot trigger reads as a hazard that
 * exists.
 */
template <typename Fn>
inline void forEachAtomEvent(const LV2_Atom_Sequence* seq, Fn&& fn) {
  if (!seq) return;
  const auto* body = (const uint8_t*) &seq->body;
  uint32_t offset = (uint32_t) sizeof(LV2_Atom_Sequence_Body);
  while (offset + sizeof(LV2_Atom_Event) <= seq->atom.size) {
    const auto* ev = (const LV2_Atom_Event*) (body + offset);
    const uint32_t step = atomStep(ev);
    // Checked BEFORE the body is handed over: an event claiming more bytes
    // than the sequence holds is one whose payload is not all there.
    //
    // Written as a subtraction rather than `offset + step > size`, because
    // that sum can wrap on a size an untrusted buffer supplied -- and a
    // wrapped comparison passes. The loop condition guarantees offset is
    // inside the sequence, so this subtraction cannot underflow.
    if (step > seq->atom.size - offset) break;
    fn(ev);
    offset += step;
  }
}

inline int numControlPorts() { return SONORE_NUM_PARAMS; }
inline bool isInstrument() { return kDesc.isInstrument; }
/** LV2 ports are STATIC, so the width is fixed at build time: stereo when the
 *  descriptor's range allows it, mono for a mono-only DSP. Surround stays a
 *  negotiated-format feature (CLAP/VST3/AU). */
/** How wide the main bus is in this format.
 *
 *  LV2 ports are FIXED: there is no arrangement to negotiate the way CLAP and
 *  VST3 negotiate one, so the bundle has to commit to a width when its TTL is
 *  written. Stereo where the DSP allows it, and otherwise the nearest edge of
 *  the range it declared.
 *
 *  That last clause is the fix. This used to fall back to ONE channel whenever
 *  stereo was not allowed, which is right for a mono-only DSP and wrong for a
 *  surround-only one: a plugin declaring 4..8 would have been given a single
 *  channel it never said it could process. Sharing defaultMainChannels() with
 *  the CLAP wrapper means the two cannot disagree about what a descriptor
 *  means. */
inline int lv2Width() { return (int) clapwrap::defaultMainChannels(); }
inline int audioInPorts() { return isInstrument() ? 0 : lv2Width(); }
inline int midiPorts() { return isInstrument() ? 1 : 0; }
/** Host bypass, LV2's way: a designated lv2:enabled control port. Effects
 *  only -- an instrument has no dry signal for a bypass to pass. */
inline int enabledPorts() { return isInstrument() ? 0 : 1; }
/** Latency, LV2's way: a designated lv2:latency OUTPUT control the plugin
 *  writes each run(). No port, and an oversampling effect plays early against
 *  everything the host time-aligns. Only exists when the DSP declares it. */
inline int latencyPorts() { return clapwrap::HasLatency<SonoreDsp>::value ? 1 : 0; }
// ── What an LV2 build is never told ─────────────────────────────
//
// A DSP that declares setHostInfo or setTrackInfo will hear from CLAP, from
// VST3 and from the standalone. It will never hear from here, and that is the
// format rather than an omission in this file:
//
//   Host identity  LV2 gives a host no way to name itself. There is no
//                  feature for it and no convention either -- the closest
//                  thing is the bundle URI of whoever loaded you, which is
//                  not available to a plugin.
//   Track info     Nothing in LV2 describes the track a plugin sits on. Some
//                  hosts pass a name through their own extensions; using one
//                  would tie the SDK to that host, which is exactly the
//                  problem track info exists to solve.
//   Host menu      There is no way for a UI to ask its host for the menu that
//                  carries MIDI learn and "remove automation". LV2 keeps the
//                  UI at arm's length from the host on purpose, and the price
//                  is that a right-click has nowhere to go.
//   State bag      NOT SENT YET, which is different from "cannot be". An LV2
//                  UI is a separate module and reaches the plugin only
//                  through ports -- but an ATOM port carries structured data,
//                  and telling a UI which file is loaded is the canonical
//                  example of what that mechanism exists for. Doing it
//                  properly needs a notification port in one direction AND a
//                  request port in the other, because a UI opened after the
//                  plugin has to be able to ask; half of that protocol is
//                  worse than none of it. Until it exists window.sonore.state
//                  stays empty here, and a page that needs a value on every
//                  format should put it in a PARAMETER, which crosses
//                  everywhere including this one.
//
// So both stay at their defaults: empty strings and false flags. A DSP that
// treats "no name" as a real answer -- which is what the documentation on
// HostInfo and TrackInfo asks for -- behaves correctly here without knowing
// which format it was built into.

/** A sidechain DSP grows a second stereo input pair, marked lv2:isSideChain
 *  and connectionOptional -- hosts route a key signal or leave it silent. */
inline int scPorts() {
  return (!isInstrument() && clapwrap::TakesSidechain<SonoreDsp>::value) ? 2 : 0;
}
/** Aux output channels, flattened: LV2 has no bus concept, so each aux bus
 *  becomes its own run of audio output ports named after it. */
/** An atom:Sequence OUTPUT port when the plugin emits MIDI. */
inline int midiOutPorts() { return kDesc.producesMidi ? 1 : 0; }

inline int auxOutChannels() {
  int n = 0;
  for (uint32_t b = 0; b < clapwrap::numAuxOutputs(); ++b)
    n += (int) clapwrap::auxBusChannels(b);
  return n;
}

/** lv2:freeWheeling, which every plugin gets.
 *
 *  This is LV2's way of saying what clap.render and VST3's processMode say:
 *  the host is rendering faster than real time, so nothing has to keep up
 *  with a clock and a DSP that trades quality for CPU can stop trading. The
 *  SDK grew that flag and wired it into two formats; this is the third, and
 *  without it an LV2 bounce silently got the monitoring-quality render.
 *
 *  Unconditional, and connectionOptional, so a host that does not know about
 *  it simply leaves the pointer null and the plugin reads "not freewheeling"
 *  -- which is the safe answer and the one every existing host gives. */
inline int freeWheelPorts() { return 1; }

/**
 * Two output control ports carrying the block's peak and RMS.
 *
 * Every other format's editor has meters and an LV2 build's had none, because
 * an LV2 UI is a different module and the only way anything reaches it is a
 * port. Raw peak and RMS rather than a finished needle position: the
 * ballistics live in MeterState, the UI already owns one, and computing them
 * here would be the same curve written twice with the plugin's copy running
 * on the audio thread.
 *
 * At the END of the port list on purpose. Every index before them is already
 * published in a ttl file, and inserting in the middle would silently
 * renumber somebody's saved session.
 */
inline int meterPorts() { return 2; }

/**
 * The two ATOM ports an interface talks over.
 *
 * A control port carries one float, which is enough for a knob and a meter
 * and nothing else. Anything structured -- which sample is loaded, what a
 * waveform looks like -- needs an atom port, and that is the mechanism every
 * LV2 plugin with a file browser uses.
 *
 * TWO of them, and the second is the reason this took a second attempt. A
 * notification port alone only works for a UI that was already open when the
 * plugin changed; one opened afterwards has missed everything and has no way
 * to catch up. So the UI ASKS on the way up, over the input port, and the
 * plugin answers on the output one.
 *
 * Present on every plugin rather than only the ones that need it: the port
 * map is published in a ttl file, and a layout that varies with what a DSP
 * happens to declare is a layout that renumbers when somebody adds a feature.
 */
inline int uiAtomPorts() { return 2; }

inline int totalPorts() {
  return numControlPorts() + enabledPorts() + latencyPorts() + freeWheelPorts() + midiPorts() +
         audioInPorts() + scPorts() + lv2Width() + auxOutChannels() + midiOutPorts() +
         meterPorts() + uiAtomPorts();
}

inline int portEnabled() { return numControlPorts(); } // effects only
inline int portLatency() { return numControlPorts() + enabledPorts(); }
inline int portFreeWheel() { return numControlPorts() + enabledPorts() + latencyPorts(); }
inline int portAudioInL() {
  return portFreeWheel() + freeWheelPorts() + midiPorts();
}
inline int portScInL() { return portAudioInL() + audioInPorts(); }
inline int portAudioOutL() { return portAudioInL() + audioInPorts() + scPorts(); }
inline int portAuxOut0() { return portAudioOutL() + lv2Width(); }
inline int portMidiOut() { return portAuxOut0() + auxOutChannels(); }
inline int portMeterPeak() { return portMidiOut() + midiOutPorts(); }
inline int portMeterRms() { return portMeterPeak() + 1; }
/** UI -> plugin: "tell me what you have". */
inline int portUiControl() { return portMeterRms() + 1; }
/** Plugin -> UI: the answer, and anything that changes later. */
inline int portUiNotify() { return portUiControl() + 1; }
inline int portMidiIn() { return portFreeWheel() + freeWheelPorts(); }
} // namespace lv2
} // namespace sonore
