// SPDX-License-Identifier: Apache-2.0
//
// Binding the native UI to plugin parameters, and the editor a plugin gets
// when it says nothing about its own.
//
// ── Why the widgets did not do this themselves ──────────────────────────────
//
// A Slider knows what a drag means and nothing about audio. This file is
// where the two meet: the same reason a parameter attachment is kept separate
// from the slider it drives.
//
// The split is what makes both testable: the widget's rules can be checked
// with no plugin, and the binding's rules -- which curve, which gesture, which
// direction the value flows -- can be checked with no window.
//
// ── The loop this has to avoid ──────────────────────────────────────────────
//
// A host changes a parameter, the editor updates its knob, the knob reports a
// change, the plugin sets the parameter, the host notices... In a DAW with
// automation running that loop closes thirty times a second and the parameter
// never settles. Everything arriving FROM the host uses setValue(notify=false),
// and that is not an optimisation.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../plugin.h"
#include "concertina.h"
#include "layout.h"
#include "midi_keyboard.h"
#include "value_box.h"
#include "widgets.h"

namespace sonore {
namespace gfx {

/**
 * What an editor needs from whatever is hosting it.
 *
 * Function objects rather than an interface to implement, so the CLAP, VST3,
 * AU and standalone paths each supply four lambdas rather than each growing a
 * subclass -- and so a test can supply four lambdas of its own.
 */
struct EditorHost {
  /** The parameter's PLAIN value, in its own units. */
  std::function<float(int index)> getParameter;
  std::function<void(int index, float plain)> setParameter;
  /** A drag is one automation edit and one undo step. */
  std::function<void(int index)> beginGesture;
  std::function<void(int index)> endGesture;

  /**
   * Notes from an on-screen keyboard. Velocity is 0..1.
   *
   * Left EMPTY by an effect, and that is how the editor decides whether to show
   * a keyboard at all: a wrapper sets these only for an instrument, so nothing
   * has to pass an "is this a synth" flag down three layers to arrive at the
   * same answer the descriptor already had.
   */
  std::function<void(int note, float velocity)> noteOn;
  std::function<void(int note)> noteOff;

  /**
   * The user right-clicked parameter `index`. Ask the HOST for its menu.
   *
   * Not our menu. Right-clicking a knob in any DAW opens the host's own list --
   * MIDI learn, assign automation, remove automation, reset to default -- and
   * every format has a call for it: clap_host_context_menu, VST3's
   * IComponentHandler3. Only the wrapper can reach those, so the editor asks
   * and the wrapper answers.
   *
   * Left empty by a host that offers nothing, and then a right-click does
   * nothing -- which is the honest outcome. Drawing our own menu of things the
   * host would have offered would be inventing a list we cannot fulfil.
   *
   * Coordinates are relative to the plugin's own window, which is what the
   * extensions ask for.
   */
  std::function<void(int index, int x, int y)> showContextMenu;
};

/**
 * Binds one Slider to one parameter.
 *
 * Conversion goes through toNormalisedValue/toPlainValue -- the SAME curve the
 * host's automation lane uses. A knob with its own curve would sit at a
 * different place from the lane drawing the same value, and a user would be
 * right to call that a bug.
 */
class SliderAttachment {
public:
  SliderAttachment(Slider& slider, const ParamInfo& info, int index, EditorHost host)
      : slider_(slider), info_(info), index_(index), host_(std::move(host)) {
    slider_.setDefaultValue((float) toNormalisedValue(info_, info_.defaultValue));

    slider_.onDragStart = [this]() {
      if (host_.beginGesture) host_.beginGesture(index_);
    };
    slider_.onDragEnd = [this]() {
      if (host_.endGesture) host_.endGesture(index_);
    };
    slider_.onValueChange = [this](float normalised) {
      if (host_.setParameter) host_.setParameter(index_, (float) toPlainValue(info_, normalised));
      if (onDisplayChanged) onDisplayChanged(displayText());
    };
    sync();
  }

