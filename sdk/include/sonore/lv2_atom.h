// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: walking an LV2 atom sequence a host filled in.
//
// The plugin reads one and so does the interface, and they are separate
// modules that must agree exactly about the layout -- so the walk lives here,
// where both include it, rather than being written out on each side. It
// depends on nothing but LV2's own atom header, which is also what lets a
// test drive it with hostile bytes without building a plugin around it.
//
// The padding is the part worth centralising. An atom event is followed by
// its body rounded UP to eight bytes, and a reader that forgets the rounding
// walks into the middle of the next event and reads a size out of somebody's
// payload. That arithmetic appeared six times before this.
#pragma once

#include <cstdint>

#include <lv2/atom/atom.h>

namespace sonore {
namespace lv2 {

/** An atom body's size, padded to the alignment the spec requires. For sizes
 *  the caller chose; a size read from a host goes through atomStep(). */
inline uint32_t atomPad(uint32_t size) { return (size + 7u) & ~7u; }

/** How far the next event is from this one. In 64 bits, because the size is
 *  the host's: padded in 32, anything from 0xFFFFFFF9 up wraps to a step of
 *  sixteen, which passes every bounds check and hands a reader a body four
 *  gigabytes long. */
inline uint64_t atomStep(const LV2_Atom_Event* ev) {
  return (uint64_t) sizeof(LV2_Atom_Event) + (((uint64_t) ev->body.size + 7u) & ~(uint64_t) 7u);
}

/**
 * Call `fn(const LV2_Atom_Event*)` for every event in a sequence whose body
 * lies wholly inside it.
 *
 * Tolerant of rubbish on purpose: a sequence is memory a HOST filled in, and
 * a plugin that trusts it crashes in somebody's session rather than in a
 * test. A size that runs past the buffer is where the walk stops rather than
 * where it reads. `seq->atom.size` itself is the host's statement of how big
 * the buffer is, and the one number this has to take on trust.
 */
template <typename Fn>
inline void forEachAtomEvent(const LV2_Atom_Sequence* seq, Fn&& fn) {
  if (!seq) return;
  const auto* body = (const uint8_t*) &seq->body;
  uint32_t offset = (uint32_t) sizeof(LV2_Atom_Sequence_Body);
  while (offset + sizeof(LV2_Atom_Event) <= seq->atom.size) {
    const auto* ev = (const LV2_Atom_Event*) (body + offset);
    const uint64_t step = atomStep(ev);
    // Checked BEFORE the body is handed over: an event claiming more bytes
    // than the sequence holds is one whose payload is not all there. The loop
    // condition keeps offset inside the sequence, so the subtraction cannot
    // underflow, and the step cannot wrap in 64 bits.
    if (step > (uint64_t) (seq->atom.size - offset)) break;
    fn(ev);
    offset += (uint32_t) step;
  }
}

/**
 * Call `fn(const LV2_Atom_Property_Body*)` for every property of an atom
 * OBJECT whose value lies wholly inside it -- a time:Position, say.
 *
 * `obj` is the object's atom header, as an event's body is: its size counts
 * the object body (id, otype) and the properties after it. A property's
 * unpadded value must fit for it to be handed over; the padding after the
 * last one is the forge's habit, not a promise, so the step that includes it
 * only has to keep the walk inside the object. Tolerant of rubbish for the
 * same reason as the sequence walk.
 */
template <typename Fn>
inline void forEachAtomProperty(const LV2_Atom* obj, Fn&& fn) {
  if (!obj || obj->size < sizeof(LV2_Atom_Object_Body)) return;
  const auto* body = (const uint8_t*) (obj + 1);
  uint32_t offset = (uint32_t) sizeof(LV2_Atom_Object_Body);
  while ((uint64_t) offset + sizeof(LV2_Atom_Property_Body) <= obj->size) {
    const auto* prop = (const LV2_Atom_Property_Body*) (body + offset);
    const uint64_t remaining = (uint64_t) (obj->size - offset);
    const uint64_t used = (uint64_t) sizeof(LV2_Atom_Property_Body) + prop->value.size;
    if (used > remaining) break;
    fn(prop);
    const uint64_t step =
        (uint64_t) sizeof(LV2_Atom_Property_Body) + (((uint64_t) prop->value.size + 7u) & ~(uint64_t) 7u);
    if (step >= remaining) break;
    offset += (uint32_t) step;
  }
}

} // namespace lv2
} // namespace sonore
