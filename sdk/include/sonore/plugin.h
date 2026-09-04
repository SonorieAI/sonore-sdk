// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: what a plugin declares about itself. The generated project fills
// this in once (identity + parameter table) and every format wrapper reads it:
// CLAP today, VST3/AU/standalone off the same descriptor.
//
// The parameter table is the CONTRACT that Sonorie already freezes at generation
// time: index order IS p[] order in the DSP, and the labels are what the UI
// binds to. Keeping it as plain data (no virtuals, no allocation) means a
// wrapper can read it before any instance exists, which is exactly what a host
// does when it scans.
#pragma once
#include <cmath> // the skew curve is a power law
#include <cstdint>
#include <cstdio>
#include <cstdlib> // std::strtod: MSVC leaks it via other headers, gcc does not
#include <cctype>
#include <string>
#include <cstring>
#include "audio.h"

namespace sonore {

/** One automatable control. Ranges are plain values (not normalised): the host
 *  sees the same numbers the user sees, which is what CLAP wants and what makes
 *  automation survive a range change. */
/**
 * Which kind of editor a plugin opens.
 *
 * Auto is what almost everything should use, and it means: a plugin that
 * supplies `uihtml` has ASKED for a web interface and gets one; a plugin that
 * supplies none gets the native editor.
 *
 * That rule makes native the default without taking anything away. Before it,
 * a plugin with no interface of its own got a generated HTML page inside a
 * browser process -- 100 to 300 MB of it, and nothing at all on a machine
 * whose webview runtime is missing. Now it gets controls drawn by the SDK, in
 * about two megabytes, on every machine.
 *
 * The explicit values are for the two plugins in a hundred that want the other
 * one: a bespoke web face forced to native for a session with thirty
 * instances, or a plugin with no HTML that still wants a webview because it is
 * about to load some.
 */
enum class EditorKind { Auto, Native, Web };

/** What a plugin ACTUALLY opened, which is not always what it asked for. */
enum class EditorBackend { Native, Web, None };

struct EditorChoice {
  EditorBackend backend = EditorBackend::None;
  /** Empty when the plugin got what it asked for. Otherwise why not -- and
   *  never a generic "unavailable", because a user reporting "my plugin has
   *  the wrong interface" deserves an answer in the first reply. */
  const char* reason = "";
};

/**
 * Decide which editor to open, and say so out loud when it is not the one
 * that was wanted.
 *
 * A pure function of four facts, deliberately: every branch here is a
 * degradation path, degradation paths are the ones nobody exercises by hand,
 * and this way all of them can be checked without a window, a webview runtime
 * or an operating system.
 *
 * `webAvailable` is whether this BUILD has a webview backend compiled in. A
 * runtime failure -- WebView2 not installed, WebKitGTK missing -- cannot be
 * known until create() is called, and is handled where that happens.
 */
inline EditorChoice chooseEditorBackend(EditorKind wanted, bool hasWebPage, bool nativeAvailable,
                                        bool webAvailable, const char* nativeUnavailableReason) {
  // Auto: a plugin that supplied a page asked for a web interface. A plugin
  // that supplied none never asked for a browser process, and used to get one
  // anyway.
  EditorBackend prefer = wanted == EditorKind::Web    ? EditorBackend::Web
                         : wanted == EditorKind::Native ? EditorBackend::Native
                         : hasWebPage                 ? EditorBackend::Web
                                                      : EditorBackend::Native;

  EditorChoice out;
  if (prefer == EditorBackend::Native) {
    if (nativeAvailable) {
      out.backend = EditorBackend::Native;
    } else if (webAvailable) {
      out.backend = EditorBackend::Web;
      out.reason = nativeUnavailableReason && *nativeUnavailableReason
                       ? nativeUnavailableReason
                       : "no native window backend for this platform";
    } else {
      out.reason = "neither a native window nor a webview backend in this build";
    }
    return out;
  }

  if (webAvailable) {
    out.backend = EditorBackend::Web;
  } else if (nativeAvailable) {
    out.backend = EditorBackend::Native;
    out.reason = "no webview backend in this build";
  } else {
    out.reason = "neither a webview nor a native window backend in this build";
  }
  return out;
}

struct ParamInfo {
  const char* id = "";        // stable machine id, never shown
  const char* label = "";     // display name, e.g. "Drive"
  const char* unit = "";      // e.g. "dB", "Hz", "%": may be empty
  float minValue = 0.0f;
  float maxValue = 1.0f;
  float defaultValue = 0.0f;
  /** >0 marks a stepped/enum control with this many discrete positions
   *  (a 4-way ratio switch is stepCount 4); 0 = continuous. */
  int stepCount = 0;
  /** Optional display group, e.g. "Filter" or "Envelope". Null or empty keeps
   *  the parameter at the top level, exactly as before. With forty controls a
   *  host's generic UI is unreadable without these; every format has a place
   *  to put them (CLAP module path, VST3 unit, AU clump, LV2 port group). */
  const char* group = nullptr;
  /**
   * What each step of a stepped control is CALLED.
   *
   * A stepped parameter without these renders as its index: a filter-type
   * switch shows "0", "1", "2" in the host's generic editor, in its automation
   * lane, and in every readout a user has when the plugin's own face is
   * closed. Which is not a display detail -- automation is the place people
   * work on a parameter they cannot see, and "2" tells them nothing about
   * whether the filter is a highpass.
   *
   * In order, index 0 first. Null means fall back to the number, which is
   * right for a control whose steps genuinely are numbers -- a 4-pole switch
   * is 2, 4, 6, 8 and naming those would be worse.
   *
   * `numValueNames` is not redundant with stepCount and cannot be inferred:
   * this is a bare pointer and there is no length in it. Bounds-checking
   * against stepCount instead LOOKS right and reads past the end of any table
   * with fewer entries than the control has steps -- which is a mistake
   * somebody will make, and did, in the first version of this.
   */
  const char* const* valueNames = nullptr;
  int numValueNames = 0;

