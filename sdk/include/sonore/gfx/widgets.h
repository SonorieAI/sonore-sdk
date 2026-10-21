// SPDX-License-Identifier: Apache-2.0
//
// Slider, Button, Label, ComboBox.
//
// ── They work in 0..1 and know nothing about parameters ─────────────────────
//
// A slider knows nothing about a processor parameter, and should not. A
// widget reports a normalised value and calls back; something else binds that
// to a plugin parameter, formats it and records the gesture.
//
// The layering is not decoration. A widget that reached into the parameter
// system could not be tested without one, and the interaction rules here --
// what a drag means, when a gesture starts -- are the part most worth testing
// and the part with nothing audio-specific in it.
//
// ── Gestures ────────────────────────────────────────────────────────────────
//
// onDragStart and onDragEnd exist because a host needs them. A drag is ONE
// automation edit and one undo step; without the brackets, a DAW records
// forty separate changes for one movement of a knob and the user's undo takes
// forty presses to get back.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "component.h"
#include "lookandfeel.h"

namespace sonore {
namespace gfx {

/**
 * What the mouse wheel does over a control.
 *
 * The interesting value is the default. A control claiming the wheel outright
 * is right in a fixed editor and wrong in a scrolling one: a user reaching for
 * a parameter further down scrolls, the pointer crosses a knob, and the next
 * notch moves that knob instead of the list. That is an accidental edit --
 * reported to the host as automation -- made while only navigating.
 *
 * Deferring outright is wrong too, and for a reason already written into this
 * file: a wheel WHILE DRAGGING a slider belongs to the slider, or the list
 * scrolls out from under the hand that is adjusting it.
 *
 * DeferToScroll is both. The wheel moves the control unless something above it
 * could scroll AND the control is not currently being dragged.
 */
enum class WheelMode {
  Never,         ///< this control never responds to the wheel
  Always,        ///< it always does, even inside a scrolling view
  DeferToScroll, ///< it does unless an ancestor would scroll and no drag is in progress
};

/** Shared plumbing: which LookAndFeel to draw with, and which font. */
class Widget : public Component {
public:
  Widget* asWidget() override { return this; }

  /** The off case is conventionally a single boolean; this is that plus the
   *  case that usually has no name. */
  void setWheelMode(WheelMode mode) { wheelMode_ = mode; }
  /** DeferToScroll unless setWheelMode said otherwise. */
  WheelMode wheelMode() const { return wheelMode_; }

  /** Whether this control should act on a wheel event right now. `dragging`
   *  is the control's own drag state -- a wheel mid-drag is always the
   *  control's, whatever is above it. */
  bool wheelBelongsHere(bool dragging) const {
    if (wheelMode_ == WheelMode::Never) return false;
    if (wheelMode_ == WheelMode::Always) return true;
    return dragging || !hasScrollableAncestor();
  }

  /**
   * Somebody right-clicked this control.
   *
   * On a parameter control this is how a user reaches the HOST's own menu --
   * MIDI learn, assign automation, reset to default -- which is where those
   * live in every DAW. The widget does not show the menu and must not: it is
   * the host's menu, and only the wrapper can ask for it.
   */
  std::function<void(const MouseEvent&)> onContextMenu;

  void contextMenu(const MouseEvent& e) override {
    if (isEnabled() && onContextMenu) onContextMenu(e);
  }

  /** ShowMenu, for every control: the same menu a right click asks for, as
   *  if the click had landed in the control's centre -- a host placing its
   *  menu at the event's position puts it over the control a reader is on.
   *  Offered only when somebody supplied the menu (see baseInfo). */
  bool performAccessibleAction(AccessibleAction action) override {
    if (action != AccessibleAction::ShowMenu || !isEnabled() || !onContextMenu) return false;
    MouseEvent e;
    e.position = {localBounds().w * 0.5f, localBounds().h * 0.5f};
    e.rootPosition = localToRoot(e.position);
    e.downPosition = e.position;
    e.rightButton = true;
    contextMenu(e);
    return true;
  }

  /** Draw this widget, and every widget inside it that sets none of its own,
   *  with `laf`; null inherits again. Not owned: it must outlive the widgets
   *  drawing with it. */
  void setLookAndFeel(LookAndFeel* laf) {
    laf_ = laf;
    repaint();
  }

  /** This widget's look, else the nearest ancestor widget's, else
   *  LookAndFeel::defaultLookAndFeel(). */
  LookAndFeel& lookAndFeel() const {
    if (laf_) return *laf_;
    // Inherited from the nearest ancestor that has one, so a panel can set
    // the look for everything inside it in a single call.
    for (Component* p = parent(); p; p = p->parent())
      if (Widget* w = p->asWidget())
        if (w->laf_) return *w->laf_;
    return LookAndFeel::defaultLookAndFeel();
  }