  /** Pull the current value from the host. Called on the UI tick, so a knob
   *  follows automation and a preset load. */
  void sync() {
    if (!host_.getParameter) return;
    const float plain = host_.getParameter(index_);
    slider_.setValue((float) toNormalisedValue(info_, plain), /*notify=*/false);
    if (onDisplayChanged) onDisplayChanged(displayText());
  }

  /** The same text the host's generic panel shows, from the same function.
   *  Two readouts disagreeing about one parameter is the kind of thing a user
   *  reports as "the plugin is wrong". */
  std::string displayText() const {
    char text[64];
    formatParamValue(info_, (float) toPlainValue(info_, slider_.value()), text, sizeof(text));
    return text;
  }

  /**
   * A value somebody TYPED. False when it is not a number this parameter can
   * use, so the box can keep the field open rather than reverting.
   *
   * Through parseParamValue, which is the same function the host's own "enter
   * a value" box goes through. Two parsers for one parameter is two places for
   * "2.3 kHz" to mean different things.
   *
   * Wrapped in a gesture, because that is what makes it ONE automation edit and
   * ONE undo step. A bare setParameter with no begin/end around it is a write
   * the host cannot group: the DAW records a point rather than an edit, and
   * undo does not take it back.
   */
  bool setFromText(const std::string& typed) {
    float plain = 0.0f;
    if (!parseParamValue(info_, typed.c_str(), &plain)) return false;
    if (host_.beginGesture) host_.beginGesture(index_);
    if (host_.setParameter) host_.setParameter(index_, plain);
    if (host_.endGesture) host_.endGesture(index_);
    // The knob follows the number, not the other way round -- and without
    // notifying, or it would set the parameter a second time from the value it
    // was just given.
    slider_.setValue((float) toNormalisedValue(info_, plain), /*notify=*/false);
    if (onDisplayChanged) onDisplayChanged(displayText());
    return true;
  }

  std::function<void(const std::string&)> onDisplayChanged;

private:
  Slider& slider_;
  const ParamInfo& info_;
  int index_;
  EditorHost host_;
};

/**
 * The same, for a two-state parameter drawn as a toggle.
 *
 * A parameter with exactly two positions is on or off, and a drop-down with two
 * entries is the wrong control for it: two clicks and a moment's reading to do
 * what one click should, and nothing legible at a glance across a row of them.
 * Bypass, invert, sync, mute -- these are switches.
 *
 * The button carries the value NAME rather than a fixed label where the table
 * gives one, so a parameter whose states are "Pre" and "Post" says which it is
 * rather than glowing or not glowing and leaving the user to guess which way
 * round it goes.
 */
class ButtonAttachment {
public:
  ButtonAttachment(Button& button, const ParamInfo& info, int index, EditorHost host)
      : button_(button), info_(info), index_(index), host_(std::move(host)) {
    button_.setToggleable(true);
    button_.setAccessibleName(info_.label);
    button_.onToggle = [this](bool on) {
      // One complete edit, opened and closed around the change -- the same
      // rule a choice follows, and for the same reason: a switch is not a drag
      // and must not leave a gesture open behind it.
      if (host_.beginGesture) host_.beginGesture(index_);
      if (host_.setParameter) host_.setParameter(index_, on ? info_.maxValue : info_.minValue);
      if (host_.endGesture) host_.endGesture(index_);
      updateLabel();
    };
    sync();
  }

  void sync() {
    if (!host_.getParameter) return;
    const float plain = host_.getParameter(index_);
    // The MIDPOINT, not equality with maxValue. A host that automates this
    // parameter sends a continuous ramp through it, and a switch that only
    // turned on at exactly 1.0 would flicker off for every frame of the ramp.
    const float middle = (info_.minValue + info_.maxValue) * 0.5f;
    button_.setToggled(plain >= middle, /*notify=*/false);
    updateLabel();
  }

private:
  void updateLabel() {
    const char* name = paramValueName(info_, button_.isToggled() ? 1 : 0);
    if (name && name[0]) {
      button_.setLabel(name);
      // The reader hears the STATE, from the same string that is drawn. A
      // toggle announced only as pressed or not makes the user work out which
      // of two named states that means.
      button_.setAccessibleValueText(name);
    }
  }