  /**
   * Whether a host may RECORD and replay this control.
   *
   * True for almost everything, and a plugin that says false about the wrong
   * knob has taken away the reason people buy plugins. It is the right answer
   * for a handful: an oversampling switch that reallocates buffers, a
   * quality mode that changes latency, anything whose change is a
   * reconfiguration rather than a movement. Automating one of those is not a
   * sweep -- it is a rebuild, thirty times a second.
   *
   * A host told nothing assumes yes, which is why this defaults to true.
   */
  bool automatable = true;

  /**
   * Whether the host should show it at all.
   *
   * For a value that must live in the parameter list because a host has to
   * save and restore it, but that means nothing to a user reading a generic
   * panel -- an internal mode, a compatibility switch. Not a way to hide a
   * control from an interface: it is still automatable, still saved, still
   * everything else.
   */
  bool hidden = false;

  /**
   * Whether the plugin OWNS the value and the host may only read it.
   *
   * A gain-reduction readout on a compressor, a detected pitch, a load
   * meter: things a host can usefully display next to the plugin and can no
   * more set than it could set the output signal. A read-only parameter is
   * never automatable regardless of the flag above, because there is nothing
   * to record.
   *
   * Not every format has this. Where it does not, the flag degrades to an
   * ordinary parameter rather than disappearing -- the value still reaches
   * the host, it is just not protected from being written back.
   */
  bool readOnly = false;

