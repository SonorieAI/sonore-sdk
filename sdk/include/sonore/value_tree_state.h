// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: AudioProcessorValueTreeState: the reference's parameter object,
// built on this SDK's own parameters so ported code has somewhere to land.
//
// ── Why it exists, when the pieces already did ──────────────────────────────
//
// The reference's APVTS bundles four jobs: the parameter list, state beyond
// the parameters, undo, and binding widgets to values. This SDK has all four
// as separate things -- ParamInfo, StateBag, UndoHistory, and the attachments
// in gfx/plugin_editor.h -- and still does: a plugin that wants parameters
// without undo takes what it needs. What it lacked was the SHAPE. A processor
// ported from the reference builds a ParameterLayout of AudioParameterFloat
// and AudioParameterChoice, reads getRawParameterValue() in its process call,
// listens with parameterChanged(), saves copyState(), and binds sliders with
// SliderAttachment. Every one of those lines had to be rewritten by hand, and
// a hand rewrite of a parameter table is where a range or a default goes
// quietly wrong. This is that shape, made of the existing pieces, so those
// lines change their namespace and nothing else.
//
// ── How it maps onto the real parameters ────────────────────────────────────
//
// Every parameter in the layout produces a ParamInfo, and parameterInfos()
// is the table a PluginDescriptor points at -- the same order, so the DSP's
// p[i] is layout entry i. Ranges, skew, defaults, steps and value names go
// across exactly; text conversion defaults to formatParamValue and
// parseParamValue, the functions every wrapper and the host's own text boxes
// already use, so the knob, the automation lane and the host's generic panel
// cannot disagree. A layout entry with a curve ParamInfo cannot express -- a
// symmetric skew -- is reported by layoutProblem() rather than silently drawn
// linear by every host.
//
// ── The threads, which is the part that matters ─────────────────────────────
//
// The value of each parameter is one std::atomic<float>, the one
// getRawParameterValue() returns. The audio thread READS it, and does
// nothing else that could wait: updateFromHost(), the one call a DSP makes on
// the audio thread, stores the block's host values into those atomics with
// relaxed stores and sets a bit in an atomic mask. No lock, no allocation, no
// listener. Everything else -- listeners, gestures, undo, state -- is the
// message thread's, and a change that arrived from the audio side reaches
// listeners when the message thread calls dispatchPendingChanges() (or
// syncFromHost(), which also does it) on its UI tick.
//
// That is a deliberate difference from the reference, whose listeners are
// called on whichever thread changed the value, the audio thread included. A
// listener that repaints, allocates or locks was then running on the audio
// thread, and the reference's documentation spends a paragraph warning about
// it. Here a listener runs where it is safe to do anything, always.
//
// ── Undo ────────────────────────────────────────────────────────────────────
//
// Through UndoHistory, which stores whole states. A gesture -- a drag, a click
// on a switch, a typed value -- is one step, opened when the first gesture
// begins and recorded when the last one ends, and only if something actually
// changed: a click that moved nothing is not an undo step. Undoing pushes the
// restored values to the host inside gestures of their own, so the DAW's
// automation and the DSP follow the undo rather than the editor alone.
//
// A step restores only the parameters the EDIT changed, by their values in
// the snapshot; the snapshot names them (under "@changed"). Everything else
// may have moved since for reasons that are not this edit -- automation on
// another control while the user dragged this one, a value the host set --
// and restoring the whole snapshot put each back where it was and sent it to
// the host as an edit, which a DAW writing automation records over the lane.
// The non-parameter state is the same: a step puts it back only if an
// editState inside it changed it (marked "@state"). Something written to
// `state` directly is not undoable, and is not undone by undoing a knob
// either -- a tab the user moved to, a sample the plugin loaded since.
//
// Listeners edit too -- a parameter linked to the one moved, a mode that
// resets a gain -- and what they do is part of whatever they answer. An edit
// a listener makes while another edit is under way joins that edit's step,
// as nested actions join one transaction in the reference, so one user action
// is one undo. While an undo or redo is being applied nothing is recorded:
// pushing there threw away the redo just made, so a plugin with one linked
// parameter could never redo anything. And a listener's write to a parameter
// the step itself restores is dropped -- the step says where that one goes;
// letting the listener answer a half-restored state undid a mode switch to
// the reset gain rather than the user's. Undo and redo are refused while an
// edit is under way, as they are while a gesture is open.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "plugin.h"
#include "state_bag.h"
#include "undo.h"

namespace sonore {

/**
 * A range with an optional step and a power-law skew: the reference's
 * NormalisableRange, with its arithmetic. ParamInfo::skew is the same
 * exponent, so a range with an ordinary skew maps onto a parameter exactly.
 */
template <typename ValueType>
class NormalisableRange {
public:
  /** 0 to 1, linear and continuous. */
  NormalisableRange() = default;
  /** `rangeStart` to `rangeEnd` in plain units. An `intervalValue` above 0
   *  snaps every value convertFrom0to1() and snapToLegalValue() return onto
   *  that step, counted from the start; 0 is continuous. `skewFactor` is the
   *  exponent ParamInfo::skew describes: 1 is linear, below 1 gives the low
   *  end more of the travel, and 0 or below is treated as linear.
   *  `useSymmetricSkew` applies it to each half, outward from the middle, a
   *  curve ParamInfo cannot carry (layoutProblem() reports it). */
  NormalisableRange(ValueType rangeStart, ValueType rangeEnd, ValueType intervalValue = ValueType(0),
                    ValueType skewFactor = ValueType(1), bool useSymmetricSkew = false)
      : start(rangeStart), end(rangeEnd), interval(intervalValue), skew(skewFactor),
        symmetricSkew(useSymmetricSkew) {}

  /** Plain -> 0..1. */
  ValueType convertTo0to1(ValueType v) const {
    const ValueType span = end - start;
    if (!(span > ValueType(0))) return ValueType(0);
    ValueType p = (v - start) / span;
    p = p < ValueType(0) ? ValueType(0) : (p > ValueType(1) ? ValueType(1) : p);
    if (skew == ValueType(1) || !(skew > ValueType(0))) return p;
    if (!symmetricSkew) return (ValueType) std::pow(p, skew);
    const ValueType d = ValueType(2) * p - ValueType(1);
    const ValueType bent = (ValueType) std::pow(std::fabs(d), skew);
    return (ValueType(1) + (d < ValueType(0) ? -bent : bent)) / ValueType(2);
  }

  /** 0..1 -> plain, snapped to the interval. */
  ValueType convertFrom0to1(ValueType p) const {
    p = p < ValueType(0) ? ValueType(0) : (p > ValueType(1) ? ValueType(1) : p);
    if (!(p == p)) p = ValueType(0);
    if (skew != ValueType(1) && skew > ValueType(0)) {
      if (!symmetricSkew) {
        if (p > ValueType(0)) p = (ValueType) std::exp(std::log(p) / skew);
      } else {
        ValueType d = ValueType(2) * p - ValueType(1);
        if (d != ValueType(0)) {
          const ValueType bent = (ValueType) std::exp(std::log(std::fabs(d)) / skew);
          d = d < ValueType(0) ? -bent : bent;
        }
        p = (ValueType(1) + d) / ValueType(2);
      }
    }
    return snapToLegalValue(start + (end - start) * p);
  }

  /** Inside the range, and on the interval when there is one. */
  ValueType snapToLegalValue(ValueType v) const {
    if (!(v == v)) return start;
    if (interval > ValueType(0))
      v = start + interval * (ValueType) std::floor((v - start) / interval + ValueType(0.5));
    return v < start ? start : (v > end ? end : v);
  }

  /** The skew that puts `centre` halfway: skewForCentre's rule. */
  void setSkewForCentre(ValueType centre) {
    skew = (ValueType) skewForCentre((float) start, (float) end, (float) centre);
    symmetricSkew = false;
  }