  Button& button_;
  const ParamInfo& info_;
  int index_;
  EditorHost host_;
};

/** The same, for a stepped parameter drawn as a list of named choices. */
class ComboAttachment {
public:
  ComboAttachment(ComboBox& box, const ParamInfo& info, int index, EditorHost host)
      : box_(box), info_(info), index_(index), host_(std::move(host)) {
    std::vector<std::string> items;
    for (int i = 0; i < info_.stepCount; ++i) {
      // valueNames is a BARE POINTER with its own count, and bounds-checking
      // against stepCount instead reads past the end of any table with fewer
      // entries than the control has steps.
      if (info_.valueNames && i < info_.numValueNames) items.push_back(info_.valueNames[i]);
      else items.push_back(std::to_string(i));
    }
    box_.setItems(std::move(items));
    box_.onChange = [this](int selected) {
      // A choice is a complete edit: opened and closed around one change,
      // never left open the way a drag is.
      if (host_.beginGesture) host_.beginGesture(index_);
      // Where step N sits is plugin.h's rule (stepValueOf), the same one the
      // formatter, the LV2 scale points and the VST3 snap read. This list
      // used to spell it for itself.
      if (host_.setParameter) host_.setParameter(index_, stepValueOf(info_, selected));
      if (host_.endGesture) host_.endGesture(index_);
    };
    sync();
  }

  void sync() {
    if (!host_.getParameter || info_.stepCount <= 0) return;
    box_.setSelectedIndex(stepIndexOf(info_, host_.getParameter(index_)), /*notify=*/false);
  }

private:
  ComboBox& box_;
  const ParamInfo& info_;
  int index_;
  EditorHost host_;
};

/**
 * The editor a plugin gets when it supplies none of its own.
 *
 * A generic processor editor, and it exists for the usual reason: a plugin must never be faceless. Until now a webview that could not
 * start left the user one line of text and no controls at all -- the plugin
 * played, automated and saved, and nothing in its own window could be touched.
 *
 * It is deliberately a plain list. A generated plugin that wants a face builds
 * one; this is the floor, and a floor that tried to look designed would be a
 * second design to keep in step with the first.
 *
 * -- Groups -----------------------------------------------------------------
 *
 * A plain list stops being a floor somewhere around thirty parameters, where
 * it becomes a wall the user has to read all of to find one control. Every
 * wrapper already reports ParamInfo::group -- CLAP module path, VST3 unit, AU
 * clump, LV2 port group -- so a HOST's own generic panel has been showing
 * these parameters grouped while ours showed them flat. The data was there;
 * only this used none of it.
 *
 * The layout follows what the hosts do with a mixed table: parameters with no
 * group stay at the top as ordinary rows, and each declared group becomes a
 * section under them. A table with no groups at all therefore lays out exactly
 * as it did before -- no panel is built, and the flat list is not a special
 * case of anything.
 *
 * Sections start OPEN, all of them. A closed section is invisible to a screen
 * reader as well as to the eye, so opening collapsed would have taken
 * parameters that used to be reachable and hidden them behind a control the
 * reader cannot see -- the panel paints its headers rather than making them
 * components. Open-by-default keeps every parameter reachable on the first
 * frame and leaves collapsing to the user, which is the direction that cannot
 * lose anything.
 */
class GenericEditor : public Widget {
public:
  static constexpr float kRowHeight = 34.0f;
  static constexpr float kPadding = 12.0f;
  static constexpr float kLabelWidth = 108.0f;
  static constexpr float kValueWidth = 84.0f;
  /** A toggle is a control to press, not a bar to fill the row with. */
  static constexpr float kSwitchWidth = 96.0f;
  static constexpr float kKeyboardHeight = 64.0f;