  /**
   * How the range is spread across a knob's travel.
   *
   * 1.0 is linear, and linear is wrong for most of the controls a plugin
   * actually has. A cutoff from 20 Hz to 20 kHz puts 1 kHz at five per cent
   * of the sweep: everything musical is crushed into the bottom of the knob
   * and the top half is all hiss. The same is true of an attack from 0.1 ms
   * to 2 s, and of any delay time worth the name.
   *
   * Below 1.0 stretches the LOW end of the range across more of the travel,
   * which is what a frequency or a time control wants. Above 1.0 does the
   * opposite. The value is an exponent, so nothing here is guesswork -- but
   * an exponent is a bad thing to ask somebody to choose, which is what
   * skewForCentre() is for: say where the middle of the knob should land and
   * the exponent follows.
   *
   * Applies wherever a format has a normalised 0..1 value -- VST3's whole
   * parameter API, our own UI -- and is a display HINT in LV2 and AU. CLAP
   * has no notion of it: its parameters are plain values with a min and a
   * max, and a CLAP host draws a linear slider whatever we say. That is the
   * format, and a plugin's own interface is where it matters most anyway.
   */
  float skew = 1.0f;
};

/**
 * The exponent that puts `centre` at the middle of the knob.
 *
 * The way a plugin actually wants to specify this: "1 kHz should be halfway"
 * rather than "the exponent is 0.3". Derived from the definition -- at
 * halfway the normalised position is 0.5, so skew = log(0.5) / log(fraction),
 * where fraction is where the centre sits linearly.
 *
 * Returns 1.0 (linear) for a centre that is not strictly inside the range,
 * because a centre AT an end has no exponent that puts it in the middle and
 * the honest answer is to change nothing.
 */
inline float skewForCentre(float minValue, float maxValue, float centre) {
  const float span = maxValue - minValue;
  if (!(span > 0.0f)) return 1.0f;
  const float fraction = (centre - minValue) / span;
  if (!(fraction > 0.0f) || !(fraction < 1.0f)) return 1.0f;
  return (float) (std::log(0.5) / std::log((double) fraction));
}

/** Plain value -> 0..1, honouring the skew. The one place this curve is
 *  written: a plugin's own knob and the host's automation lane have to agree
 *  about where a value sits, and two implementations of a power law do not. */
inline double toNormalisedValue(const ParamInfo& p, double plain) {
  const double span = (double) p.maxValue - (double) p.minValue;
  if (span <= 0.0) return 0.0;
  double n = (plain - (double) p.minValue) / span;
  if (n < 0.0) n = 0.0;
  if (n > 1.0) n = 1.0;
  if (p.skew == 1.0f || !(p.skew > 0.0f)) return n;
  return std::pow(n, (double) p.skew);
}

/** 0..1 -> plain value. The exact inverse of the above; a stepped control is
 *  snapped by the caller, not here, because snapping is about what a value
 *  MEANS and this is about where it sits. */
inline double toPlainValue(const ParamInfo& p, double normalised) {
  // Same hole as clampToRange: a NaN normalised value survives both clamps
  // and a VST3 host handing one over would reach the DSP with it.
  if (!(normalised == normalised)) return (double) p.defaultValue;
  double n = normalised < 0.0 ? 0.0 : (normalised > 1.0 ? 1.0 : normalised);
  if (p.skew != 1.0f && p.skew > 0.0f) n = std::pow(n, 1.0 / (double) p.skew);
  return (double) p.minValue + n * ((double) p.maxValue - (double) p.minValue);
}

/** The name of one step, or null if this parameter does not name its steps.
 *  Bounds-checked, because a table shorter than stepCount is a mistake that
 *  would otherwise read past the end of somebody's static array. */
inline const char* paramValueName(const ParamInfo& p, int index) {
  if (!p.valueNames || p.stepCount <= 0) return nullptr;
  // BOTH bounds. stepCount says how many positions the control has;
  // numValueNames says how many of them were given names, and a table with
  // fewer entries than the control has steps is a mistake that must read as
  // "no name" rather than as whatever follows the array.
  if (index < 0 || index >= p.stepCount || index >= p.numValueNames) return nullptr;
  return p.valueNames[index];
}

/**
 * Where a stepped control's positions sit, and the ONE place that decides.
 *
 * Step i of a control with stepCount positions is
 *
 *     minValue + i * (maxValue - minValue) / (stepCount - 1)
 *
 * so the positions are spread evenly from the minimum to the maximum, and
 * for the common table -- minimum 0, maximum stepCount - 1 -- the value IS
 * the index. That common case is how this rule came to be written five
 * times before it was written once: the value formatter, the LV2 scale
 * points and the VST3 snap all assumed value == index, the native editor's
 * list spread the steps across the range, and the web bridge rounded the
 * value and never saw a name. A control declared 2..8 in four steps ("2, 4,
 * 6, 8 poles") was formatted, listed and snapped three different ways while
 * its own test passed, because it had no names to disagree about. Every
 * caller goes through these now, so there is nothing left to disagree.
 */
inline float stepSize(const ParamInfo& p) {
  if (p.stepCount < 2) return 0.0f;
  return (p.maxValue - p.minValue) / (float) (p.stepCount - 1);
}

/** The value at step `index`, clamped into the control's positions. The top
 *  step is the maximum EXACTLY rather than min + (n-1)*size, so float
 *  rounding can never leave the last position a hair under the range. */
inline float stepValueOf(const ParamInfo& p, int index) {
  if (p.stepCount <= 0 || index <= 0) return p.minValue;
  if (index >= p.stepCount - 1) return p.maxValue;
  return p.minValue + (float) index * stepSize(p);
}

/** The nearest step to a plain value, 0..stepCount-1. */
inline int stepIndexOf(const ParamInfo& p, float plain) {
  if (p.stepCount < 2) return 0;
  const float size = stepSize(p);
  if (!(size > 0.0f)) return 0;
  int index = (int) std::floor((plain - p.minValue) / size + 0.5f);
  if (index < 0) index = 0;
  if (index > p.stepCount - 1) index = p.stepCount - 1;
  return index;
}

/** A plain value moved onto its nearest step; a continuous control passes
 *  through untouched. */
inline float snapToStep(const ParamInfo& p, float plain) {
  return p.stepCount > 0 ? stepValueOf(p, stepIndexOf(p, plain)) : plain;
}

/** Whether every step lands on a whole number -- what LV2's integer property
 *  promises a host, so it is only claimed when true. A control stepping 0,
 *  0.5, 1 is stepped and not integer. */
inline bool stepsAreIntegers(const ParamInfo& p) {
  if (p.stepCount <= 0) return false;
  if (std::floor(p.minValue) != p.minValue) return false;
  if (p.stepCount == 1) return true;
  const float size = stepSize(p);
  return size >= 1.0f && std::floor(size) == size;
}

/** Whether the steps are the CONSECUTIVE integers min..max -- the stronger
 *  promise CLAP's stepped flag makes (a host steps such a control by one), so
 *  a control stepping 2, 4, 6, 8 is integer but not consecutive and is left
 *  continuous there; the parameter path snaps whatever arrives. */
inline bool stepsAreConsecutiveIntegers(const ParamInfo& p) {
  return stepsAreIntegers(p) && (p.stepCount == 1 || stepSize(p) == 1.0f);
}

/** The distinct group names in table order, so every format can number them
 *  the same way. Index 0 is reserved for "no group" in the formats that need
 *  a root, so a group's id is its index here PLUS ONE. */
struct GroupTable {
  static constexpr int kMaxGroups = 32;
  const char* names[kMaxGroups] = {};
  int count = 0;