  ValueType start = ValueType(0);    ///< The lowest plain value.
  ValueType end = ValueType(1);      ///< The highest; not above `start` maps every value to 0.
  ValueType interval = ValueType(0); ///< The step values snap to, from `start`; 0 is continuous.
  ValueType skew = ValueType(1);     ///< The curve's exponent: 1, or 0 and below, is linear.
  bool symmetricSkew = false;        ///< Bend each half outward from the middle, not from `start`.
};

/** A parameter's id, with the version hint the reference attaches to it. The
 *  hint has no meaning in this SDK's formats -- the parameter's position and
 *  id are what a session keys on -- and is kept so ported code compiles. */
struct ParameterID {
  /** An empty id, version 0. */
  ParameterID() = default;
  /** `identifier` copied, null read as empty. Implicit, so a string literal
   *  stands wherever a ParameterID is wanted, as in the reference. */
  ParameterID(const char* identifier, int versionHint = 0)
      : id(identifier ? identifier : ""), version(versionHint) {}
  /** As above, from a std::string. */
  ParameterID(std::string identifier, int versionHint = 0)
      : id(std::move(identifier)), version(versionHint) {}
  /** The id, which becomes the parameter's ParamInfo::id. */
  const std::string& getParamID() const { return id; }
  /** The hint given at construction; nothing in the SDK reads it. */
  int getVersionHint() const { return version; }

  std::string id;   ///< The stable machine id a session and the host key on.
  int version = 0;  ///< The reference's version hint, kept and unused.
};

class AudioProcessorValueTreeState;

/**
 * One parameter of a layout. The value lives here, in the atomic the audio
 * thread reads; the declaration lives in info(), the ParamInfo the wrappers
 * read. Not copyable, because the APVTS and the audio thread hold its address.
 */
class RangedAudioParameter {
public:
  /** The reference's attribute builder, for the options every type shares. */
  struct Attributes {
    std::string label; ///< The unit, "dB" or "Hz", shown after the value: ParamInfo::unit. Empty for none.
    /** The text for a PLAIN value, called with the caller's maximum length
     *  (the result is cut to it anyway). Empty uses formatParamValue. Only
     *  getText() and getCurrentValueAsText() call it: the host, the wrappers
     *  and the editor's attachments format through ParamInfo, which has no
     *  place for a function. */
    std::function<std::string(float value, int maximumLength)> stringFromValue;
    /** Text to a PLAIN value, legalised afterwards, in place of
     *  parseParamValue. As with stringFromValue, only getValueForText() calls
     *  it. */
    std::function<float(const std::string& text)> valueFromString;
    bool automatable = true; ///< ParamInfo::automatable: false for a control a host must not record.
    bool hidden = false;     ///< ParamInfo::hidden: out of the host's generic panel, still saved.
    /** ParamInfo::readOnly: the plugin reports the value, the host only reads
     *  it. Never automatable, and an edit through the APVTS is refused. */
    bool readOnly = false;
    /** ParamInfo::group, the display group. Empty takes the path of the
     *  AudioProcessorParameterGroup the parameter is added in, if any; a name
     *  given here wins over the group's. */
    std::string group;

    /** A copy with `label`, the unit, set. Each with...() returns a copy, so
     *  they chain: `Attributes().withLabel("dB").withAutomatable(false)`. */
    Attributes withLabel(std::string text) const {
      Attributes a = *this;
      a.label = std::move(text);
      return a;
    }
    /** A copy with `stringFromValue` set. */
    Attributes withStringFromValueFunction(std::function<std::string(float, int)> f) const {
      Attributes a = *this;
      a.stringFromValue = std::move(f);
      return a;
    }
    /** A copy with `valueFromString` set. */
    Attributes withValueFromStringFunction(std::function<float(const std::string&)> f) const {
      Attributes a = *this;
      a.valueFromString = std::move(f);
      return a;
    }
    /** A copy with `automatable` set. */
    Attributes withAutomatable(bool yes) const {
      Attributes a = *this;
      a.automatable = yes;
      return a;
    }
    /** A copy with `hidden` set. */
    Attributes withHidden(bool yes) const {
      Attributes a = *this;
      a.hidden = yes;
      return a;
    }
    /** A value the plugin reports and the host may only read: a meter. */
    Attributes withReadOnly(bool yes) const {
      Attributes a = *this;
      a.readOnly = yes;
      return a;
    }
    /** A copy with `group` set. */
    Attributes withGroup(std::string name) const {
      Attributes a = *this;
      a.group = std::move(name);
      return a;
    }
  };

  /** Virtual: a layout owns its parameters through this base. */
  virtual ~RangedAudioParameter() = default;
  /** Not copyable: the APVTS and the audio thread hold its address. */
  RangedAudioParameter(const RangedAudioParameter&) = delete;
  /** Not assignable, for the same reason. */
  RangedAudioParameter& operator=(const RangedAudioParameter&) = delete;

  /** The id from its ParameterID; info().id points into it. */
  const std::string& getParameterID() const { return id_; }
  /** The ParameterID's version hint, which nothing in the SDK reads. */
  int getVersionHint() const { return version_; }
  /** The display name (info().label), cut to at most `maximumLength` bytes
   *  when that is above 0, never inside a UTF-8 character (see clip()). */
  std::string getName(int maximumLength = 1024) const { return clip(name_, maximumLength); }
  /** The unit, Attributes::label: "dB", "Hz", or empty. */
  const std::string& getLabel() const { return attributes_.label; }
  /** The range as constructed, symmetric skew included -- which info() and
   *  every normalised value here draw linear (see layoutProblem()). */
  const NormalisableRange<float>& getNormalisableRange() const { return range_; }
  /** What the SDK's wrappers, editors and bridge read. */
  const ParamInfo& info() const { return info_; }
  /** Position in its APVTS -- the DSP's p[] index -- or -1 before adoption. */
  int getParameterIndex() const { return index_; }
  /** False when the attributes said so, and always for a read-only one. */
  bool isAutomatable() const { return info_.automatable && !info_.readOnly; }

  /** Normalised 0..1, through the SAME curve the host's lane uses. */
  float getValue() const { return convertTo0to1(value_.load(std::memory_order_relaxed)); }
  /** The default, normalised 0..1 the same way. */
  float getDefaultValue() const { return convertTo0to1(info_.defaultValue); }
  /** Plain to normalised 0..1, clamped, through info()'s curve
   *  (toNormalisedValue): the skew for a continuous control, evenly spaced
   *  positions for a stepped one. Pure arithmetic. */
  float convertTo0to1(float plain) const { return (float) toNormalisedValue(info_, plain); }
  /** Normalised 0..1 to plain, then legalised: in the range, on a step or the
   *  interval. A NaN gives the default. Pure arithmetic. */
  float convertFrom0to1(float normalised) const {
    return legalise((float) toPlainValue(info_, normalised));
  }

  /** [message-thread] Set from 0..1 and tell the host. Inside an APVTS this
   *  is an edit like a knob's: the host hears it, listeners hear it, and it
   *  is undoable. */
  void setValueNotifyingHost(float normalised);
  /** [message-thread] Bracket a run of edits as one gesture: one automation
   *  edit for the host, one undo step. */
  void beginChangeGesture();
  /** [message-thread] Close the gesture beginChangeGesture() opened. An end
   *  with no gesture open on this parameter is ignored, and both do nothing
   *  for a parameter no APVTS has adopted. */
  void endChangeGesture();

  /** The text for a `normalised` value: Attributes::stringFromValue when set,
   *  else formatParamValue -- "-6.0 dB", or a step's name. Cut to at most
   *  `maximumLength` bytes when that is above 0, never inside a UTF-8
   *  character (see clip()). Allocates. */
  virtual std::string getText(float normalised, int maximumLength) const {
    const float plain = convertFrom0to1(normalised);
    if (attributes_.stringFromValue) return clip(attributes_.stringFromValue(plain, maximumLength), maximumLength);
    char text[96];
    formatParamValue(info_, plain, text, sizeof(text));
    return clip(text, maximumLength);
  }