  GenericEditor(const ParamInfo* params, int numParams, EditorHost host, Font font)
      : params_(params), numParams_(numParams < 0 ? 0 : numParams), host_(std::move(host)) {
    setFont(font);
    // A layout container, and nothing else. Announcing it puts an unnamed
    // level between a reader and every control in the plugin -- which the user
    // then has to walk down through before they reach anything at all.
    setAccessibilityIgnored(true);

    // Before any row is built, because a row's PARENT depends on the answer.
    groups_ = collectGroups(params_, numParams_);
    if (groups_.count > 0) {
      panel_.setFont(font);
      for (int g = 0; g < groups_.count; ++g) {
        // No font: a section body draws nothing at all. It is a parent and a
        // name in the accessible tree, and the rows inside it carry their own.
        bodies_.push_back(std::unique_ptr<RowHolder>(new RowHolder(groups_.names[g])));
      }
    }

    rows_.reserve((size_t) numParams_);
    for (int i = 0; i < numParams_; ++i) {
      auto row = std::make_unique<Row>();
      const ParamInfo& p = params_[i];
      // -1 for a parameter that declares no group, which is the ordinary case
      // and stays an ordinary row at the top.
      const int group = groups_.indexOf(p.group);
      Component* parent =
          group >= 0 ? (Component*) bodies_[(size_t) group].get() : (Component*) this;
      row->name = std::make_unique<Label>(p.label, Justify::Left);
      row->name->setFont(font);
      // Ignored for a reader: the control beside it carries the same name, and
      // announcing both means hearing every parameter twice.
      row->name->setAccessibilityIgnored(true);
      parent->addChild(row->name.get());

      // A stepped parameter with names is a choice, not a slider. Drawing it
      // as one would ask a user to find "Bandpass" by dragging.
      const bool named = p.stepCount > 1 && p.valueNames != nullptr && p.numValueNames > 0;
      // ...and a choice between exactly TWO things is a switch, not a list.
      const bool binary = p.stepCount == 2;
      if (binary) {
        row->button = std::make_unique<Button>(p.label);
        row->button->setFont(font);
        parent->addChild(row->button.get());
        row->buttonAttach =
            std::make_unique<ButtonAttachment>(*row->button, p, i, host_);
      } else if (named) {
        row->combo = std::make_unique<ComboBox>();
        row->combo->setFont(font);
        parent->addChild(row->combo.get());
        // Forwarded, not handled. A drop-down is a top-level window and this
        // editor is a Component: whatever owns a window subscribes, and where
        // nobody does the box cycles as it always did.
        row->combo->onOpenMenu = [this](ComboBox& box) {
          if (onOpenComboMenu) onOpenComboMenu(box);
          else box.setSelectedIndex((box.selectedIndex() + 1) %
                                    (int) (box.items().empty() ? 1 : box.items().size()));
        };
        row->combo->setAccessibleName(p.label);
        row->comboAttach =
            std::make_unique<ComboAttachment>(*row->combo, p, i, host_);
      } else {
        row->slider = std::make_unique<Slider>(Slider::Style::LinearHorizontal);
        parent->addChild(row->slider.get());
        // A ValueBox rather than a Label: the number under a knob was there to
        // be READ and not used, and a 66 dB range across two hundred pixels
        // cannot be dragged to the tenth of a decibel somebody asked for.
        row->value = std::make_unique<ValueBox>();
        row->value->setFont(font);
        parent->addChild(row->value.get());
        row->attach = std::make_unique<SliderAttachment>(*row->slider, p, i, host_);
        // Named for a screen reader, from the SAME label drawn beside it. A
        // knob with no accessible name is announced as "slider, -6.0 dB" with
        // no way to know which slider, which is a list of numbers rather than
        // an interface.
        row->slider->setAccessibleName(p.label);
        ValueBox* valueBox = row->value.get();
        Slider* slider = row->slider.get();
        row->attach->onDisplayChanged = [valueBox, slider](const std::string& t) {
          valueBox->setText(t);
          // The same text to both, from one place. A reader announcing a
          // different number from the displayed one is worse than one
          // announcing nothing.
          slider->setAccessibleValueText(t);
        };
        SliderAttachment* attach = row->attach.get();
        row->value->onTextEntered = [attach](const std::string& typed) {
          return attach->setFromText(typed);
        };
        row->attach->sync();
      }
      // Right-click reaches the host's menu, from whichever control this row
      // ended up with. Wired here rather than in the two branches above so a
      // control added later cannot quietly miss it.
      Widget* control = row->slider   ? (Widget*) row->slider.get()
                        : row->combo  ? (Widget*) row->combo.get()
                                      : (Widget*) row->button.get();
      if (control) {
        EditorHost* h = &host_;
        control->onContextMenu = [h, i](const MouseEvent& e) {
          if (h->showContextMenu)
            h->showContextMenu(i, (int) e.rootPosition.x, (int) e.rootPosition.y);
        };
      }

      Row* raw = row.get();
      rows_.push_back(std::move(row));
      if (group >= 0) bodies_[(size_t) group]->add(raw);
      else ungrouped_.push_back(raw);
    }

    // The panel joins the children AFTER the ungrouped rows, and the order is
    // the whole point of doing it here rather than above.
    //
    // Child order is reading order: it is what a screen reader walks and what
    // Tab follows. Added before the rows -- which is where this was first
    // written, because that is where the bodies are made -- the tree read
    // Filter, Envelope, THEN the two ungrouped controls sitting at the very top
    // of the window. A reader was told about a filter section before the input
    // knob above it, and Tab jumped down the window and back up.
    //
    // Measured rather than reasoned about: the probe printed each node's screen
    // y beside its position in the tree, and 12 and 46 came last.
    if (groups_.count > 0) {
      addChild(&panel_);
      // Sections in table order, every one open. See the class comment for why
      // open rather than collapsed.
      for (int g = 0; g < groups_.count; ++g)
        panel_.addSection(groups_.names[g], bodies_[(size_t) g].get(),
                          bodies_[(size_t) g]->preferredHeight(), /*open=*/true);
    }

    // An instrument gets a keyboard. A synth with none cannot be auditioned
    // until a controller is plugged in and mapped, which is a barrier in
    // exactly the first thirty seconds someone spends with a plugin.
    if (host_.noteOn) {
      keyboard_ = std::make_unique<MidiKeyboard>();
      addChild(keyboard_.get());
      MidiKeyboard* kb = keyboard_.get();
      EditorHost* h = &host_;
      keyboard_->onNoteOn = [h](int note, float velocity) {
        if (h->noteOn) h->noteOn(note, velocity);
      };
      keyboard_->onNoteOff = [h](int note) {
        if (h->noteOff) h->noteOff(note);
      };
      (void) kb;
    }

    setSize(360.0f, preferredHeight());
  }