  /** Text in this widget, and in widgets inside it that set none, uses `f`;
   *  an invalid Font -- the default -- inherits. */
  void setFont(const Font& f) {
    font_ = f;
    repaint();
  }

  /** This widget's font if valid, else the nearest ancestor widget's. When none
   *  has one the Font is invalid and no text is drawn. */
  const Font& font() const {
    if (font_.isValid()) return font_;
    for (Component* p = parent(); p; p = p->parent())
      if (Widget* w = p->asWidget())
        if (w->font_.isValid()) return w->font_;
    return font_;
  }

  /** A disabled widget draws dimmed, ignores clicks, keys and the wheel, and
   *  lets the mouse through to whatever is behind it. Enabled by default. */
  void setEnabled(bool shouldBeEnabled) {
    if (enabled_ == shouldBeEnabled) return;
    enabled_ = shouldBeEnabled;
    setInterceptsMouse(shouldBeEnabled);
    repaint();
  }
  /** True unless setEnabled(false). */
  bool isEnabled() const { return enabled_; }

protected:
  /**
   * The parts of an accessible description every control answers the same way.
   *
   * A control that overrode accessibleInfo() from scratch would be one more
   * place for "is it enabled" to be answered wrongly, and the enabled flag is
   * exactly the one a reader must not get wrong -- announcing a control the
   * user cannot operate, with no hint of why, is worse than not announcing it.
   */
  AccessibleInfo baseInfo(AccessibleRole role) const {
    AccessibleInfo info;
    info.role = role;
    info.name = accessibleName();
    info.description = accessibleDescription();
    info.focusable = wantsKeyboardFocus();
    info.focused = hasKeyboardFocus();
    info.enabled = isEnabled();
    if (isEnabled() && onContextMenu) info.actions |= accessibleActionBit(AccessibleAction::ShowMenu);
    return info;
  }

  /** Draw the focus ring if this widget has the keyboard. Called at the END of
   *  a control's paint, so it lands on top of whatever the control drew. */
  void paintFocusRing(Graphics& g) const {
    if (hasKeyboardFocus()) lookAndFeel().drawFocusRing(g, localBounds());
  }

  /** The WidgetState a LookAndFeel call takes: hover and enabled from this
   *  widget, pressed and toggled from the caller. */
  WidgetState stateFor(bool pressed, bool toggled = false) const {
    WidgetState s;
    s.hovered = hovered_;
    s.pressed = pressed;
    s.toggled = toggled;
    s.enabled = enabled_;
    return s;
  }

  void mouseEnter(const MouseEvent&) override {
    hovered_ = true;
    repaint();
  }
  void mouseExit(const MouseEvent&) override {
    hovered_ = false;
    repaint();
  }

  /** Whether the pointer is over this widget, kept by Widget's mouseEnter and
   *  mouseExit: a subclass overriding those calls these versions too. */
  bool hovered_ = false;

private:
  LookAndFeel* laf_ = nullptr;
  Font font_;
  bool enabled_ = true;
  WheelMode wheelMode_ = WheelMode::DeferToScroll;
};

/**
 * A slider, rotary or linear.
 *
 * Value is normalised 0..1 throughout. A caller wanting decibels converts on
 * the way in and out -- the same curve the host's automation lane uses, so
 * the knob and the lane agree about where halfway is.
 */
class Slider : public Widget {
public:
  enum class Style {
    Rotary,  ///< a knob; dragged vertically unless setRotaryDrag says otherwise
    LinearHorizontal,  ///< a horizontal track with a thumb: 0 at the left
    LinearVertical,  ///< a vertical track with a thumb: 1 at the top
    /** A filled proportion rather than a thumb. */
    LinearBar,
    LinearBarVertical,  ///< the same, filling upwards
  };

  /**
   * Which way a ROTARY reads a drag.
   *
   * Vertical is the default and stays the default: it is what almost every
   * plugin does and what the hand expects from a knob.
   *
   * Both exists because a purely horizontal drag on a vertical-only knob does
   * NOTHING, and that is the version users meet -- people drag diagonally
   * without thinking about it, and a knob that ignores half the gesture reads
   * as broken rather than as opinionated. Summing the two axes means any
   * direction with an upward or rightward component turns it up.
   */
  enum class RotaryDrag { Vertical, /**< up turns it up */ Horizontal, /**< right turns it up */ Both /**< up and right both count, summed */ };

  /** How a Rotary reads a drag; the linear styles ignore it. The cursor
   *  follows -- left-right for Horizontal, up-down otherwise -- unless a
   *  cursor was set on this slider, which is kept. */
  void setRotaryDrag(RotaryDrag d) {
    rotaryDrag_ = d;
    followDragCursor();
  }
  /** Vertical unless setRotaryDrag said otherwise. */
  RotaryDrag rotaryDrag() const { return rotaryDrag_; }