  /** Normalised. Text that is not a value of this parameter leaves it where
   *  it is -- parseParamValue refuses "12abc" rather than reading 12 -- so a
   *  typo moves nothing. */
  virtual float getValueForText(const std::string& text) const {
    if (attributes_.valueFromString) return convertTo0to1(legalise(attributes_.valueFromString(text)));
    float plain = 0.0f;
    if (!parseParamValue(info_, text.c_str(), &plain)) return getValue();
    return convertTo0to1(legalise(plain));
  }

  /** getText() of the value now, up to 1024 bytes. */
  std::string getCurrentValueAsText() const { return getText(getValue(), 1024); }

  /** The reference's count: positions for a stepped type, and for a float its
   *  interval's positions, or the "continuous" sentinel when it has none. */
  virtual int getNumSteps() const = 0;
  /** True for the types whose values are whole positions: int, bool and
   *  choice. A float is not, even with an interval. */
  virtual bool isDiscrete() const { return false; }
  /** True for AudioParameterBool only. */
  virtual bool isBoolean() const { return false; }

  /** The plain value as the audio thread reads it. */
  std::atomic<float>& rawValue() { return value_; }
  /** As above, read-only. */
  const std::atomic<float>& rawValue() const { return value_; }

  /** Inside the range and on a legal position: a step, the interval. Pure
   *  arithmetic, so the audio thread may call it. */
  virtual float legalise(float plain) const { return clampToRange(info_, plain); }

protected:
  /** For the concrete types. `defaultPlain` is stored as given, and the
   *  type's constructor then calls settleDefault(); `stepCount` is
   *  ParamInfo::stepCount (0 for continuous), `valueNames` its step names in
   *  order. The ParamInfo built here points into this object's strings. */
  RangedAudioParameter(const ParameterID& id, const std::string& name, NormalisableRange<float> range,
                       float defaultPlain, Attributes attributes, int stepCount,
                       std::vector<std::string> valueNames = {})
      : id_(id.id), version_(id.version), name_(name), range_(range),
        attributes_(std::move(attributes)), names_(std::move(valueNames)) {
    for (const std::string& n : names_) namePointers_.push_back(n.c_str());
    info_.id = id_.c_str();
    info_.label = name_.c_str();
    info_.unit = attributes_.label.c_str();
    info_.minValue = range_.start;
    info_.maxValue = range_.end;
    info_.stepCount = stepCount;
    info_.group = attributes_.group.empty() ? nullptr : attributes_.group.c_str();
    info_.valueNames = namePointers_.empty() ? nullptr : namePointers_.data();
    info_.numValueNames = (int) namePointers_.size();
    info_.automatable = attributes_.automatable;
    info_.hidden = attributes_.hidden;
    info_.readOnly = attributes_.readOnly;
    // ParamInfo has the plain power law and nothing else. A symmetric skew is
    // drawn linear rather than drawn wrong, and layoutProblem() says so.
    info_.skew = range_.symmetricSkew ? 1.0f : range_.skew;
    info_.defaultValue = defaultPlain;
    value_.store(defaultPlain, std::memory_order_relaxed);
  }

  /** Bring the default onto a legal position once the type's own rules are
   *  in place. Called by each concrete constructor, never by the base: a
   *  virtual call from a base constructor would reach the base's version. */
  void settleDefault() {
    info_.defaultValue = legalise(info_.defaultValue);
    value_.store(info_.defaultValue, std::memory_order_relaxed);
  }

  /** `s` cut to at most `maximumLength` bytes when that is above 0, else
   *  whole, and never inside a UTF-8 character: a cut that would split one
   *  ends before it, so the text may come out up to three bytes shorter.
   *  It cut at the byte count, and "Gain é" at 6 left the first byte of the
   *  "é", which no host can show and the next UTF-8 decoder rejects. Bytes
   *  that are not UTF-8 are cut where the count says. */
  static std::string clip(std::string s, int maximumLength) {
    if (maximumLength > 0 && (int) s.size() > maximumLength) {
      size_t cut = (size_t) maximumLength;
      // The byte at `cut` is the first one dropped. A continuation byte there
      // means the character it belongs to began before the cut.
      for (int back = 0; back < 3 && cut > 0 && ((unsigned char) s[cut] & 0xC0) == 0x80; ++back) --cut;
      if (((unsigned char) s[cut] & 0xC0) == 0x80) cut = (size_t) maximumLength; // not UTF-8
      s.resize(cut);
    }
    return s;
  }

  /** Re-point the group after an APVTS has given it one from a group path. */
  void setGroupName(const std::string& group) {
    attributes_.group = group;
    info_.group = attributes_.group.empty() ? nullptr : attributes_.group.c_str();
  }

private:
  friend class AudioProcessorValueTreeState;
  friend class AudioProcessorParameterGroup;

  std::string id_;
  int version_ = 0;
  std::string name_;
  NormalisableRange<float> range_;
  Attributes attributes_;
  std::vector<std::string> names_;
  std::vector<const char*> namePointers_;
  ParamInfo info_;
  std::atomic<float> value_{0.0f};
  AudioProcessorValueTreeState* owner_ = nullptr;
  int index_ = -1;
};

/** A continuous control. The interval, when there is one, is applied to every
 *  value -- a UI edit and the host's automation alike. */
class AudioParameterFloat : public RangedAudioParameter {
public:
  /** `range` and `defaultValue` in plain units; the default is clamped and
   *  put on the interval. A host sees a continuous control (stepCount 0)
   *  whatever the interval: the interval is applied here, not offered as
   *  steps. */
  AudioParameterFloat(const ParameterID& id, const std::string& name, NormalisableRange<float> range,
                      float defaultValue, const Attributes& attributes = Attributes())
      : RangedAudioParameter(id, name, range, defaultValue, attributes, 0) {
    settleDefault();
  }
  /** Linear and continuous from `minValue` to `maxValue`, with the default
   *  attributes. */
  AudioParameterFloat(const ParameterID& id, const std::string& name, float minValue, float maxValue,
                      float defaultValue)
      : AudioParameterFloat(id, name, NormalisableRange<float>(minValue, maxValue), defaultValue) {}

  /** The plain value: one relaxed load, so the audio thread may call it. */
  float get() const { return rawValue().load(std::memory_order_relaxed); }
  /** get(). */
  operator float() const { return get(); }
  /** [message-thread] An edit to `plain`, as setValueNotifyingHost() makes
   *  one: legalised, sent to the host, heard by listeners, undoable. */
  AudioParameterFloat& operator=(float plain) {
    setValueNotifyingHost(convertTo0to1(plain));
    return *this;
  }

  int getNumSteps() const override {
    const NormalisableRange<float>& r = getNormalisableRange();
    if (r.interval > 0.0f) return (int) ((r.end - r.start) / r.interval) + 1;
    return 0x7fffffff;
  }
  float legalise(float plain) const override {
    return clampToRange(info(), getNormalisableRange().snapToLegalValue(plain));
  }
};

/** Whole numbers from min to max, each a step a host can offer. */
class AudioParameterInt : public RangedAudioParameter {
public:
  /** `minValue` to `maxValue` inclusive, one step per number; `defaultValue`
   *  is clamped into them. A range of one number, or an inverted one, is a
   *  layout problem (layoutProblem()). */
  AudioParameterInt(const ParameterID& id, const std::string& name, int minValue, int maxValue,
                    int defaultValue, const Attributes& attributes = Attributes())
      : RangedAudioParameter(id, name, NormalisableRange<float>((float) minValue, (float) maxValue, 1.0f),
                             (float) defaultValue, attributes,
                             maxValue >= minValue ? maxValue - minValue + 1 : 0) {
    settleDefault();
  }