  /**
   * Grows and SHRINKS as sections close, which is the point of closing one.
   *
   * The Viewport above this asks every frame during a transition, so the scroll
   * bar tracks the section rather than arriving after it.
   */
  float preferredHeight() const {
    const int flat = groups_.count > 0 ? (int) ungrouped_.size()
                                       : (numParams_ > 0 ? numParams_ : 1);
    return kPadding * 2.0f + (float) flat * kRowHeight +
           (groups_.count > 0 ? panel_.preferredHeight() : 0.0f) +
           (keyboard_ ? kKeyboardHeight + kPadding : 0.0f);
  }

  /**
   * Advance the section animations. Handed the window's clock rather than
   * reading one, same as everything else here that moves over time.
   *
   * Returns whether anything moved, so a still editor costs no repaint -- and
   * the caller uses it to know when to re-ask preferredHeight().
   */
  bool tickAnimations(double now) {
    if (groups_.count == 0) return false;
    panel_.setInputClock(now);
    return panel_.tick(now);
  }

  /** The sections, for a plugin that wants to collapse one at startup or a
   *  test that wants to know what the user is looking at. Null when the
   *  parameter table declares no groups. */
  ConcertinaPanel* sections() { return groups_.count > 0 ? &panel_ : nullptr; }

  /** Null on an effect. Exposed so a wrapper can light keys the DSP is
   *  sounding, which is the answer to "is it even receiving MIDI". */
  MidiKeyboard* keyboard() { return keyboard_.get(); }