  /** The three questions the drawing and the drag arithmetic both ask, spelled
   *  once. They carry the names these shapes are known by. */
  bool isRotary() const { return style_ == Style::Rotary; }
  /** LinearBar or LinearBarVertical: a fill with no thumb. */
  bool isBar() const { return style_ == Style::LinearBar || style_ == Style::LinearBarVertical; }
  /** LinearHorizontal or LinearBar: the value runs left to right. */
  bool isHorizontal() const {
    return style_ == Style::LinearHorizontal || style_ == Style::LinearBar;
  }

  /** A slider of style `s` at 0, with a default of 0. Takes keyboard focus. */
  explicit Slider(Style s = Style::Rotary) : style_(s) {
    // Reachable by Tab, and therefore operable. Nothing but the text field and
    // the list took focus before this, so every knob in every generated editor
    // was invisible to the keyboard.
    setWantsKeyboardFocus(true);
    // A rotary is dragged VERTICALLY however round it looks, so its cursor says
    // so -- which is the only hint a new user gets that a knob is not turned by
    // dragging in a circle. Asked the way the drag itself asks: testing for
    // LinearHorizontal alone gave the horizontal BAR the up-and-down cursor
    // while it moved left and right.
    ownCursor_ = dragCursor();
    setCursor(ownCursor_);
  }

  /** Change the shape; the value is kept. The cursor follows -- left-right
   *  for LinearHorizontal, LinearBar and a rotary dragged Horizontal, up-down
   *  for the rest -- unless a cursor was set on this slider, which is kept. */
  void setStyle(Style s) {
    style_ = s;
    followDragCursor();
    repaint();
  }
  /** The shape it is drawn as. */
  Style style() const { return style_; }

  /** Normalised, 0..1. */
  float value() const { return value_; }

  /** `notify` false is for a host telling the widget what the parameter now
   *  is: echoing that back as a change would be a loop, and in a DAW a very
   *  fast one. */
  void setValue(float normalised, bool notify = true) {
    const float v = normalised < 0.0f ? 0.0f : (normalised > 1.0f ? 1.0f : normalised);
    if (v == value_) return;
    value_ = v;
    repaint();
    if (notify && onValueChange) onValueChange(value_);
  }

  /** Where a double click puts it, normalised; 0 unless set. */
  void setDefaultValue(float normalised) { defaultValue_ = normalised; }

  /** How many pixels of drag cover the whole range. 200 is roughly a hand's
   *  travel and is what most plugins use; a shorter distance makes a knob
   *  impossible to set. */
  void setDragSensitivity(float pixelsForFullRange) {
    if (pixelsForFullRange > 1.0f) sensitivity_ = pixelsForFullRange;
  }

  /** The new normalised value, each time it changes -- by a drag, a key, the
   *  wheel, a double click, a reader, or setValue with `notify`. Not called
   *  when a set leaves the value where it was. The handler may delete the
   *  slider. */
  std::function<void(float)> onValueChange;
  /** Opens a gesture: when a drag begins, and before each one-step edit (a key,
   *  a wheel notch, a double click, a reader's set). Where an attachment begins
   *  the host's edit. */
  std::function<void()> onDragStart;
  /** Closes it: when the drag's button comes up, and after each one-step edit.
   *  A slider removed from the tree or destroyed mid-drag gets no release, so
   *  its owner closes that gesture (SliderAttachment does, when destroyed). */
  std::function<void()> onDragEnd;

  /**
   * The human-readable value -- "-6.0 dB" -- for anything that has to SAY what
   * this slider reads.
   *
   * A Slider holds a normalised 0..1 and nothing else; the plain value, its
   * unit and its formatting live in the parameter, which the slider has never
   * heard of. So whatever binds the two sets this, from the SAME text it puts
   * in the readout -- because a screen reader announcing a different number
   * from the displayed one is worse than one announcing nothing.
   */
  void setAccessibleValueText(std::string text) { valueText_ = std::move(text); }

  AccessibleInfo accessibleInfo() const override {
    AccessibleInfo info = baseInfo(AccessibleRole::Slider);
    // The normalised range, not the parameter's. A bridge offering the
    // platform's range interface wants numbers it can set back, and 0..1 is
    // the only thing this control can accept.
    info.hasRange = true;
    info.minValue = 0.0;
    info.maxValue = 1.0;
    info.currentValue = (double) value_;
    // The formatted text when somebody supplied one, and a percentage
    // otherwise -- which is not useful but is honest, where an empty string
    // would be a control that reads as having no value at all.
    if (!valueText_.empty()) {
      info.value = valueText_;
    } else {
      char buffer[32];
      std::snprintf(buffer, sizeof(buffer), "%.0f%%", (double) value_ * 100.0);
      info.value = buffer;
    }
    if (isEnabled()) {
      info.actions |= accessibleActionBit(AccessibleAction::Increment) |
                      accessibleActionBit(AccessibleAction::Decrement);
      info.valueSettable = true;
    }
    return info;
  }