  /** The value, rounded to the nearest whole number: one relaxed load, so
   *  the audio thread may call it. */
  int get() const { return (int) std::lround(rawValue().load(std::memory_order_relaxed)); }
  /** get(). */
  operator int() const { return get(); }
  /** [message-thread] An edit to `value`, clamped into the range, as
   *  setValueNotifyingHost() makes one. */
  AudioParameterInt& operator=(int value) {
    setValueNotifyingHost(convertTo0to1((float) value));
    return *this;
  }
  int getNumSteps() const override { return info().stepCount; }
  bool isDiscrete() const override { return true; }
};

/** On or off. Its two positions are named "Off" and "On", so a host's lane,
 *  the generic editor and a toggle's label all say the same words. */
class AudioParameterBool : public RangedAudioParameter {
public:
  /** Two steps, plain 0 and 1, starting at `defaultValue`. */
  AudioParameterBool(const ParameterID& id, const std::string& name, bool defaultValue,
                     const Attributes& attributes = Attributes())
      : RangedAudioParameter(id, name, NormalisableRange<float>(0.0f, 1.0f, 1.0f),
                             defaultValue ? 1.0f : 0.0f, attributes, 2, {"Off", "On"}) {
    settleDefault();
  }

  /** On when the plain value is 0.5 or above: one relaxed load, so the audio
   *  thread may call it. */
  bool get() const { return rawValue().load(std::memory_order_relaxed) >= 0.5f; }
  /** get(). */
  operator bool() const { return get(); }
  /** [message-thread] An edit to on or off, as setValueNotifyingHost() makes
   *  one. */
  AudioParameterBool& operator=(bool on) {
    setValueNotifyingHost(on ? 1.0f : 0.0f);
    return *this;
  }
  int getNumSteps() const override { return 2; }
  bool isDiscrete() const override { return true; }
  bool isBoolean() const override { return true; }
};

/** One of a list of named choices; the value is the index. */
class AudioParameterChoice : public RangedAudioParameter {
public:
  /** `choiceNames` in order, index 0 first: each index is a step, named by
   *  its choice. `defaultItemIndex` is clamped to 0..size-1. Fewer than two
   *  choices is a layout problem (layoutProblem()). */
  AudioParameterChoice(const ParameterID& id, const std::string& name, std::vector<std::string> choiceNames,
                       int defaultItemIndex, const Attributes& attributes = Attributes())
      : RangedAudioParameter(id, name,
                             NormalisableRange<float>(0.0f, (float) (choiceNames.empty() ? 0 : choiceNames.size() - 1), 1.0f),
                             (float) defaultItemIndex, attributes, (int) choiceNames.size(), choiceNames),
        choices(std::move(choiceNames)) {
    settleDefault();
  }

  /** The chosen index, 0-based: one relaxed load, so the audio thread may
   *  call it. */
  int getIndex() const { return (int) std::lround(rawValue().load(std::memory_order_relaxed)); }
  /** The chosen entry of `choices`, or empty when there is none. Allocates. */
  std::string getCurrentChoiceName() const {
    const int i = getIndex();
    return i >= 0 && i < (int) choices.size() ? choices[(size_t) i] : std::string();
  }
  /** getIndex(). */
  operator int() const { return getIndex(); }
  /** [message-thread] An edit to choice `index`, clamped to the list, as
   *  setValueNotifyingHost() makes one. */
  AudioParameterChoice& operator=(int index) {
    setValueNotifyingHost(convertTo0to1((float) index));
    return *this;
  }
  int getNumSteps() const override { return (int) choices.size(); }
  bool isDiscrete() const override { return true; }

  /** The names given at construction, in order: also info()'s step names. */
  const std::vector<std::string> choices;
};

/**
 * A named group of parameters, nestable, as the reference builds them. Flattened
 * into the layout in order; each parameter's ParamInfo::group becomes the path
 * of group NAMES joined by the separator -- "Filter" or "Voice|Filter" --
 * which is what every format's grouping shows.
 */
class AudioProcessorParameterGroup {
public:
  /** `groupName` is what the path shows; `groupID` is kept for getID() and
   *  used for nothing else. `subgroupSeparator` joins the enclosing group's
   *  path to this group's name when this group is nested ("|" when empty).
   *  `items` are parameters and subgroups, in order, as addChild() takes
   *  them. */
  template <typename... Items>
  AudioProcessorParameterGroup(std::string groupID, std::string groupName, std::string subgroupSeparator,
                               std::unique_ptr<Items>... items)
      : id_(std::move(groupID)), name_(std::move(groupName)), separator_(std::move(subgroupSeparator)) {
    (addChild(std::move(items)), ...);
  }

  /** Append a parameter, owned here until a layout takes it; null is
   *  ignored. */
  void addChild(std::unique_ptr<RangedAudioParameter> parameter) {
    if (parameter) entries_.push_back(Entry{std::move(parameter), nullptr});
  }
  /** Append a subgroup; null is ignored. */
  void addChild(std::unique_ptr<AudioProcessorParameterGroup> group) {
    if (group) entries_.push_back(Entry{nullptr, std::move(group)});
  }

  /** The id given at construction. */
  const std::string& getID() const { return id_; }
  /** The name, the part of the path this group adds. */
  const std::string& getName() const { return name_; }
  /** The separator as given, empty included (the path then uses "|"). */
  const std::string& getSeparator() const { return separator_; }

private:
  friend class AudioProcessorValueTreeState;
  /** One child, in the order added: a parameter or a subgroup, exactly one
   *  of the two set. Owned here until flattenInto() moves the parameters out
   *  into the layout and empties the list. */
  struct Entry {
    std::unique_ptr<RangedAudioParameter> parameter;      ///< Set for a parameter child.
    std::unique_ptr<AudioProcessorParameterGroup> group;  ///< Set for a subgroup child.
  };

  /** Into `out`, in order, each parameter carrying its group path. */
  void flattenInto(std::vector<std::unique_ptr<RangedAudioParameter>>& out, const std::string& prefix) {
    const std::string path = prefix.empty() ? name_ : prefix + (separator_.empty() ? "|" : separator_) + name_;
    for (Entry& e : entries_) {
      if (e.parameter) {
        // A group named on the parameter itself is the more specific choice.
        if (e.parameter->attributes_.group.empty()) e.parameter->setGroupName(path);
        out.push_back(std::move(e.parameter));
      } else if (e.group) {
        e.group->flattenInto(out, path);
      }
    }
    entries_.clear();
  }

  std::string id_, name_, separator_;
  std::vector<Entry> entries_;
};

/**
 * The parameters, their state, their listeners and their undo, as one object.
 * See the file comment for the threads; in short, the audio thread calls
 * getRawParameterValue() and updateFromHost() and nothing else.
 */
class AudioProcessorValueTreeState {
public:
  /** The parameters, in the order the DSP will see them. */
  class ParameterLayout {
  public:
    /** Empty; add() fills it. */
    ParameterLayout() = default;
    /** Movable and not copyable: it owns its parameters. */
    ParameterLayout(ParameterLayout&&) = default;
    /** Moved, as above. */
    ParameterLayout& operator=(ParameterLayout&&) = default;

    /** The parameters and groups given, in order, as add() takes them. */
    template <typename... Items>
    ParameterLayout(std::unique_ptr<Items>... items) {
      add(std::move(items)...);
    }

    /** Append parameters and groups, as unique_ptrs, in order. A group is
     *  flattened into its parameters here, each carrying its group path;
     *  null pointers are skipped. */
    template <typename... Items>
    void add(std::unique_ptr<Items>... items) {
      (addOne(std::move(items)), ...);
    }