  /** Set by whatever owns a window, so a stepped parameter drops down a real
   *  menu instead of cycling. The box passed in knows its own items, its own
   *  selection and its own bounds; the handler supplies the window. */
  std::function<void(ComboBox&)> onOpenComboMenu;

  /** Pull every value from the host. Called on the UI tick so the editor
   *  follows automation, preset loads and host undo. */
  void sync() {
    for (auto& row : rows_) {
      if (row->attach) row->attach->sync();
      if (row->comboAttach) row->comboAttach->sync();
      if (row->buttonAttach) row->buttonAttach->sync();
    }
  }

  /**
   * The PLAIN value row `index` is currently showing.
   *
   * Read off the widget, not off the host. That is the whole point: a test
   * asking whether the editor followed a change has to look at the control, or
   * it is checking the value it set itself.
   */
  float parameterValueShown(int index) const {
    if (index < 0 || index >= (int) rows_.size() || !params_) return 0.0f;
    const Row& row = *rows_[(size_t) index];
    if (row.slider) return (float) toPlainValue(params_[index], row.slider->value());
    if (row.combo) return stepValueOf(params_[index], row.combo->selectedIndex());
    if (row.button)
      return row.button->isToggled() ? params_[index].maxValue : params_[index].minValue;
    return 0.0f;
  }

  void paint(Graphics& g) override {
    g.fillAll(lookAndFeel().background());
  }

  /**
   * Laid out with FlexBox rather than by hand.
   *
   * The old version was four lines of subtraction per row, and it is what the
   * layout engine was written for: `w - controlX - kValueWidth - kPadding * 2`
   * is correct exactly once and wrong after any change to any of the four
   * numbers in it. A row is a label of a fixed width, a control that takes what
   * is left, and a readout of a fixed width -- which is what the code below now
   * says.
   */
  void resized() override {
    const float w = localBounds().w;
    if (keyboard_) {
      // Along the bottom, full width, where every soft synth puts one.
      keyboard_->setBounds({kPadding, localBounds().h - kKeyboardHeight - kPadding,
                            w - kPadding * 2.0f, kKeyboardHeight});
    }
    // Ungrouped rows first. With no groups declared this is every row and the
    // layout is the one this editor always had.
    const std::vector<Row*>& flat = groups_.count > 0 ? ungrouped_ : allRows();
    for (size_t i = 0; i < flat.size(); ++i)
      layoutRow(*flat[i], Rect(kPadding, kPadding + (float) i * kRowHeight,
                               w - kPadding * 2.0f, kRowHeight - 6.0f));

    if (groups_.count > 0) {
      const float top = kPadding + (float) ungrouped_.size() * kRowHeight;
      panel_.setBounds({kPadding, top, w - kPadding * 2.0f, panel_.preferredHeight()});
    }
  }

private:
  struct Row {
    std::unique_ptr<Label> name;
    std::unique_ptr<ValueBox> value;
    std::unique_ptr<Slider> slider;
    std::unique_ptr<ComboBox> combo;
    std::unique_ptr<Button> button;
    std::unique_ptr<SliderAttachment> attach;
    std::unique_ptr<ComboAttachment> comboAttach;
    std::unique_ptr<ButtonAttachment> buttonAttach;
  };