  /** Increment and Decrement are the arrow keys' step, through keyPressed --
   *  one bracketed edit each, exactly as a keyboard user makes them. */
  bool performAccessibleAction(AccessibleAction action) override {
    if (action != AccessibleAction::Increment && action != AccessibleAction::Decrement)
      return Widget::performAccessibleAction(action);
    KeyPress key;
    key.keyCode = action == AccessibleAction::Increment ? KeyPress::Up : KeyPress::Down;
    return keyPressed(key);
  }

  /** A reader setting the position outright: one bracketed edit, as a
   *  double click to the default is. */
  bool setAccessibleValue(double value) override {
    if (!isEnabled() || !(value == value)) return false; // NaN is no position
    edit((float) value);
    return true;
  }

  void paint(Graphics& g) override {
    if (style_ == Style::Rotary)
      lookAndFeel().drawRotarySlider(g, localBounds(), value_, stateFor(dragging_));
    else if (isBar())
      lookAndFeel().drawLinearBar(g, localBounds(), value_, !isHorizontal(),
                                  stateFor(dragging_));
    else
      lookAndFeel().drawLinearSlider(g, localBounds(), value_, style_ == Style::LinearVertical,
                                     stateFor(dragging_));
    paintFocusRing(g);
  }

  /**
   * Arrow keys, page keys, Home and End.
   *
   * Until this existed a generated plugin's editor could not be operated from
   * the keyboard AT ALL: nothing but the text field and the list took focus, so
   * Tab walked past every control and the arrow keys did nothing. That is a
   * usability gap for anyone whose hands are on the keyboard and a total
   * barrier for anyone who cannot use a mouse.
   *
   * Shift is fine adjustment, at a tenth of the step -- the keyboard equivalent
   * of the modifier a drag already has, and the only way to reach a value a 1%
   * step steps over.
   *
   * ONE GESTURE PER KEYSTROKE, which is deliberate and has a cost worth
   * stating: holding an arrow key records a run of small edits rather than one.
   * The alternative is to open a gesture on the first key and close it on a
   * timeout, which needs a clock this component does not have and would leave
   * a gesture open if the editor closed mid-press. A verbose undo history is
   * the better failure.
   */
  bool keyPressed(const KeyPress& key) override {
    if (!isEnabled()) return false;
    const float fine = key.shiftDown ? 0.1f : 1.0f;
    const float step = 0.01f * fine;
    const float page = 0.10f * fine;

    float target = value_;
    if (key.is(KeyPress::Right) || key.is(KeyPress::Up)) target = value_ + step;
    else if (key.is(KeyPress::Left) || key.is(KeyPress::Down)) target = value_ - step;
    else if (key.is(KeyPress::PageUp)) target = value_ + page;
    else if (key.is(KeyPress::PageDown)) target = value_ - page;
    else if (key.is(KeyPress::Home)) target = 0.0f;
    else if (key.is(KeyPress::End)) target = 1.0f;
    else return false;

    target = target < 0.0f ? 0.0f : (target > 1.0f ? 1.0f : target);
    // Bracketed even when the value does not move -- at the end of the range an
    // unbracketed set would be the only path here that is not an edit, and a
    // host counting gestures would see them unbalanced.
    edit(target);
    return true;
  }

  void mouseDown(const MouseEvent& e) override {
    if (!isEnabled()) return;
    // A double click returns to the default, and must NOT also start a drag:
    // a gesture bracket opened here and closed by the following release would
    // record an edit the user did not make.
    if (e.clickCount >= 2) {
      edit(defaultValue_);
      return;
    }
    dragging_ = true;
    valueAtDown_ = value_;
    // Repainted first: the handlers below may delete this slider (see edit).
    repaint();
    detail::DeletionWatch alive(this);
    if (onDragStart) onDragStart();
    if (alive.deleted) return;
    // A linear slider JUMPS to where it was clicked, because the track shows
    // an absolute position and clicking a place you can see should go there.
    // A rotary does not: there is no position under the pointer to mean.
    if (style_ != Style::Rotary) {
      const float jumped = valueFromPosition(e.position);
      valueAtDown_ = jumped < 0.0f ? 0.0f : (jumped > 1.0f ? 1.0f : jumped);
      dragOrigin_ = e.position;
      setValue(jumped);
    }
  }

