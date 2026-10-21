// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the LADSPA binary interface, declared from the specification.
//
// ── Why these declarations are ours ─────────────────────────────────────────
//
// LADSPA is a C ABI: one exported function, `ladspa_descriptor`, that hands
// out descriptor structs by index, and a handful of integer flags. The ABI
// itself is a fact about how existing binaries are laid out and cannot be
// owned by anybody. The header that is conventionally #included to get it,
// however, is LGPL, and an Apache-2.0 header-only SDK that pulled it in would
// hand every plugin that includes host.h a licence question it never asked.
//
// So the layout is written here from the specification's text, not copied:
// the same fields in the same order, with this SDK's names. Nothing here may
// be reordered or resized. Every field is read by binaries compiled against
// the original declarations, and a struct that is one int out of step with
// theirs reads the port count from the copyright string.
//
// ── unsigned long, deliberately ─────────────────────────────────────────────
//
// The specification types its indices and counts as `unsigned long`, and that
// is 32 bits on Windows and 64 on Linux and macOS. It looks like the very
// thing a fixed-width integer should replace, and replacing it would break
// the ABI on exactly one of the platforms: a plugin built with the original
// header has `unsigned long` in its struct, so ours must too.
#pragma once

namespace sonore {
namespace ladspa {

/** A sample, and also a control value: LADSPA has one numeric type. */
typedef float Data;

/** Plugin-wide properties: a bit set. */
typedef int Properties;
/** run() is real-time safe in the usual sense: no allocation, no waiting. */
constexpr Properties kPropertyRealtime = 0x1;
/** An output buffer may NOT be the same as an input buffer. A host that runs
 *  plugins in place must give such a plugin separate buffers. */
constexpr Properties kPropertyInplaceBroken = 0x2;
/** Safe to run on a hard real-time thread. */
constexpr Properties kPropertyHardRtCapable = 0x4;

/** What one port is: a bit set of direction and kind. */
typedef int PortDescriptor;
constexpr PortDescriptor kPortInput = 0x1;
constexpr PortDescriptor kPortOutput = 0x2;
constexpr PortDescriptor kPortControl = 0x4;
constexpr PortDescriptor kPortAudio = 0x8;

/** How a host should present a control port: a bit set, plus a default
 *  selector packed into bits 6..9. */
typedef int HintDescriptor;
constexpr HintDescriptor kHintBoundedBelow = 0x1;
constexpr HintDescriptor kHintBoundedAbove = 0x2;
/** On or off: 0 is off, anything above is on. The bounds are meaningless. */
constexpr HintDescriptor kHintToggled = 0x4;
/** The bounds are FRACTIONS of the sample rate, to be multiplied by it. */
constexpr HintDescriptor kHintSampleRate = 0x8;
/** The control is best presented on a logarithmic scale. */
constexpr HintDescriptor kHintLogarithmic = 0x10;
/** Integer values only. */
constexpr HintDescriptor kHintInteger = 0x20;

/** The default selector: (hint & kHintDefaultMask) is one of the values
 *  below. The specification computes the "low", "middle" and "high" defaults
 *  from the bounds -- geometrically when kHintLogarithmic is set -- so the
 *  value itself is never stored. */
constexpr HintDescriptor kHintDefaultMask = 0x3C0;
constexpr HintDescriptor kHintDefaultNone = 0x0;
constexpr HintDescriptor kHintDefaultMinimum = 0x40;
constexpr HintDescriptor kHintDefaultLow = 0x80;
constexpr HintDescriptor kHintDefaultMiddle = 0xC0;
constexpr HintDescriptor kHintDefaultHigh = 0x100;
constexpr HintDescriptor kHintDefaultMaximum = 0x140;
constexpr HintDescriptor kHintDefault0 = 0x200;
constexpr HintDescriptor kHintDefault1 = 0x240;
constexpr HintDescriptor kHintDefault100 = 0x280;
constexpr HintDescriptor kHintDefault440 = 0x2C0;

/** One port's presentation hints. The bounds mean something only where the
 *  matching kHintBounded* bit is set. */
struct PortRangeHint {
  HintDescriptor hintDescriptor;
  Data lowerBound;
  Data upperBound;
};

/** An instance, opaque to the host. */
typedef void* Handle;

/**
 * One plugin type. A library may export several, indexed from 0 until the
 * entry point returns null.
 *
 * The field order is the ABI. See the file comment.
 */
struct Descriptor {
  /** Registered globally, so it identifies the plugin across libraries. */
  unsigned long uniqueId;
  /** A short machine name, unique within this library. */
  const char* label;
  Properties properties;
  const char* name;
  const char* maker;
  const char* copyright;
  unsigned long portCount;
  const PortDescriptor* portDescriptors;
  const char* const* portNames;
  const PortRangeHint* portRangeHints;
  /** The plugin's own, never touched by a host. */
  void* implementationData;

  /** The sample rate is fixed for the life of an instance: a host that changes
   *  rate makes a new one. */
  Handle (*instantiate)(const Descriptor* descriptor, unsigned long sampleRate);
  /** Every port must be connected before run(); a port may be reconnected
   *  between runs. */
  void (*connect_port)(Handle instance, unsigned long port, Data* dataLocation);
  /** Optional. Resets the plugin's internal state; run() follows. */
  void (*activate)(Handle instance);
  void (*run)(Handle instance, unsigned long sampleCount);
  /** Optional: adds into the outputs, scaled by the run-adding gain. */
  void (*run_adding)(Handle instance, unsigned long sampleCount);
  /** Optional, present exactly when run_adding is. */
  void (*set_run_adding_gain)(Handle instance, Data gain);
  /** Optional. Paired with activate(). */
  void (*deactivate)(Handle instance);
  void (*cleanup)(Handle instance);
};

/** The one symbol a LADSPA library exports, under the name
 *  "ladspa_descriptor". Returns null past the last plugin. */
typedef const Descriptor* (*DescriptorFunction)(unsigned long index);

/** The exported symbol's name, spelled once. */
constexpr const char* kEntryPointName = "ladspa_descriptor";

} // namespace ladspa
} // namespace sonore