    /** A range of unique_ptrs, as a ported loop over a vector builds them.
     *  Only for things that increment: two unique_ptrs of one type are two
     *  parameters, and without the constraint they chose this overload. */
    template <typename Iterator, typename = decltype(++std::declval<Iterator&>())>
    void add(Iterator begin, Iterator end) {
      for (; begin != end; ++begin) addOne(std::move(*begin));
    }

  private:
    friend class AudioProcessorValueTreeState;
    void addOne(std::unique_ptr<RangedAudioParameter> p) {
      if (p) params_.push_back(std::move(p));
    }
    void addOne(std::unique_ptr<AudioProcessorParameterGroup> g) {
      if (g) flattenGroup(*g, params_);
    }
    std::vector<std::unique_ptr<RangedAudioParameter>> params_;
  };

  /** Told when a parameter's value changed. [message-thread], always. */
  struct Listener {
    /** The APVTS keeps a bare pointer: removeParameterListener() first. */
    virtual ~Listener() = default;
    /** `parameterID` now holds `newValue`, a PLAIN value, already stored.
     *  Called for an edit made here, an undo or redo, a state restore, and a
     *  change from the host once dispatchPendingChanges() or syncFromHost()
     *  delivers it; never for a value that did not change. A listener may
     *  edit parameters -- the edit joins the step under way, and during an
     *  undo or redo is not recorded (see the file comment) -- and may add or
     *  remove listeners, itself included. */
    virtual void parameterChanged(const std::string& parameterID, float newValue) = 0;
  };

  /**
   * What owns the REAL parameter -- a wrapper -- as four calls, by index.
   * The same four gfx::EditorHost carries, so a native editor's host connects
   * as it is; see editorHostFor() and hostLinkFrom() in
   * gfx/value_tree_state_attachments.h.
   */
  struct HostLink {
    /** The real parameter's PLAIN value at a 0-based index; what
     *  syncFromHost() reads back. */
    std::function<float(int index)> getParameter;
    /** Set the real parameter to a PLAIN value; every edit here calls it.
     *  Without it the object is not connected (isConnectedToHost()). */
    std::function<void(int index, float plain)> setParameter;
    /** Open a host gesture on parameter `index`. */
    std::function<void(int index)> beginGesture;
    /** Close the gesture beginGesture opened. */
    std::function<void(int index)> endGesture;
    /** The plugin's WHOLE state -- the bytes its host save path writes --
     *  and a state to put back through its restore path. Optional: without
     *  them nothing can be an editWholeState step, and everything else is
     *  as it was. A wrapper's EditorHost supplies them through its preset
     *  manager (gfx::hostLinkFrom). */
    std::function<bool(std::vector<uint8_t>& out)> saveState;
    /** Restore a state saveState wrote. False on failure, with the reason in
     *  `*why` when `why` is not null; the undo path passes null. */
    std::function<bool(const std::vector<uint8_t>& state, std::string* why)> loadState;
  };

  /** Widget bindings, defined in gfx/value_tree_state_attachments.h so this
   *  header carries no GUI. */
  class SliderAttachment;
  class ButtonAttachment;
  class ComboBoxAttachment;

  /**
   * `undoManager` may be null, and then nothing is undoable. `valueTreeType`
   * names the state, as the reference's tree type does, and is saved with it.
   */
  AudioProcessorValueTreeState(UndoHistory* undoManager, std::string valueTreeType, ParameterLayout layout)
      : undo_(undoManager), type_(std::move(valueTreeType)), params_(std::move(layout.params_)) {
    for (size_t i = 0; i < params_.size(); ++i) {
      params_[i]->owner_ = this;
      params_[i]->index_ = (int) i;
      infos_.push_back(params_[i]->info_);
    }
    open_.assign(params_.size(), 0);
    touched_.assign(params_.size(), 0);
    restoreSet_.assign(params_.size(), 0);
    pendingWords_ = (params_.size() + 31) / 32;
    pending_.reset(new std::atomic<uint32_t>[pendingWords_ > 0 ? pendingWords_ : 1]);
    for (size_t w = 0; w < (pendingWords_ > 0 ? pendingWords_ : 1); ++w)
      pending_[w].store(0, std::memory_order_relaxed);
    if (undo_) undo_->reset(serialised());
  }

  /** Not copyable: its parameters point back at it, and a DSP keeps pointers
   *  to their atomics. */
  AudioProcessorValueTreeState(const AudioProcessorValueTreeState&) = delete;
  /** Not assignable, for the same reason. */
  AudioProcessorValueTreeState& operator=(const AudioProcessorValueTreeState&) = delete;

  /** Destroys the parameters with it: every pointer from getParameter(),
   *  parameterAt(), getRawParameterValue() and parameterInfos() ends here.
   *  The UndoHistory is not the APVTS's and is left alone. */
  ~AudioProcessorValueTreeState() {
    for (auto& p : params_) p->owner_ = nullptr;
  }

  /** State beyond the parameters -- the reference's tree properties: which
   *  sample, which tab. Saved and restored by copyState()/replaceState(). Keys
   *  beginning "param." or "@" are the APVTS's own and are not copied. */
  StateBag state;

  // ── The table the SDK reads ──────────────────────────────────────────────

  /** What a PluginDescriptor's `params` points at, valid as long as this
   *  object lives. A plugin's static descriptor table is therefore one static
   *  APVTS built from the same layout function the DSP uses. */
  const ParamInfo* parameterInfos() const { return infos_.empty() ? nullptr : infos_.data(); }
  /** The layout's parameter count, groups flattened: the length of
   *  parameterInfos() and of the DSP's p[]. */
  int numParameters() const { return (int) params_.size(); }

  /** The first thing wrong with the layout, or null: every ParamInfo rule,
   *  plus duplicate ids, plus a curve ParamInfo cannot carry. */
  const char* layoutProblem(int* badIndex = nullptr) const {
    if (badIndex) *badIndex = -1;
    for (size_t i = 0; i < params_.size(); ++i) {
      const char* why = paramInfoProblem(params_[i]->info_);
      if (!why) {
        for (size_t j = 0; j < i && !why; ++j)
          if (params_[j]->id_ == params_[i]->id_) why = "a duplicate parameter id";
      }
      if (!why && params_[i]->range_.symmetricSkew && params_[i]->range_.skew != 1.0f)
        why = "a symmetric skew, which hosts would draw linearly";
      if (why) {
        if (badIndex) *badIndex = (int) i;
        return why;
      }
    }
    return nullptr;
  }

  /** Null when `params` is this layout's table -- same count, same ids, same
   *  order -- else what differs. For a DSP to check, once, that the
   *  descriptor the wrapper reads and the APVTS it reads p[] through agree. */
  const char* tableMismatch(const ParamInfo* params, int count) const {
    if (count != numParameters()) return "a different number of parameters";
    for (int i = 0; i < count; ++i)
      if (!params || !params[i].id || params_[(size_t) i]->id_ != params[i].id)
        return "a different id at the same position";
    return nullptr;
  }

  // ── Finding parameters ───────────────────────────────────────────────────

  /** -1 when there is no such id. By const char*, so an audio-thread caller
   *  passing a literal allocates nothing. */
  int indexOf(const char* id) const {
    if (!id) return -1;
    for (size_t i = 0; i < params_.size(); ++i)
      if (params_[i]->id_ == id) return (int) i;
    return -1;
  }
  /** As above, for a std::string. */
  int indexOf(const std::string& id) const { return indexOf(id.c_str()); }

  /** The parameter with this id, or null. Owned here and valid as long as
   *  this object; the lookup compares strings, so do it once. */
  RangedAudioParameter* getParameter(const char* id) const { return parameterAt(indexOf(id)); }
  /** As above, for a std::string. */
  RangedAudioParameter* getParameter(const std::string& id) const { return getParameter(id.c_str()); }
  /** The parameter at a 0-based layout position, or null outside
   *  0..numParameters()-1. */
  RangedAudioParameter* parameterAt(int index) const {
    return index >= 0 && index < numParameters() ? params_[(size_t) index].get() : nullptr;
  }

