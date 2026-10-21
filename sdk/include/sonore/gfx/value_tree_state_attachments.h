// SPDX-License-Identifier: Apache-2.0
//
// AudioProcessorValueTreeState's widget attachments: SliderAttachment,
// ButtonAttachment and ComboBoxAttachment, as the reference nests them.
//
// ── Built on the SDK's own attachments ──────────────────────────────────────
//
// gfx/plugin_editor.h already binds a Slider, a Button and a ComboBox to a
// parameter: the right curve, a gesture around every edit, value names on a
// switch, read-only parameters shown and never set. Those rules were each
// learnt the hard way and are tested there. These classes do not repeat
// them. Each one builds the SDK's attachment over an EditorHost whose four
// calls land in the APVTS, and adds the one thing an APVTS needs that an
// EditorHost polling on a tick does not: it LISTENS, so a change made in code,
// by undo, or by the host through syncFromHost() moves the widget at once.
//
// ── Kept out of value_tree_state.h ──────────────────────────────────────────
//
// So a DSP that includes the APVTS for its parameters compiles no GUI. The
// classes are declared inside AudioProcessorValueTreeState, as the reference
// declares them, and defined here.
//
// ── Lifetime ────────────────────────────────────────────────────────────────
//
// As in the reference: the APVTS and the widget must outlive the attachment.
// Destroying an attachment unhooks the widget's callbacks, so a widget that
// outlives its attachment is inert rather than calling into freed memory.
#pragma once

#include <memory>
#include <string>

#include "../user_presets.h"
#include "../value_tree_state.h"
#include "plugin_editor.h"
#include "widgets.h"

namespace sonore {
namespace gfx {

/**
 * An EditorHost whose calls land in an APVTS: an edit becomes
 * setParameterValue, a drag a gesture, and a read the raw atomic.
 *
 * Which also means the SDK's GenericEditor can be given an APVTS:
 *   GenericEditor(state.parameterInfos(), state.numParameters(),
 *                 editorHostFor(state), font)
 */
inline EditorHost editorHostFor(AudioProcessorValueTreeState& state) {
  EditorHost host;
  AudioProcessorValueTreeState* s = &state;
  host.getParameter = [s](int index) -> float {
    RangedAudioParameter* p = s->parameterAt(index);
    return p ? p->rawValue().load(std::memory_order_relaxed) : 0.0f;
  };
  host.setParameter = [s](int index, float plain) { s->setParameterValue(index, plain); };
  host.beginGesture = [s](int index) { s->beginGesture(index); };
  host.endGesture = [s](int index) { s->endGesture(index); };
  return host;
}

/** The other direction: connect an APVTS to whatever hosts a native editor,
 *  with the EditorHost a wrapper hands the editor. */
inline AudioProcessorValueTreeState::HostLink hostLinkFrom(const EditorHost& host) {
  AudioProcessorValueTreeState::HostLink link;
  link.getParameter = host.getParameter;
  link.setParameter = host.setParameter;
  link.beginGesture = host.beginGesture;
  link.endGesture = host.endGesture;
  // The whole state, from the instance's preset manager, so a preset load
  // can be one undo step (editWholeState) and undoing it restores the
  // plugin's own state through the same path a session restore takes.
  if (PresetManager* presets = host.presets) {
    link.saveState = [presets](std::vector<uint8_t>& out) { return presets->captureState(out); };
    link.loadState = [presets](const std::vector<uint8_t>& state, std::string* why) {
      return presets->restoreState(state, why);
    };
  }
  return link;
}

} // namespace gfx

/** A Slider bound to one parameter of an APVTS. */
class AudioProcessorValueTreeState::SliderAttachment : private AudioProcessorValueTreeState::Listener {
public:
  /**
   * [main-thread] Bind `slider` to the parameter `parameterID` of `state`.
   *
   * The slider's onValueChange, onDragStart and onDragEnd are taken over: a
   * drag, or a one-step edit, is one gesture and one undo step, through the
   * parameter's own normalisation curve. The slider moves the moment the
   * parameter changes -- in code, by undo, or from the host via syncFromHost --
   * since the state's listeners run on the main thread. A read-only parameter
   * is shown with the slider disabled.
   *
   * An ID the state does not have binds nothing: isBound() is false and the
   * slider is left as it was. `state` and `slider` must outlive this.
   */
  SliderAttachment(AudioProcessorValueTreeState& state, const std::string& parameterID,
                   gfx::Slider& slider)
      : state_(state), id_(parameterID), slider_(slider) {
    const int index = state_.indexOf(id_);
    // An id the layout does not have binds nothing, and leaves the widget as
    // it was rather than wired to parameter -1.
    if (index < 0) return;
    index_ = index;
    // The drag's two ends counted on the way through, so the destructor
    // knows whether it is leaving one open.
    gfx::EditorHost host = gfx::editorHostFor(state_);
    host.beginGesture = [this](int i) {
      ++openGestures_;
      state_.beginGesture(i);
    };
    host.endGesture = [this](int i) {
      if (openGestures_ > 0) --openGestures_;
      state_.endGesture(i);
    };
    inner_.reset(new gfx::SliderAttachment(slider_, state_.parameterAt(index)->info(), index, std::move(host)));
    state_.addParameterListener(id_, this);
  }