  void mouseDrag(const MouseEvent& e) override {
    if (!isEnabled() || !dragging_) return;
    // Shift is FINE mode. A quarter speed keeps a 200-pixel drag usable for
    // the last few percent of a filter cutoff.
    const float speed = e.shiftDown ? 0.25f : 1.0f;
    if (style_ == Style::Rotary) {
      // Up is positive, and so is right. Summing both axes for Both means a
      // diagonal drag works, which is the gesture people actually make.
      const float dy = -(e.position.y - e.downPosition.y);
      const float dx = e.position.x - e.downPosition.x;
      float travel = 0.0f;
      switch (rotaryDrag_) {
        case RotaryDrag::Vertical: travel = dy; break;
        case RotaryDrag::Horizontal: travel = dx; break;
        case RotaryDrag::Both: travel = dx + dy; break;
      }
      setValue(valueAtDown_ + (travel / sensitivity_) * speed);
    } else if (e.shiftDown) {
      const float dx = isHorizontal() ? e.position.x - dragOrigin_.x
                                      : -(e.position.y - dragOrigin_.y);
      const float span = isHorizontal() ? localBounds().w : localBounds().h;
      setValue(valueAtDown_ + (dx / (span > 1.0f ? span : 1.0f)) * speed);
    } else {
      setValue(valueFromPosition(e.position));
    }
  }

  void mouseUp(const MouseEvent&) override {
    if (!dragging_) return;
    dragging_ = false;
    // Repainted BEFORE the handler, which may delete this slider: an editor
    // rebuilt once a drag ends.
    repaint();
    if (onDragEnd) onDragEnd();
  }

  /** A wheel notch is a small step, and brackets itself: it is a complete
   *  edit rather than part of a longer gesture. */
  bool mouseWheel(const MouseEvent& e, float delta) override {
    if (!isEnabled() || delta == 0.0f) return false;
    // Declined when something above would scroll and this slider is not being
    // dragged -- see WheelMode. Mid-drag it is always claimed, which is the
    // original rule this comment used to state outright: the list must not
    // scroll out from under the hand adjusting the control.
    if (!wheelBelongsHere(dragging_)) return false;
    const float step = (e.shiftDown ? 0.01f : 0.05f) * (delta > 0.0f ? 1.0f : -1.0f);
    edit(value_ + step);
    return true;
  }

private:
  /**
   * One complete edit: onDragStart, the value, onDragEnd -- what a key, a
   * wheel notch, a double click and a reader setting the value each make.
   *
   * All three handlers are the owner's code, and onValueChange may delete
   * this slider: an editor that lays itself out again around the value just
   * set (a band count, a mode) rebuilds the control that set it. Each is
   * read only from a slider that survived the one before -- a deleted one's
   * handlers went with it.
   */
  void edit(float target) {
    detail::DeletionWatch alive(this);
    if (onDragStart) onDragStart();
    if (alive.deleted) return;
    setValue(target);
    if (alive.deleted) return;
    if (onDragEnd) onDragEnd();
  }

  float valueFromPosition(Point p) const {
    const Rect b = localBounds();
    // A BAR has no thumb, so nothing is shortened and nothing is offset: the
    // fill spans the whole rectangle, which is exactly what makes a row of
    // them comparable by eye. Using the thumb arithmetic here would make a bar
    // reach 1.0 seven pixels before its own right edge.
    const float knob = isBar() ? 0.0f : 14.0f;
    if (!isHorizontal()) {
      const float span = b.h - knob;
      if (span <= 0.0f) return value_;
      // Upward: the top of a fader is 1.
      return 1.0f - (p.y - knob * 0.5f) / span;
    }
    const float span = b.w - knob;
    if (span <= 0.0f) return value_;
    return (p.x - knob * 0.5f) / span;
  }

  // The cursor this slider's drag calls for: left-right where a drag reads
  // left-right, which is what valueFromPosition and the rotary arithmetic
  // ask, and up-down everywhere else, Both included.
  MouseCursor dragCursor() const {
    const bool leftRight = isRotary() ? rotaryDrag_ == RotaryDrag::Horizontal : isHorizontal();
    return leftRight ? MouseCursor::DragHorizontal : MouseCursor::DragVertical;
  }

  // Moves the cursor to what the drag now calls for, but only while it is
  // still the one this slider gave itself. Anything else was set on it, and
  // a cursor a plugin chose is never replaced -- setStyle used to replace
  // it. Told apart by value, since setCursor is Component's and not ours to
  // watch: a plugin that sets the very cursor the slider already had cannot
  // be told from one that set nothing, and it is followed like the default.
  void followDragCursor() {
    if (Component::cursor() != ownCursor_) return;
    ownCursor_ = dragCursor();
    setCursor(ownCursor_);
  }