  /** The plain value the audio thread reads. Look it up once and keep the
   *  pointer; the lookup is a string comparison per parameter. */
  std::atomic<float>* getRawParameterValue(const char* id) const {
    RangedAudioParameter* p = getParameter(id);
    return p ? &p->value_ : nullptr;
  }
  /** As above, for a std::string. */
  std::atomic<float>* getRawParameterValue(const std::string& id) const {
    return getRawParameterValue(id.c_str());
  }

  /** The range the parameter was built with, symmetric skew included, or a
   *  default 0..1 range for an unknown id. */
  NormalisableRange<float> getParameterRange(const std::string& id) const {
    RangedAudioParameter* p = getParameter(id);
    return p ? p->range_ : NormalisableRange<float>();
  }

  /** The UndoHistory given at construction, or null. Not owned. */
  UndoHistory* getUndoManager() const { return undo_; }
  /** The `valueTreeType` given at construction, which copyState() saves
   *  under `"@type"`. replaceState() does not check it. */
  const std::string& getStateType() const { return type_; }

  // ── Listeners [message-thread] ───────────────────────────────────────────

  /** `listener` hears every change of parameter `id`. Not owned: remove it
   *  before it is destroyed. An unknown id or a null listener is ignored, and
   *  the same pair added twice is held once. */
  void addParameterListener(const std::string& id, Listener* listener) {
    const int i = indexOf(id);
    if (i < 0 || !listener) return;
    for (const auto& entry : listeners_)
      if (entry.first == i && entry.second == listener) return;
    listeners_.push_back({i, listener});
  }

  /** Undo addParameterListener() for this id and listener; nothing happens
   *  if they were not added. Safe from inside parameterChanged(): a listener
   *  removed there is not called again for the change being delivered. */
  void removeParameterListener(const std::string& id, Listener* listener) {
    const int i = indexOf(id);
    for (size_t k = listeners_.size(); k-- > 0;)
      if (listeners_[k].first == i && listeners_[k].second == listener)
        listeners_.erase(listeners_.begin() + (ptrdiff_t) k);
  }

  // ── The host ─────────────────────────────────────────────────────────────

  /** [message-thread] From now on every edit made here reaches the real
   *  parameter, and syncFromHost() reads it back. */
  void connectToHost(HostLink link) { link_ = std::move(link); }
  /** [message-thread] Drop the link: edits stay here, nothing reaches a
   *  host, and syncFromHost() only dispatches what updateFromHost() stored. */
  void disconnectFromHost() { link_ = HostLink(); }
  /** True once connected with a link whose setParameter is set. */
  bool isConnectedToHost() const { return (bool) link_.setParameter; }

  /**
   * [audio-thread] The block's parameter values, as the wrapper hands them
   * to the DSP in p[], into the raw atomics. Relaxed stores and a bit set per
   * changed parameter; listeners hear about it on the message thread, from
   * dispatchPendingChanges().
   */
  void updateFromHost(const float* values, int count) {
    if (!values) return;
    const int n = count < numParameters() ? count : numParameters();
    for (int i = 0; i < n; ++i) {
      const float v = values[i];
      if (!(v == v)) continue; // the wrapper never sends one; a caller might
      RangedAudioParameter& p = *params_[(size_t) i];
      const float legal = p.legalise(v);
      if (p.value_.load(std::memory_order_relaxed) == legal) continue;
      p.value_.store(legal, std::memory_order_relaxed);
      pending_[(size_t) i >> 5].fetch_or(1u << ((unsigned) i & 31u), std::memory_order_release);
    }
  }

  /** [message-thread] Tell listeners about everything updateFromHost()
   *  changed since the last call. Call it on the UI tick. */
  void dispatchPendingChanges() {
    for (size_t w = 0; w < pendingWords_; ++w) {
      uint32_t bits = pending_[w].exchange(0, std::memory_order_acquire);
      while (bits) {
        unsigned b = 0;
        while (!(bits & (1u << b))) ++b;
        bits &= ~(1u << b);
        const int i = (int) (w * 32 + b);
        if (i < numParameters()) notify(i, params_[(size_t) i]->value_.load(std::memory_order_relaxed));
      }
    }
  }

  /** [message-thread] Read every parameter back from the host -- automation,
   *  a preset the host loaded -- and then dispatch what the audio side
   *  changed. The UI tick's one call. */
  void syncFromHost() {
    if (link_.getParameter) {
      for (int i = 0; i < numParameters(); ++i) {
        RangedAudioParameter& p = *params_[(size_t) i];
        const float v = p.legalise(link_.getParameter(i));
        if (p.value_.load(std::memory_order_relaxed) == v) continue;
        p.value_.store(v, std::memory_order_relaxed);
        notify(i, v);
      }
    }
    dispatchPendingChanges();
  }

  // ── Edits [message-thread] ───────────────────────────────────────────────

  /**
   * An edit: legalised, stored, sent to the host, heard by listeners. Outside
   * a gesture it is an undo step of its own; inside one it is part of the
   * gesture's. Whatever listeners edit in answer is part of the same step.
   */
  void setParameterValue(int index, float plain) {
    RangedAudioParameter* p = parameterAt(index);
    if (!p || p->info_.readOnly) return;
    // A listener answering an undo or redo, writing a parameter the step
    // puts back: the step decides that one (see the file comment).
    if (restoring_ && restoreSet_[(size_t) index]) return;
    if (gestureDepth_ == 0 && editDepth_ == 0) beginStep(p->name_);
    ++editDepth_;
    const float v = p->legalise(plain);
    const bool changed = p->value_.load(std::memory_order_relaxed) != v;
    p->value_.store(v, std::memory_order_relaxed);
    touched_[(size_t) index] = 1;
    // Sent even when unchanged here: the raw value may be a tick behind the
    // host's, and an edit the user made must reach the host either way.
    if (link_.setParameter) link_.setParameter(index, v);
    if (changed) notify(index, v);
    --editDepth_;
    if (gestureDepth_ == 0 && editDepth_ == 0) endStep();
  }

  /** Open a gesture on parameter `index` (0-based) and tell the host. Edits
   *  until the matching endGesture() are one automation edit and one undo
   *  step; gestures on several parameters at once make one step, recorded
   *  when the last closes. Ignored outside the layout and for a read-only
   *  parameter. */
  void beginGesture(int index) {
    if (!parameterAt(index) || params_[(size_t) index]->info_.readOnly) return;
    if (gestureDepth_ == 0 && editDepth_ == 0) beginStep(params_[(size_t) index]->name_);
    ++gestureDepth_;
    ++open_[(size_t) index];
    if (link_.beginGesture) link_.beginGesture(index);
  }

  /** Unbalanced ends are ignored: a widget torn down mid-drag must not close a
   *  gesture somebody else opened. */
  void endGesture(int index) {
    if (!parameterAt(index) || open_[(size_t) index] == 0) return;
    --open_[(size_t) index];
    --gestureDepth_;
    if (link_.endGesture) link_.endGesture(index);
    if (gestureDepth_ == 0 && editDepth_ == 0) endStep();
  }

  /** Change the non-parameter state as one undoable step. It changes no
   *  parameter, so undoing it sets none -- unless `edit` sets one, which is
   *  then part of the step. */
  void editState(const std::string& label, const std::function<void(StateBag&)>& edit) {
    if (!edit) return;
    if (gestureDepth_ == 0 && editDepth_ == 0) beginStep(label);
    ++editDepth_;
    edit(state);
    stateEdited_ = true;
    --editDepth_;
    if (gestureDepth_ == 0 && editDepth_ == 0) endStep();
  }