  int indexOf(const char* name) const {
    if (!name || !name[0]) return -1;
    for (int i = 0; i < count; ++i)
      if (std::strcmp(names[i], name) == 0) return i;
    return -1;
  }
};

/** Collect the groups a parameter table declares, first-seen order. */
inline GroupTable collectGroups(const ParamInfo* params, int numParams) {
  GroupTable t;
  for (int i = 0; i < numParams; ++i) {
    const char* g = params[i].group;
    if (!g || !g[0]) continue;
    if (t.indexOf(g) >= 0) continue;
    if (t.count >= GroupTable::kMaxGroups) break;
    t.names[t.count++] = g;
  }
  return t;
}

/**
 * What sizes this plugin's editor may be.
 *
 * -- Why this is not four numbers in the wrapper --------------------------
 *
 * It was. kMinEditorWidth/Height were 320x200 for EVERY plugin, the maximum
 * was 8192 for every plugin, and preserve_aspect_ratio was hardcoded false.
 * Which means a synth whose interface has a five-octave keyboard along the
 * bottom could be resized to 320 pixels wide by any host, and a plugin drawn
 * as a fixed-proportion skin could be stretched to any shape at all -- both of
 * them silently, because nothing in the plugin had a way to say otherwise.
 *
 * Every format has somewhere to put this and all of them were being told the
 * same invented answer: CLAP's gui_get_resize_hints and adjust_size, VST3's
 * canResize and checkSizeConstraint, AU's view size, the standalone window.
 * The equivalent is conventionally called a bounds constrainer.
 *
 * -- The defaults are the old constants -----------------------------------
 *
 * 320x200 to 8192x8192, freely resizable, no aspect ratio. A plugin that
 * declares nothing behaves exactly as every plugin did before this existed,
 * which is the only version of this change that cannot break a shipped
 * product.
 *
 * -- When the box and the aspect ratio disagree ---------------------------
 *
 * They can be made to: 16:9 inside a 400..500 by 100..120 box has no solution.
 * The BOX wins, and the aspect is best-effort -- a window a few pixels off its
 * intended proportions is ugly, and a window outside the range its host will
 * give it is one the user cannot see all of. Stated because it is a real
 * choice and the opposite one is defensible.
 */
struct EditorConstraints {
  /** Inclusive, in LOGICAL pixels -- the size before any HiDPI scale, which is
   *  what a layout is written in and what every format's size call means. */
  uint32_t minWidth = 320;
  uint32_t minHeight = 200;
  uint32_t maxWidth = 8192;
  uint32_t maxHeight = 8192;