  Style style_;
  RotaryDrag rotaryDrag_ = RotaryDrag::Vertical;
  // The last cursor followDragCursor or the constructor gave this slider.
  MouseCursor ownCursor_ = MouseCursor::DragVertical;
  float value_ = 0.0f, defaultValue_ = 0.0f, valueAtDown_ = 0.0f;
  std::string valueText_;
  float sensitivity_ = 200.0f;
  Point dragOrigin_;
  bool dragging_ = false;
};

/** A push button, or a toggle when setToggleable(true). */
class Button : public Widget {
public:
  /** A push button showing `label`. Takes keyboard focus. */
  explicit Button(std::string label = {}) : label_(std::move(label)) {
    setWantsKeyboardFocus(true);
  }

  /** Repaints only when the text changes. A toggle's attachment sets the
   *  label on every 33 ms sync; repainting each time kept an editor with a
   *  switch from ever going idle. */
  void setLabel(std::string t) {
    if (t == label_) return;
    label_ = std::move(t);
    repaint();
  }
  /** The text drawn on it. */
  const std::string& label() const { return label_; }

  /** Make it a toggle: each press flips isToggled, calling onToggle, before
   *  onClick. */
  void setToggleable(bool shouldToggle) { toggleable_ = shouldToggle; }
  /** Whether presses flip it. */
  bool isToggleable() const { return toggleable_; }

  /** The toggle's state. */
  bool isToggled() const { return toggled_; }
  /** Set the state directly: repaints, and calls onToggle when it changed and
   *  `notify` is true. `notify` false is for a host's echo, as for
   *  Slider::setValue. */
  void setToggled(bool on, bool notify = true) {
    if (toggled_ == on) return;
    toggled_ = on;
    repaint();
    if (notify && onToggle) onToggle(toggled_);
  }

  /** A press: a click released INSIDE the button, Space, Return or a reader's
   *  Press -- after onToggle, for a toggle. The handler may delete the button. */
  std::function<void()> onClick;
  /** A toggle's new state, when a press or setToggled with `notify` flips it. */
  std::function<void(bool)> onToggle;

  /** What this toggle's current state is CALLED, for a reader. Set from the
   *  same string the button draws, so the two cannot disagree. */
  void setAccessibleValueText(std::string text) { stateText_ = std::move(text); }

  AccessibleInfo accessibleInfo() const override {
    AccessibleInfo info =
        baseInfo(toggleable_ ? AccessibleRole::ToggleButton : AccessibleRole::Button);
    // The label is the name when nobody set one. A button reading "button" is
    // a button nobody can find.
    if (info.name.empty()) info.name = label_;
    // Only a toggle has a value. A plain button reading "off" would say it was
    // in a state it does not have.
    //
    // The STATE'S OWN NAME when whatever bound this supplied one. A parameter
    // whose two positions are "Pre" and "Post" announced as "on" makes the
    // listener work out which of the two that is, and they cannot -- the
    // mapping is the plugin's, not a convention.
    if (toggleable_) {
      info.value = !stateText_.empty() ? stateText_ : (toggled_ ? "on" : "off");
      info.toggle = toggled_ ? AccessibleToggle::On : AccessibleToggle::Off;
    }
    if (isEnabled()) info.actions |= accessibleActionBit(AccessibleAction::Press);
    return info;
  }

  /** Press is Return, through keyPressed: the same path as a click, so a
   *  toggle toggles and onClick fires exactly once. */
  bool performAccessibleAction(AccessibleAction action) override {
    if (action != AccessibleAction::Press) return Widget::performAccessibleAction(action);
    KeyPress key;
    key.keyCode = KeyPress::Return;
    return keyPressed(key);
  }

  void paint(Graphics& g) override {
    lookAndFeel().drawButton(g, localBounds(), label_, font(), stateFor(pressed_, toggled_));
    paintFocusRing(g);
  }

  /** Space or Return, which is what every toolkit uses and what a user who has
   *  just tabbed to a button will try first. Goes through the SAME path as a
   *  click, so a toggle toggles and onClick fires exactly once either way. */
  bool keyPressed(const KeyPress& key) override {
    if (!isEnabled()) return false;
    if (!key.is(KeyPress::Return) && key.character != (uint32_t) ' ') return false;
    press();
    return true;
  }

  void mouseDown(const MouseEvent&) override {
    if (!isEnabled()) return;
    pressed_ = true;
    repaint();
  }

  void mouseDrag(const MouseEvent& e) override {
    if (!isEnabled()) return;
    // The pressed look follows the pointer back inside and out, which is how
    // a user cancels a press they changed their mind about.
    const bool inside = localBounds().contains(e.position);
    if (inside != pressed_) {
      pressed_ = inside;
      repaint();
    }
  }