  /**
   * A change made BEHIND the parameters, as ONE undo step: a preset loaded
   * through the plugin's state path (gfx/preset_bar.h), which moves every
   * parameter and the plugin's own state at once.
   *
   * The step holds the whole plugin state from before -- the link's
   * saveState -- and Undo puts it back through loadState, so undoing a
   * preset brings back the sample, the curve, whatever the DSP keeps, and
   * not only the knobs. One step, not one per parameter: the parameters land
   * through the state path, and this object follows them (syncFromHost)
   * without recording each as an edit of its own.
   *
   * `change` returns whether it worked, with `why` when it did not; a change
   * that failed records nothing. Without saveState and loadState in the link,
   * or with a gesture or another edit under way, the change still happens and
   * is simply not undoable. What listeners do in answer to it is part of it.
   */
  bool editWholeState(const std::string& label, const std::function<bool(std::string* why)>& change,
                      std::string* why = nullptr) {
    if (!change) return false;
    std::vector<uint8_t> before;
    const bool undoable = undo_ && !restoring_ && gestureDepth_ == 0 && editDepth_ == 0 &&
                          link_.saveState && link_.loadState && link_.saveState(before);
    const std::string snapshot = undoable ? wholeSnapshot(before) : std::string();
    ++editDepth_;
    wholeStep_ = undoable;
    const bool done = change(why);
    wholeStep_ = false;
    if (done) syncFromHost();
    --editDepth_;
    if (!done) return false;
    if (undoable) undo_->push(snapshot, label);
    return true;
  }

  // ── State ────────────────────────────────────────────────────────────────

  /** Everything, as a StateBag: "@type", "param.<id>" for each parameter,
   *  and the entries of `state`. What a DSP's saveState(StateBag&) hands
   *  back, or nests inside its own bag with setBytes. */
  StateBag copyState() const {
    StateBag out;
    out.setString("@type", type_);
    for (const auto& p : params_)
      out.setDouble(("param." + p->id_).c_str(), (double) p->value_.load(std::memory_order_relaxed));
    for (size_t i = 0; i < state.size(); ++i) {
      const std::string& key = state.keyAt(i);
      if (isReservedKey(key)) continue;
      const std::vector<uint8_t>* bytes = state.getBytes(key.c_str());
      if (bytes) out.setBytes(key.c_str(), bytes->data(), bytes->size());
    }
    return out;
  }

  /**
   * Restore what copyState() produced -- a session the host is reopening.
   *
   * A parameter the bag does not mention goes to its DEFAULT, as the state
   * blob's loader does with a control added since the session was saved:
   * keeping whatever the instance had would restore a state nobody saved.
   * Values are not sent to the host, which restored its own parameters from
   * the same session. The undo history starts again here, as the reference's
   * does: undoing past a session load into another session is not an edit.
   *
   * Except when the load is itself an undoable step, or the undo or redo of
   * one: a DSP whose loadState is this call, as a processor ported from the
   * reference has, is reached by editWholeState for a preset load
   * (gfx/preset_bar.h) and again when that step is undone. Starting again
   * there threw away every step before the preset, and undoing the preset
   * emptied the history in the middle of the undo -- no redo, and nothing
   * more to undo.
   */
  void replaceState(const StateBag& bag) {
    applyState(bag, /*toHost=*/false);
    if (undo_ && !wholeStep_ && !restoring_) undo_->reset(serialised());
  }

  // ── Undo [message-thread] ────────────────────────────────────────────────

  /** There is an UndoHistory with a step to undo, and no gesture, edit, undo
   *  or redo is under way: undo() will try. */
  bool canUndo() const { return undo_ && undo_->canUndo() && idle(); }
  /** The same for redo(). */
  bool canRedo() const { return undo_ && undo_->canRedo() && idle(); }

  /** Step back, and tell the host: each restored value goes out inside a
   *  gesture of its own, so the host records it as an edit. Refused while a
   *  gesture is open -- undoing under a hand that is still dragging would be
   *  undone again by the next mouse move -- and from a listener while an
   *  edit, an undo or a redo is under way. */
  bool undo() {
    if (!canUndo()) return false;
    // What the state is NOW is stored for redo, in the same kind as the step
    // being undone: a whole-state step has to be redone whole.
    std::string now;
    if (!currentFor(undo_->peekUndo(), now)) return false;
    return restore(undo_->undo(now));
  }

  /** Step forward again, telling the host as undo() does. False when
   *  canRedo() is, when a whole-state step cannot capture the state now, or
   *  when the step's state does not load. */
  bool redo() {
    if (!canRedo()) return false;
    std::string now;
    if (!currentFor(undo_->peekRedo(), now)) return false;
    return restore(undo_->redo(now));
  }

private:
  /** The group's parameters into a layout, through the APVTS -- which the
   *  group befriends -- rather than from the nested layout class itself. */
  static void flattenGroup(AudioProcessorParameterGroup& group,
                           std::vector<std::unique_ptr<RangedAudioParameter>>& out) {
    group.flattenInto(out, std::string());
  }

  static bool isReservedKey(const std::string& key) {
    return key.compare(0, 6, "param.") == 0 || (!key.empty() && key[0] == '@');
  }

  /** No gesture open, no edit under way, nothing being restored. */
  bool idle() const { return gestureDepth_ == 0 && editDepth_ == 0 && !restoring_; }

  /** Opens the step the outermost edit or gesture records into: what the
   *  state is before it, and nothing touched yet. Under an undo or redo there
   *  is no snapshot, so the step records nothing. */
  void beginStep(const std::string& label) {
    std::fill(touched_.begin(), touched_.end(), (char) 0);
    stateEdited_ = false;
    pendingUndo_.clear();
    if (undo_ && !restoring_) {
      pendingUndo_ = serialised();
      pendingLabel_ = label;
    }
  }

  /** Closes it. What it changed: a parameter an edit inside it touched, and
   *  not where it was when the step began. A click that moved nothing is not
   *  an undo step, and neither is automation that moved another control
   *  meanwhile. The same for the non-parameter state: the step's only when an
   *  editState inside it changed it, not when something wrote to `state`
   *  directly meanwhile. */
  void endStep() {
    if (pendingUndo_.empty()) return;
    StateBag before;
    before.deserialise((const uint8_t*) pendingUndo_.data(), pendingUndo_.size());
    std::string changed;
    for (size_t i = 0; i < params_.size(); ++i) {
      if (!touched_[i]) continue;
      const RangedAudioParameter& p = *params_[i];
      const float was = (float) before.getDouble(("param." + p.id_).c_str(), (double) p.info_.defaultValue);
      if (was == p.value_.load(std::memory_order_relaxed)) continue;
      if (!changed.empty()) changed += ',';
      changed += std::to_string(i);
    }
    const bool stateChanged = stateEdited_ && ownState(before) != ownState(state);
    if (!changed.empty() || stateChanged) undo_->push(naming(pendingUndo_, changed, stateChanged), pendingLabel_);
    pendingUndo_.clear();
  }

  /** The entries of `bag` that are not the APVTS's own, as bytes to compare. */
  static std::string ownState(const StateBag& bag) {
    StateBag own;
    for (size_t i = 0; i < bag.size(); ++i) {
      const std::string& key = bag.keyAt(i);
      if (isReservedKey(key)) continue;
      const std::vector<uint8_t>* bytes = bag.getBytes(key.c_str());
      if (bytes) own.setBytes(key.c_str(), bytes->data(), bytes->size());
    }
    std::vector<uint8_t> out;
    own.serialise(out);
    return std::string(out.begin(), out.end());
  }

  /** An undo or redo step applied. Listeners hear the restored values, and
   *  what they edit in answer is not recorded: it follows from the step. */
  bool restore(const std::string& text) {
    restoring_ = true;
    const bool ok = applySerialised(text);
    restoring_ = false;
    return ok;
  }