  /**
   * A fixed interface says false, and says it per axis.
   *
   * A bitmap skin is not resizable in either direction; a rack-style effect is
   * often resizable in width and fixed in height. An axis that cannot resize
   * is pinned to the editor's natural size rather than to its minimum -- being
   * told "you may not resize" and then being given the smallest allowed window
   * is the behaviour nobody wants.
   */
  bool resizableHorizontally = true;
  bool resizableVertically = true;

  /**
   * width / height the editor keeps, or 0 for free.
   *
   * 0 rather than a bool plus a ratio: one field cannot say "keep the aspect"
   * and "the aspect is 0" at once, and a plugin that sets a ratio has by
   * definition asked for it to be kept.
   */
  float aspectRatio = 0.0f;
};

/** The floor and ceiling no descriptor may cross. A declared minimum below
 *  this is a typo or a mistake, and a window of a few pixels is one nobody can
 *  find the edge of to fix. */
constexpr uint32_t kEditorSizeFloor = 64;
constexpr uint32_t kEditorSizeCeiling = 8192;

/**
 * Bring a size inside what the plugin declared. The ONE place that decides.
 *
 * `naturalWidth`/`naturalHeight` are the editor's designed size, used for an
 * axis that cannot be resized. Passed in rather than read from a macro so this
 * is a pure function -- which is what lets every rule in it be a test rather
 * than something checked by resizing a window in a host and looking.
 */
inline void applyEditorConstraints(const EditorConstraints& limits, uint32_t naturalWidth,
                                   uint32_t naturalHeight, uint32_t* width, uint32_t* height) {
  if (!width || !height) return;

  // The declared box, itself brought inside the absolute one. A descriptor
  // asking for a 4-pixel minimum or a 40000-pixel maximum is answered rather
  // than obeyed.
  uint32_t minW = limits.minWidth < kEditorSizeFloor ? kEditorSizeFloor : limits.minWidth;
  uint32_t minH = limits.minHeight < kEditorSizeFloor ? kEditorSizeFloor : limits.minHeight;
  uint32_t maxW = limits.maxWidth > kEditorSizeCeiling ? kEditorSizeCeiling : limits.maxWidth;
  uint32_t maxH = limits.maxHeight > kEditorSizeCeiling ? kEditorSizeCeiling : limits.maxHeight;
  // A descriptor with its maximum below its minimum describes no size at all.
  // The minimum wins: it is the one with a stated reason behind it.
  if (maxW < minW) maxW = minW;
  if (maxH < minH) maxH = minH;

  // A fixed axis is pinned to the NATURAL size, not to the minimum.
  if (!limits.resizableHorizontally) *width = naturalWidth;
  if (!limits.resizableVertically) *height = naturalHeight;

  if (*width < minW) *width = minW;
  if (*height < minH) *height = minH;
  if (*width > maxW) *width = maxW;
  if (*height > maxH) *height = maxH;

  if (limits.aspectRatio > 0.0f && limits.resizableHorizontally && limits.resizableVertically) {
    // Height follows width. If that lands outside the box, height is clamped
    // and WIDTH follows instead -- and if that is outside the box too, the box
    // wins and the ratio is not met. See the struct comment.
    uint32_t h = (uint32_t) ((float) *width / limits.aspectRatio + 0.5f);
    if (h < minH || h > maxH) {
      if (h < minH) h = minH;
      if (h > maxH) h = maxH;
      uint32_t w = (uint32_t) ((float) h * limits.aspectRatio + 0.5f);
      if (w < minW) w = minW;
      if (w > maxW) w = maxW;
      *width = w;
    }
    *height = h;
  }
}

struct Preset; // presets.h

/** One extra output bus. A drum sampler routes pads to their own channels, a
 *  splitter emits bands, a synth emits stems -- all of it is aux OUTPUT buses,
 *  which every format models as ports the host routes separately from the
 *  main out. Declared in the descriptor and handed to the DSP through
 *  ProcessContext::auxOut(). */
struct AuxBusInfo {
  const char* name = "Aux";
  int channels = 2;
};

/** Identity + capabilities. `isInstrument` flips the note input and drops the
 *  audio input bus, as a generated instrument project must. */
struct PluginDescriptor {
  const char* id = "com.sonorie.plugin";  // reverse-URI, unique per product
  const char* name = "Sonorie Plugin";
  const char* vendor = "Sonorie";
  const char* version = "1.0.0";
  const char* description = "";
  const char* url = "";
  bool isInstrument = false;
  const ParamInfo* params = nullptr;
  int numParams = 0;
  /** Optional factory presets, compiled in. See presets.h. */
  const Preset* presets = nullptr;
  int numPresets = 0;
  /** Optional metadata, emitted only where a format has a slot for it (LV2
   *  TTL today; CLAP feature lists next). Absent fields are simply omitted --
   *  never invented. */
  const char* category = nullptr; // "distortion", "reverb", "synth", ...
  const char* license = nullptr;  // URI of the licence terms; falls back to `url`
  const char* email = nullptr;    // maintainer contact, bare "user@host"
  /** Main-bus width the DSP genuinely handles, inclusive. The default keeps
   *  every existing plugin exactly as it was: fixed stereo. A DSP that loops
   *  over block.getNumChannels() instead of assuming two can widen the range
   *  (1..8) and every wrapper negotiates mono/surround from it -- effects are
   *  symmetric (in width == out width), instruments are output-only. */
  int minChannels = 2;
  int maxChannels = 2;
  /** Extra output buses, beyond the main one. Empty by default: a plugin that
   *  declares none exposes exactly one output port, as before. A DSP with aux
   *  buses must take the ProcessContext form of process(). */
  const AuxBusInfo* auxOutputs = nullptr;
  int numAuxOutputs = 0;
  /** Does this plugin EMIT MIDI? An arpeggiator, a chord generator, any note
   *  effect does. It costs an output note port / event bus in every format,
   *  so it is opt-in; a DSP that says yes writes into ctx.midiOut. */
  bool producesMidi = false;
  /** Does this instrument play EXPRESSIVELY -- per-note bend, pressure and
   *  timbre? Opting in declares MPE to the hosts that ask, translates MPE's
   *  channel-per-note MIDI into expression events, and fills
   *  ProcessContext::expression. Off by default: a DSP that also reads pitch
   *  bend from the MIDI stream would otherwise apply it twice.
   *
   *  REQUIRES the ProcessContext form of process(): the simple signatures have
   *  no expression parameter, so a DSP using one while setting this makes a
   *  promise it cannot keep, and a host routes a whole expressive controller
   *  into a plugin that silently ignores every nuance of it. There is no
   *  compile-time check because the descriptor is not constexpr, so the host
   *  tests enforce it instead -- and they caught exactly this in the sampler
   *  example, one test run after it was written. */
  bool supportsMpe = false;
  /** Native, web, or decided by whether `uihtml` was supplied. See
   *  EditorKind: Auto is right for almost everything. */
  EditorKind editor = EditorKind::Auto;
  /** How big the editor may be, and whether it may be resized at all. The
   *  defaults are the constants every wrapper used to hardcode, so a plugin
   *  that says nothing behaves exactly as it did. */
  EditorConstraints editorLimits;
};

/** Clamp a plain value into a parameter's declared range. A NaN is not a
 *  value at all, and it fails BOTH comparisons, so the first version of this
 *  passed it straight through to the DSP -- where a reverb turned it into a
 *  buffer index and crashed. Every wrapper's parameter path ends here, so this
 *  is the one place the promise "a DSP only ever sees an in-range value" is
 *  kept. A NaN becomes the declared default: the only in-range value that
 *  needs no guess. Infinities clamp like any other out-of-range number. */
inline float clampToRange(const ParamInfo& p, float v) {
  if (!(v == v)) return snapToStep(p, p.defaultValue);
  v = v < p.minValue ? p.minValue : (v > p.maxValue ? p.maxValue : v);
  // A stepped control lands ON a step. A host automating between two
  // positions, or a session that stored 0.4 for a switch, would otherwise
  // hand the DSP a value the control cannot show and the user cannot reach.
  return snapToStep(p, v);
}

/** The most parameters a descriptor may declare: the state blob's loader
 *  refuses a count above this (a corrupt header claiming four billion would
 *  spin the read loop for as long as the stream fed it), so a table larger
 *  than it could never be restored. */
constexpr int kMaxParams = 4096;

/**
 * The first thing wrong with one parameter's declaration, or null.
 *
 * None of these fails to compile and none of them crashes. A default that is
 * not on one of its control's steps, a table naming half the positions, a
 * minimum above the maximum: each is shown to a host as something slightly
 * different by every format, and found by a user rather than a build. The
 * descriptor is not constexpr, so this cannot be a static_assert; it is what
 * the real-time audit runs against every plugin instead.
 */
inline const char* paramInfoProblem(const ParamInfo& p) {
  if (!p.id || !p.id[0]) return "an empty id";
  if (!p.label || !p.label[0]) return "an empty label";
  const bool finite = p.minValue == p.minValue && p.maxValue == p.maxValue &&
                      p.defaultValue == p.defaultValue && std::fabs(p.minValue) < 1e30f &&
                      std::fabs(p.maxValue) < 1e30f;
  if (!finite) return "a range that is not a finite number";
  if (!(p.minValue < p.maxValue)) return "a minimum that is not below the maximum";
  if (p.defaultValue < p.minValue || p.defaultValue > p.maxValue)
    return "a default outside the range";
  if (!(p.skew > 0.0f) || !(p.skew < 1e6f)) return "a skew that is not a positive finite exponent";
  if (p.stepCount < 0) return "a negative stepCount";
  if (p.stepCount == 1) return "one step: a control that cannot move";
  if (p.stepCount > 0 && snapToStep(p, p.defaultValue) != p.defaultValue)
    return "a default that is not on one of the steps";
  if (p.numValueNames < 0) return "a negative numValueNames";
  if (p.valueNames && p.stepCount <= 0) return "value names on a continuous control";
  if (!p.valueNames && p.numValueNames > 0) return "numValueNames without a table";
  if (p.valueNames && p.numValueNames != p.stepCount) return "names for some steps but not all";
  for (int i = 0; p.valueNames && i < p.numValueNames; ++i)
    if (!p.valueNames[i] || !p.valueNames[i][0]) return "an empty step name";
  return nullptr;
}

/** The first thing wrong with a whole descriptor, or null. `badParam` names
 *  the offending parameter when the problem is in the table (-1 otherwise),
 *  because "a default outside the range" is only useful with a number. */
inline const char* descriptorProblem(const PluginDescriptor& d, int* badParam = nullptr) {
  if (badParam) *badParam = -1;
  if (!d.id || !d.id[0]) return "an empty plugin id";
  if (!d.name || !d.name[0]) return "an empty plugin name";
  if (!d.vendor || !d.vendor[0]) return "an empty vendor";
  if (!d.version || !d.version[0]) return "an empty version";
  if (d.numParams < 0) return "a negative numParams";
  if (d.numParams > kMaxParams) return "more parameters than the state format carries";
  if (d.numParams > 0 && !d.params) return "numParams without a parameter table";
  for (int i = 0; i < d.numParams; ++i) {
    if (const char* why = paramInfoProblem(d.params[i])) {
      if (badParam) *badParam = i;
      return why;
    }
    // Hosts and the web bridge key on the id; two controls sharing one are
    // one control as far as a session file is concerned.
    for (int j = 0; j < i; ++j)
      if (std::strcmp(d.params[i].id, d.params[j].id) == 0) {
        if (badParam) *badParam = i;
        return "a duplicate parameter id";
      }
  }
  // 8 is kMaxAudioChannels, the width every wrapper's buffers are sized for.
  if (d.minChannels < 1 || d.maxChannels < d.minChannels || d.maxChannels > 8)
    return "a main-bus channel range outside 1..8, or inverted";
  if (d.numAuxOutputs < 0) return "a negative numAuxOutputs";
  if (d.numAuxOutputs > 0 && !d.auxOutputs) return "numAuxOutputs without a bus table";
  for (int i = 0; i < d.numAuxOutputs; ++i) {
    if (!d.auxOutputs[i].name || !d.auxOutputs[i].name[0]) return "an aux bus with an empty name";
    if (d.auxOutputs[i].channels < 1 || d.auxOutputs[i].channels > 8)
      return "an aux bus outside 1..8 channels";
  }
  if (d.numPresets < 0) return "a negative numPresets";
  if (d.numPresets > 0 && !d.presets) return "numPresets without a preset table";
  return nullptr;
}

/** Render a value the way a host should display it ("2.30 kHz", "-6.0 dB").
 *  Writes at most `capacity` bytes including the terminator. ASCII only:
 *  sellers build with stock MSVC, which mangles UTF-8 literals without /utf-8. */
inline void formatParamValue(const ParamInfo& p, float value, char* out, size_t capacity) {
  if (!out || capacity == 0) return;
  // Already ON a step for a stepped control: the clamp snaps.
  const float v = clampToRange(p, value);
  if (p.stepCount > 0) {
    if (const char* name = paramValueName(p, stepIndexOf(p, v))) {
      std::snprintf(out, capacity, "%s", name);
      return;
    }
    // Whole-number steps print as the whole number, with the unit if there is
    // one; 0, 0.5, 1 falls through and prints like any other value.
    if (stepsAreIntegers(p)) {
      const int whole = (int) std::lround((double) v);
      if (p.unit && p.unit[0]) std::snprintf(out, capacity, "%d %s", whole, p.unit);
      else std::snprintf(out, capacity, "%d", whole);
      return;
    }
  }
  // Hz reads better as kHz past 1000: the one unit where the raw number is
  // genuinely harder to read than the scaled one.
  if (p.unit && std::strcmp(p.unit, "Hz") == 0 && v >= 1000.0f) {
    std::snprintf(out, capacity, "%.2f kHz", v / 1000.0f);
    return;
  }
  const float span = p.maxValue - p.minValue;
  const int decimals = span >= 100.0f ? 0 : (span >= 10.0f ? 1 : 2);
  if (p.unit && p.unit[0])
    std::snprintf(out, capacity, "%.*f %s", decimals, v, p.unit);
  else
    std::snprintf(out, capacity, "%.*f", decimals, v);
}

/** Parse a host-supplied string back to a plain value. Returns false when the
 *  text isn't a number at all, so the wrapper can decline rather than guess. */
/** ASCII case folding. Deliberately not tolower(): that one is locale-dependent
 *  and in a Turkish locale maps 'I' to a dotless \u0131, which would stop
 *  "Highpass" matching itself on a machine whose owner did nothing wrong. */
inline char asciiLower(char c) { return (c >= 'A' && c <= 'Z') ? (char) (c - 'A' + 'a') : c; }

/** Equal ignoring ASCII case and surrounding spaces. */
inline bool looseEquals(const char* a, const char* b) {
  if (!a || !b) return false;
  while (*a == ' ' || *a == '\t') ++a;
  while (*b == ' ' || *b == '\t') ++b;
  const char* aEnd = a + std::strlen(a);
  const char* bEnd = b + std::strlen(b);
  while (aEnd > a && (aEnd[-1] == ' ' || aEnd[-1] == '\t')) --aEnd;
  while (bEnd > b && (bEnd[-1] == ' ' || bEnd[-1] == '\t')) --bEnd;
  if (aEnd - a != bEnd - b) return false;
  for (; a < aEnd; ++a, ++b)
    if (asciiLower(*a) != asciiLower(*b)) return false;
  return true;
}

inline bool parseParamValue(const ParamInfo& p, const char* text, float* out) {
  if (!text || !out) return false;
  // A named step is matched by NAME first. A host that reads a value back out
  // of us and then types it in again -- which is what every "enter a value"
  // box does -- would otherwise get "Bandpass" and fail to parse it, and the
  // parameter would silently stay where it was.
  //
  // Loosely, because the other caller is now a PERSON typing into the editor's
  // own value box, and a person types "sine" and " Saw ". A host echoing our
  // own string back still matches exactly, so nothing about the strict case
  // got worse.
  if (p.stepCount > 0 && p.valueNames) {
    for (int i = 0; i < p.numValueNames && i < p.stepCount; ++i) {
      if (looseEquals(p.valueNames[i], text)) {
        *out = stepValueOf(p, i);
        return true;
      }
    }
  }
  char* end = nullptr;
  const double parsed = std::strtod(text, &end);
  if (end == text) return false;
  // strtod happily reads "nan" and "inf". Neither is a value a person meant
  // to set, so they are refused like any other text that is not a number --
  // silently landing on the default (which is what the clamp does with a
  // NaN) would leave someone staring at a knob that ignored what they typed.
  if (!std::isfinite(parsed)) return false;
  float v = (float) parsed;
  // Accept what we print: "2.3 kHz" must come back as 2300.
  if (p.unit && std::strcmp(p.unit, "Hz") == 0) {
    while (*end == ' ') ++end;
    if (*end == 'k' || *end == 'K') v *= 1000.0f;
  }
  *out = clampToRange(p, v);
  return true;
}

} // namespace sonore