  void mouseUp(const MouseEvent& e) override {
    if (!isEnabled()) return;
    const bool wasPressed = pressed_;
    pressed_ = false;
    repaint();
    // Only when the release happened INSIDE. Releasing outside is a cancel,
    // and a button that fired anyway would leave a user no way to back out.
    if (!wasPressed || !localBounds().contains(e.position)) return;
    press();
  }

private:
  /** A click or a key: toggle, then onClick. onToggle is the owner's code and
   *  may delete this button -- a switch that swaps the panel it sits in for
   *  another -- so onClick is read only from a button that survived it. */
  void press() {
    if (toggleable_) {
      detail::DeletionWatch alive(this);
      setToggled(!toggled_);
      if (alive.deleted) return;
    }
    if (onClick) onClick();
  }

  std::string label_;
  bool toggleable_ = false, toggled_ = false, pressed_ = false;
  std::string stateText_;
};

/** Text. Does not take the mouse, so a click passes to whatever is behind. */
class Label : public Widget {
public:
  /** Shows `text`, placed across its width by `j`. */
  explicit Label(std::string text = {}, Justify j = Justify::Left)
      : text_(std::move(text)), justify_(j) {
    setInterceptsMouse(false);
  }

  /** Repaints only when the text changes. */
  void setText(std::string t) {
    if (t == text_) return;
    text_ = std::move(t);
    repaint();
  }
  /** What it shows, also its accessible name when none was set. */
  const std::string& text() const { return text_; }

  /** Where the text sits across the label's width. */
  void setJustify(Justify j) {
    justify_ = j;
    repaint();
  }

  void paint(Graphics& g) override {
    lookAndFeel().drawLabel(g, localBounds(), text_, font(), justify_, stateFor(false));
  }

  AccessibleInfo accessibleInfo() const override {
    AccessibleInfo info = baseInfo(AccessibleRole::Label);
    if (info.name.empty()) info.name = text_;
    return info;
  }

private:
  std::string text_;
  Justify justify_;
};

/**
 * A choice from a fixed list.
 *
 * Cycles on click rather than opening a popup. A popup is a MODAL window --
 * it has to sit above everything, take the mouse until dismissed, and close
 * on a click elsewhere -- and none of that exists yet. Cycling is honest and
 * usable for the three or four choices a plugin parameter has; the popup is
 * named as a gap rather than half-built.
 */
class ComboBox : public Widget {
public:
  /** No items, so nothing selected (index -1). Takes keyboard focus. */
  ComboBox() { setWantsKeyboardFocus(true); }

  /** Replace the choices. The selection is kept while still in range, else it
   *  is the first item, or -1 with none. onChange is not called. */
  void setItems(std::vector<std::string> items) {
    items_ = std::move(items);
    // BOTH ends. A fresh box has selected_ == -1, and testing only the upper
    // bound left it there -- so setItems on a new ComboBox selected nothing
    // and the control drew empty. Found by the first test that asked what it
    // showed.
    if (items_.empty()) selected_ = -1;
    else if (selected_ < 0 || selected_ >= (int) items_.size()) selected_ = 0;
    repaint();
  }

  /** 0-based, or -1 when there are no items. */
  int selectedIndex() const { return selected_; }

  /** Select `index`, clamped into range; ignored with no items. Calls onChange
   *  when the selection moved and `notify` is true. */
  void setSelectedIndex(int index, bool notify = true) {
    if (items_.empty()) return;
    const int clamped = index < 0 ? 0 : (index >= (int) items_.size() ? (int) items_.size() - 1
                                                                      : index);
    if (clamped == selected_) return;
    selected_ = clamped;
    repaint();
    if (notify && onChange) onChange(selected_);
  }

  /** The selected item, or empty with none. */
  std::string selectedText() const {
    if (selected_ < 0 || selected_ >= (int) items_.size()) return {};
    return items_[(size_t) selected_];
  }

  /** The new 0-based index, after any change: a click or the wheel cycling, a
   *  key, the menu, a reader, or setSelectedIndex with `notify`. */
  std::function<void(int)> onChange;

  void paint(Graphics& g) override {
    lookAndFeel().drawComboBox(g, localBounds(), selectedText(), font(), stateFor(pressed_));
    paintFocusRing(g);
  }

  /**
   * Arrows move the SELECTION rather than opening the list.
   *
   * Which is the opposite of a desktop combo box and right for this one: this
   * box cycles on click and its menu is supplied by whatever owns the window,
   * so a key that opened it would do nothing wherever nobody supplied one --
   * in a test, a headless render, or a build with no window backend. Changing
   * the selection works everywhere and is what a parameter needs.
   */
  bool keyPressed(const KeyPress& key) override {
    if (!isEnabled() || items_.empty()) return false;
    if (key.is(KeyPress::Down) || key.is(KeyPress::Right))
      setSelectedIndex(selected_ + 1);
    else if (key.is(KeyPress::Up) || key.is(KeyPress::Left))
      setSelectedIndex(selected_ - 1);
    else if (key.is(KeyPress::Home))
      setSelectedIndex(0);
    else if (key.is(KeyPress::End))
      setSelectedIndex((int) items_.size() - 1);
    else
      return false;
    return true;
  }