  std::string serialised() const {
    std::vector<uint8_t> bytes;
    copyState().serialise(bytes);
    return std::string(bytes.begin(), bytes.end());
  }

  /** A snapshot that also carries the whole plugin state, under "@whole"
   *  -- a reserved key, so it never leaks into `state`. */
  std::string wholeSnapshot(const std::vector<uint8_t>& whole) const {
    StateBag bag = copyState();
    bag.setBytes("@whole", whole.data(), whole.size());
    std::vector<uint8_t> bytes;
    bag.serialise(bytes);
    return std::string(bytes.begin(), bytes.end());
  }

  static bool isWholeSnapshot(const std::string& text) {
    StateBag bag;
    return !text.empty() && bag.deserialise((const uint8_t*) text.data(), text.size()) && bag.has("@whole");
  }

  /** `snapshot` naming the parameters its step changed, as indices joined by
   *  commas under "@changed" -- empty for a step that changed none -- and,
   *  under "@state", whether it changed the non-parameter state. */
  static std::string naming(const std::string& snapshot, const std::string& changed, bool withState) {
    StateBag bag;
    bag.deserialise((const uint8_t*) snapshot.data(), snapshot.size());
    bag.setString("@changed", changed);
    if (withState) bag.setString("@state", "1");
    std::vector<uint8_t> bytes;
    bag.serialise(bytes);
    return std::string(bytes.begin(), bytes.end());
  }

  /** The state now, as the kind of snapshot `step` is. False when a whole
   *  one is needed and the plugin cannot say what its state is. A step's
   *  "@changed" and "@state" go with it, so redo sets what undo set. */
  bool currentFor(const std::string& step, std::string& out) const {
    if (!isWholeSnapshot(step)) {
      StateBag stepBag;
      stepBag.deserialise((const uint8_t*) step.data(), step.size());
      const std::vector<uint8_t>* changed = stepBag.getBytes("@changed");
      out = changed ? naming(serialised(), std::string(changed->begin(), changed->end()), stepBag.has("@state"))
                    : serialised();
      return true;
    }
    std::vector<uint8_t> whole;
    if (!link_.saveState || !link_.saveState(whole)) return false;
    out = wholeSnapshot(whole);
    return true;
  }

  bool applySerialised(const std::string& text) {
    StateBag bag;
    if (text.empty() || !bag.deserialise((const uint8_t*) text.data(), text.size())) return false;
    if (const std::vector<uint8_t>* whole = bag.getBytes("@whole")) {
      // Back through the plugin's own restore path, which tells the host;
      // the parameters here follow what landed rather than being sent again.
      // Every parameter is the restored state's, whatever a listener says.
      std::fill(restoreSet_.begin(), restoreSet_.end(), (char) 1);
      if (!link_.loadState || !link_.loadState(*whole, nullptr)) return false;
      syncFromHost();
      state.clear();
      for (size_t i = 0; i < bag.size(); ++i) {
        const std::string& key = bag.keyAt(i);
        if (isReservedKey(key)) continue;
        const std::vector<uint8_t>* bytes = bag.getBytes(key.c_str());
        if (bytes) state.setBytes(key.c_str(), bytes->data(), bytes->size());
      }
      return true;
    }
    applyState(bag, /*toHost=*/true);
    return true;
  }

  void applyState(const StateBag& bag, bool toHost) {
    // An undo step's snapshot names what its edit changed, and only those
    // are set; a session's names nothing and sets every parameter.
    std::vector<char> only;
    if (const std::vector<uint8_t>* changed = bag.getBytes("@changed")) {
      only.assign(params_.size(), 0);
      size_t index = 0;
      bool digits = false;
      for (size_t k = 0; k <= changed->size(); ++k) {
        const char c = k < changed->size() ? (char) (*changed)[k] : ',';
        if (c >= '0' && c <= '9' && index < params_.size()) {
          index = index * 10 + (size_t) (c - '0');
          digits = true;
        } else if (c == ',') {
          if (digits && index < params_.size()) only[index] = 1;
          index = 0;
          digits = false;
        }
      }
    }
    if (only.empty()) std::fill(restoreSet_.begin(), restoreSet_.end(), (char) 1);
    else restoreSet_ = only;
    for (int i = 0; i < numParameters(); ++i) {
      if (!only.empty() && !only[(size_t) i]) continue;
      RangedAudioParameter& p = *params_[(size_t) i];
      const std::string key = "param." + p.id_;
      const double stored = bag.getDouble(key.c_str(), (double) p.info_.defaultValue);
      const float v = p.legalise((float) stored);
      const bool changed = p.value_.load(std::memory_order_relaxed) != v;
      p.value_.store(v, std::memory_order_relaxed);
      if (toHost && changed && link_.setParameter) {
        if (link_.beginGesture) link_.beginGesture(i);
        link_.setParameter(i, v);
        if (link_.endGesture) link_.endGesture(i);
      }
      if (changed) notify(i, v);
    }
    // The non-parameter state, likewise: a session's always, an undo step's
    // only if its edit changed it. Put back wholesale, undoing a knob also
    // undid a sample the plugin had loaded since, or the tab the user had
    // moved to -- which no undo step ever recorded.
    if (bag.has("@changed") && !bag.has("@state")) return;
    state.clear();
    for (size_t i = 0; i < bag.size(); ++i) {
      const std::string& key = bag.keyAt(i);
      if (isReservedKey(key)) continue;
      const std::vector<uint8_t>* bytes = bag.getBytes(key.c_str());
      if (bytes) state.setBytes(key.c_str(), bytes->data(), bytes->size());
    }
  }

  /** Over a COPY of the list: a listener may remove itself, or another, from
   *  inside its own callback -- an attachment being destroyed in response to
   *  a change is the ordinary case. */
  void notify(int index, float value) {
    if (listeners_.empty()) return;
    const std::vector<std::pair<int, Listener*>> snapshot = listeners_;
    const std::string& id = params_[(size_t) index]->id_;
    for (const auto& entry : snapshot) {
      if (entry.first != index) continue;
      bool stillThere = false;
      for (const auto& live : listeners_) stillThere = stillThere || live == entry;
      if (stillThere) entry.second->parameterChanged(id, value);
    }
  }

  UndoHistory* undo_ = nullptr;
  std::string type_;
  std::vector<std::unique_ptr<RangedAudioParameter>> params_;
  std::vector<ParamInfo> infos_;
  std::vector<std::pair<int, Listener*>> listeners_;
  HostLink link_;
  std::unique_ptr<std::atomic<uint32_t>[]> pending_;
  size_t pendingWords_ = 0;
  std::vector<int> open_;
  /** Parameters an edit inside the open step has set: what its undo step
   *  can have changed. */
  std::vector<char> touched_;
  int gestureDepth_ = 0;
  /** Edits under way: setParameterValue, editState and editWholeState calls
   *  not yet returned. Above zero, a listener's edit joins the open step. */
  int editDepth_ = 0;
  /** An editState ran inside the open step. */
  bool stateEdited_ = false;
  /** An undoable editWholeState's change is running: a replaceState inside
   *  it is that step, not a session that starts the history again. */
  bool wholeStep_ = false;
  /** An undo or redo being applied, and the parameters it restores. */
  bool restoring_ = false;
  std::vector<char> restoreSet_;
  std::string pendingUndo_;
  std::string pendingLabel_;
};

inline void RangedAudioParameter::setValueNotifyingHost(float normalised) {
  if (owner_) owner_->setParameterValue(index_, convertFrom0to1(normalised));
  else value_.store(convertFrom0to1(normalised), std::memory_order_relaxed);
}

inline void RangedAudioParameter::beginChangeGesture() {
  if (owner_) owner_->beginGesture(index_);
}

inline void RangedAudioParameter::endChangeGesture() {
  if (owner_) owner_->endGesture(index_);
}

} // namespace sonore