  /** Stops listening and unhooks the slider's three callbacks. A drag still in
   *  progress is ended here, so neither the host nor the undo history is left
   *  inside a gesture. */
  ~SliderAttachment() override {
    state_.removeParameterListener(id_, this);
    if (inner_) {
      slider_.onValueChange = nullptr;
      slider_.onDragStart = nullptr;
      slider_.onDragEnd = nullptr;
    }
    // Destroyed mid-drag -- the editor closed, or rebuilt itself, with the
    // button still down. The release that would have ended the drag now
    // reaches nobody, so it is ended here: otherwise the host keeps a gesture
    // open for good, and the APVTS, still inside it, folds every later edit
    // into an undo step that never closes -- undo is gone for the session.
    for (; openGestures_ > 0; --openGestures_) state_.endGesture(index_);
  }

  SliderAttachment(const SliderAttachment&) = delete;
  SliderAttachment& operator=(const SliderAttachment&) = delete;

  /** Whether `parameterID` named a parameter, so the slider is bound. */
  bool isBound() const { return inner_ != nullptr; }
  /** The SDK's own binding underneath: displayText(), setFromText(). */
  gfx::SliderAttachment* binding() { return inner_.get(); }

private:
  void parameterChanged(const std::string&, float) override {
    if (inner_) inner_->sync();
  }

  AudioProcessorValueTreeState& state_;
  std::string id_;
  gfx::Slider& slider_;
  int index_ = -1;
  int openGestures_ = 0;
  std::unique_ptr<gfx::SliderAttachment> inner_;
};

/** A toggle Button bound to a two-position parameter. */
class AudioProcessorValueTreeState::ButtonAttachment : private AudioProcessorValueTreeState::Listener {
public:
  /**
   * [main-thread] Bind `button` to the two-position parameter `parameterID`.
   *
   * The button becomes a toggle whose accessible name is the parameter's
   * label, and whose text is the value's name where the parameter has names.
   * It reads on at or above the middle of the range, and each press is one
   * complete edit to the minimum or the maximum. It follows the parameter the
   * moment it changes. A read-only parameter is shown with the button
   * disabled.
   *
   * An unknown ID binds nothing (isBound() false). `state` and `button` must
   * outlive this.
   */
  ButtonAttachment(AudioProcessorValueTreeState& state, const std::string& parameterID,
                   gfx::Button& button)
      : state_(state), id_(parameterID), button_(button) {
    const int index = state_.indexOf(id_);
    if (index < 0) return;
    inner_.reset(new gfx::ButtonAttachment(button_, state_.parameterAt(index)->info(), index,
                                           gfx::editorHostFor(state_)));
    state_.addParameterListener(id_, this);
  }

  /** Stops listening and unhooks the button's onToggle. */
  ~ButtonAttachment() override {
    state_.removeParameterListener(id_, this);
    if (inner_) button_.onToggle = nullptr;
  }

  ButtonAttachment(const ButtonAttachment&) = delete;
  ButtonAttachment& operator=(const ButtonAttachment&) = delete;

  /** Whether `parameterID` named a parameter, so the button is bound. */
  bool isBound() const { return inner_ != nullptr; }

private:
  void parameterChanged(const std::string&, float) override {
    if (inner_) inner_->sync();
  }

  AudioProcessorValueTreeState& state_;
  std::string id_;
  gfx::Button& button_;
  std::unique_ptr<gfx::ButtonAttachment> inner_;
};

/** A ComboBox bound to a stepped parameter. The items are the parameter's
 *  value names -- a choice parameter's choices -- filled in by the
 *  attachment, so the list and the parameter cannot disagree about order. */
class AudioProcessorValueTreeState::ComboBoxAttachment : private AudioProcessorValueTreeState::Listener {
public:
  /**
   * [main-thread] Bind `box` to the stepped parameter `parameterID`.
   *
   * The box's items are replaced by the parameter's value names -- the step
   * numbers where it has none -- and each choice is one complete edit. It
   * follows the parameter the moment it changes. A read-only parameter is
   * shown with the box disabled.
   *
   * An unknown ID binds nothing (isBound() false). `state` and `box` must
   * outlive this.
   */
  ComboBoxAttachment(AudioProcessorValueTreeState& state, const std::string& parameterID,
                     gfx::ComboBox& box)
      : state_(state), id_(parameterID), box_(box) {
    const int index = state_.indexOf(id_);
    if (index < 0) return;
    inner_.reset(new gfx::ComboAttachment(box_, state_.parameterAt(index)->info(), index,
                                          gfx::editorHostFor(state_)));
    state_.addParameterListener(id_, this);
  }

  /** Stops listening and unhooks the box's onChange. */
  ~ComboBoxAttachment() override {
    state_.removeParameterListener(id_, this);
    if (inner_) box_.onChange = nullptr;
  }

  ComboBoxAttachment(const ComboBoxAttachment&) = delete;
  ComboBoxAttachment& operator=(const ComboBoxAttachment&) = delete;

  /** Whether `parameterID` named a parameter, so the box is bound. */
  bool isBound() const { return inner_ != nullptr; }

private:
  void parameterChanged(const std::string&, float) override {
    if (inner_) inner_->sync();
  }

  AudioProcessorValueTreeState& state_;
  std::string id_;
  gfx::ComboBox& box_;
  std::unique_ptr<gfx::ComboAttachment> inner_;
};

} // namespace sonore