  void mouseDown(const MouseEvent&) override {
    if (!isEnabled()) return;
    pressed_ = true;
    repaint();
  }

  /**
   * Ask for a drop-down, if anyone is listening.
   *
   * The ComboBox does not open the menu itself, and cannot: a popup is a
   * top-level window, and a widget knows nothing about windows. Whatever owns
   * the editor supplies this, gets the item list and the box's position, and
   * calls back with the chosen index.
   *
   * When nobody supplies it -- a test, a headless render, a build with no
   * window backend -- the box falls back to cycling, which is what it did
   * before popups existed. A control that did nothing at all would be worse.
   */
  std::function<void(ComboBox&)> onOpenMenu;

  AccessibleInfo accessibleInfo() const override {
    AccessibleInfo info = baseInfo(AccessibleRole::ComboBox);
    info.value = selectedText();
    // A range as well as a value, because a choice IS a range with steps and a
    // reader that knows "3 of 5" can say so. The value text stays the item's
    // name -- announcing "2" instead of "Saw" would be the number nobody
    // wanted.
    if (!items_.empty()) {
      info.hasRange = true;
      info.minValue = 0.0;
      info.maxValue = (double) items_.size() - 1.0;
      info.currentValue = (double) selected_;
      if (isEnabled()) {
        info.actions |= accessibleActionBit(AccessibleAction::Press) |
                        accessibleActionBit(AccessibleAction::Increment) |
                        accessibleActionBit(AccessibleAction::Decrement);
        info.valueSettable = true;
      }
    }
    return info;
  }

  /** Press does what a click does -- the menu when somebody supplies one,
   *  the next item when nobody does. Increment is the NEXT item, which is
   *  Down here (the list reads downwards), not Up as on a slider. */
  bool performAccessibleAction(AccessibleAction action) override {
    if (!isEnabled() || items_.empty()) return Widget::performAccessibleAction(action);
    switch (action) {
      case AccessibleAction::Press:
        if (onOpenMenu) onOpenMenu(*this);
        else setSelectedIndex((selected_ + 1) % (int) items_.size());
        return true;
      case AccessibleAction::Increment:
      case AccessibleAction::Decrement: {
        KeyPress key;
        key.keyCode = action == AccessibleAction::Increment ? KeyPress::Down : KeyPress::Up;
        return keyPressed(key);
      }
      default:
        return Widget::performAccessibleAction(action);
    }
  }

  /** The item at that index, rounded: a reader's value is the range above. */
  bool setAccessibleValue(double value) override {
    if (!isEnabled() || items_.empty() || !(value == value)) return false;
    setSelectedIndex((int) (value < 0.0 ? 0.0 : value + 0.5));
    return true;
  }

  /** The item with that NAME, which is what a reader that sets a pop-up
   *  button's value by its title sends. Unknown names change nothing. */
  bool setAccessibleText(const std::string& text) override {
    if (!isEnabled()) return false;
    for (size_t i = 0; i < items_.size(); ++i)
      if (items_[i] == text) {
        setSelectedIndex((int) i);
        return true;
      }
    return false;
  }

  void mouseUp(const MouseEvent& e) override {
    if (!isEnabled()) return;
    pressed_ = false;
    repaint();
    if (!localBounds().contains(e.position) || items_.empty()) return;
    if (onOpenMenu) {
      onOpenMenu(*this);
      return;
    }
    setSelectedIndex((selected_ + 1) % (int) items_.size());
  }

  /** The choices, in order. */
  const std::vector<std::string>& items() const { return items_; }

  /** A notch steps to the next or previous item, wrapping -- unless the
   *  wheel mode says it is not this box's: see WheelMode. */
  bool mouseWheel(const MouseEvent&, float delta) override {
    if (!isEnabled() || items_.empty() || delta == 0.0f) return false;
    // The slider's rule, which this box never asked: inside a scrolling view
    // a notch over it changed the selection instead of scrolling the list.
    // There is no drag on a combo box, so nothing here overrides the view.
    if (!wheelBelongsHere(false)) return false;
    const int n = (int) items_.size();
    setSelectedIndex(((selected_ + (delta > 0.0f ? 1 : -1)) % n + n) % n);
    return true;
  }

private:
  std::vector<std::string> items_;
  int selected_ = -1;
  bool pressed_ = false;
};

} // namespace gfx
} // namespace sonore