  /**
   * One row, into whatever rectangle it was given.
   *
   * Pulled out of resized() unchanged when sections arrived: a row inside a
   * section is the same row, and two copies of this that could drift apart
   * would mean a grouped editor slowly stopping looking like a flat one.
   */
  static void layoutRow(Row& row, const Rect& area) {
    FlexBox flex;
    flex.direction = FlexBox::Direction::Row;
    flex.alignItems = AlignItems::Stretch;
    flex.add(FlexItem(row.name.get()).withWidth(kLabelWidth).withShrink(0.0f));
    flex.add(FlexItem::spacer(8.0f));
    if (row.button) {
      // A switch does not need the whole row: a button as wide as a slider
      // reads as a banner rather than as something to press. Left-aligned at a
      // fixed width, with the rest of the row left empty.
      flex.add(FlexItem(row.button.get()).withWidth(kSwitchWidth).withShrink(0.0f)
                   .withMargin(Margin(2.0f, 0.0f)));
      flex.add(FlexItem::spacer(0.0f).withGrow(1.0f));
    } else if (row.combo) {
      // A combo box takes the whole remainder and is inset a little
      // vertically, which is the one thing a row does differently.
      flex.add(FlexItem(row.combo.get()).withGrow(1.0f).withMargin(Margin(2.0f, 0.0f)));
    } else {
      // A minimum of 20 so a very narrow editor still leaves something to
      // grab, rather than a track of no width that cannot be dragged.
      flex.add(FlexItem(row.slider.get()).withGrow(1.0f).withMinWidth(20.0f));
      flex.add(FlexItem::spacer(kPadding));
      flex.add(FlexItem(row.value.get()).withWidth(kValueWidth).withShrink(0.0f));
    }
    flex.performLayout(area);
  }

  /**
   * The body of one section.
   *
   * It owns no rows -- GenericEditor owns every Row and this holds borrowed
   * pointers, because two owners of the same widget is the bug that outlives
   * whoever wrote it.
   *
   * It IS announced, with the group's name and the Group role. The panel paints
   * its headers rather than making them components, so without this the
   * grouping would exist for the eye and not for a reader, and a reader would
   * hear forty controls in a row exactly as before.
   */
  class RowHolder : public Component {
  public:
    explicit RowHolder(const char* name) : name_(name ? name : "") {}

    void add(Row* row) { rows_.push_back(row); }

    float preferredHeight() const { return (float) rows_.size() * kRowHeight + 4.0f; }

    void resized() override {
      for (size_t i = 0; i < rows_.size(); ++i)
        layoutRow(*rows_[i],
                  Rect(0.0f, (float) i * kRowHeight, localBounds().w, kRowHeight - 6.0f));
    }

    AccessibleInfo accessibleInfo() const override {
      AccessibleInfo info;
      info.role = AccessibleRole::Group;
      info.name = name_;
      return info;
    }

  private:
    std::string name_;
    std::vector<Row*> rows_;
  };

  /** Every row as borrowed pointers, for the ungrouped path where "the flat
   *  list" and "all of them" are the same list. */
  const std::vector<Row*>& allRows() const {
    if (allRows_.size() != rows_.size()) {
      allRows_.clear();
      for (const auto& row : rows_) allRows_.push_back(row.get());
    }
    return allRows_;
  }

  std::unique_ptr<MidiKeyboard> keyboard_;
  const ParamInfo* params_;
  int numParams_;
  EditorHost host_;
  std::vector<std::unique_ptr<Row>> rows_;

  GroupTable groups_;
  ConcertinaPanel panel_;
  std::vector<std::unique_ptr<RowHolder>> bodies_;
  std::vector<Row*> ungrouped_;
  mutable std::vector<Row*> allRows_;
};

} // namespace gfx
} // namespace sonore
