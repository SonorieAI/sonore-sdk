// SPDX-License-Identifier: Apache-2.0
//
// A readout you can type into.
//
// ── Why this is not a nicety ────────────────────────────────────────────────
//
// Every generated plugin's editor showed its parameter values in a Label, and
// a Label is not editable. So the only way to reach a value was to drag: fine
// for a filter sweep, useless for "set the output to exactly -6.0 dB", which is
// a thing engineers do constantly and check afterwards. A knob with a 66 dB
// range across 200 pixels cannot be dragged to a tenth of a decibel, and the
// number underneath it was there to be READ and not to be used.
//
// The oldest trick in the book -- a slider's text box, editable on a double
// click. It is one of those features that is invisible until it is
// missing and then is the first complaint.
//
// ── What it takes to get right ─────────────────────────────────────────────
//
// It has to accept what it PRINTS. A box that shows "2.30 kHz" and then refuses
// "2.30 kHz" back is worse than no box, because the user typed exactly what
// they were shown. parseParamValue already handles that -- the kHz suffix, and
// named steps by name -- because a host doing "enter a value" needs the same
// thing, and having one parser is the only way the two can agree.
//
// It has to be ONE undo step. A typed value that arrives as a bare setParameter
// with no gesture around it is an automation write the host cannot group, so
// the DAW records a point rather than an edit and undo does not take it back.
//
// And a value it cannot parse must NOT be silently thrown away. Reverting to
// the old number looks identical to the plugin ignoring the keyboard; staying
// in the field with the text selected says "that is not a number I can use"
// without a dialog.
#pragma once

#include <functional>
#include <string>
#include <utility>

#include "text_editor.h"
#include "widgets.h"

namespace sonore {
namespace gfx {

class ValueBox : public Widget {
public:
  ValueBox() {
    // The box itself never takes the keyboard. Only the field inside it does,
    // and only while it is up -- otherwise Tab traversal would stop on every
    // readout in the editor, and there is one per parameter.
    setWantsKeyboardFocus(false);
    setCursor(MouseCursor::Text);
    // Left out of the accessible tree, and its CHILDREN kept -- which is what
    // setAccessibilityIgnored means and why it is not setVisible. The slider
    // beside this already announces the same value; a reader that read both
    // would say "-6.0 dB" twice for every knob, which is the kind of noise
    // that makes people turn a screen reader off. The text field inside it is
    // announced normally whenever it is up, because at that moment it is the
    // thing the user is in.
    setAccessibilityIgnored(true);

    editor_.setVisible(false);
    editor_.onReturn = [this](const std::string& typed) { commit(typed); };
    // Focus lost is a COMMIT, not a cancel. Clicking away from a field you have
    // just typed into means you are done with it; asking for Return as well is
    // the behaviour that makes people type a value, look at it, click the next
    // control and find the first one unchanged.
    editor_.onFocusLost = [this](const std::string& typed) {
      if (editing_) commit(typed);
    };
    addChild(&editor_);
  }

  /** The text shown when not being edited. Set from the attachment on every UI
   *  tick, so it follows automation like everything else. */
  void setText(std::string t) {
    if (t == text_) return;
    text_ = std::move(t);
    // NOT into the field while it is up: overwriting what somebody is halfway
    // through typing, thirty times a second, is the bug this guard exists for.
    if (!editing_) repaint();
  }

  const std::string& text() const { return text_; }

  void setJustify(Justify j) {
    justify_ = j;
    repaint();
  }

  /** Off makes it an ordinary readout, which is what a meter or a read-only
   *  value wants. */
  void setEditable(bool editable) {
    editable_ = editable;
    setCursor(editable ? MouseCursor::Text : MouseCursor::Default);
    if (!editable && editing_) cancel();
  }

  bool isEditable() const { return editable_; }
  bool isEditing() const { return editing_; }

  /**
   * What to do with typed text. Return false when it cannot be used.
   *
   * False keeps the field open with the text selected rather than reverting,
   * because reverting is indistinguishable from the keyboard being ignored.
   */
  std::function<bool(const std::string&)> onTextEntered;

  void beginEditing() {
    if (!editable_ || editing_) return;
    editing_ = true;
    editor_.setBounds(localBounds());
    editor_.setText(text_);
    editor_.setVisible(true);
    editor_.selectAll();
    if (MouseRouter* r = routerFor(this)) r->setFocus(&editor_);
    repaint();
  }

  /** Escape, or losing the ability to edit. The typed text is discarded and the
   *  parameter is untouched. */
  void cancel() {
    if (!editing_) return;
    editing_ = false;
    editor_.setVisible(false);
    if (MouseRouter* r = routerFor(this))
      if (r->focused() == &editor_) r->setFocus(nullptr);
    repaint();
  }

  void resized() override {
    if (editing_) editor_.setBounds(localBounds());
  }

  void paint(Graphics& g) override {
    // Nothing while the field is up: it covers this exactly, and drawing the
    // old value underneath shows through a field with a transparent ground.
    if (editing_) return;
    lookAndFeel().drawLabel(g, localBounds(), text_, font(), justify_, stateFor(false));
  }

  void mouseDown(const MouseEvent& e) override {
    // Double click, the conventional gesture and the same one that resets a
    // knob -- which is why a readout is a separate component from the control
    // beside it rather than a region of it.
    if (editable_ && e.clickCount >= 2) beginEditing();
  }

  bool keyPressed(const KeyPress& key) override {
    if (editing_ && key.is(KeyPress::Escape)) {
      cancel();
      return true;
    }
    return false;
  }

private:
  void commit(const std::string& typed) {
    if (!editing_) return;
    // Accepted or not, the DECISION belongs to whoever owns the parameter.
    const bool accepted = onTextEntered ? onTextEntered(typed) : false;
    if (!accepted) {
      // Stay, and select what was typed so the next keystroke replaces it.
      editor_.selectAll();
      return;
    }
    editing_ = false;
    editor_.setVisible(false);
    if (MouseRouter* r = routerFor(this))
      if (r->focused() == &editor_) r->setFocus(nullptr);
    repaint();
  }

  TextEditor editor_;
  std::string text_;
  Justify justify_ = Justify::Right;
  bool editable_ = true;
  bool editing_ = false;
};

} // namespace gfx
} // namespace sonore
