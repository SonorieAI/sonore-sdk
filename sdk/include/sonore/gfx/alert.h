// SPDX-License-Identifier: Apache-2.0
//
// Telling the user something, and asking them something.
//
// ── The gap this fills ──────────────────────────────────────────────────────
//
// A plugin had no way to say anything. It could show values and take clicks,
// and if a sample failed to load, or a preset came from a newer version, or an
// action was about to throw away an edit, the only options were to fail
// silently or to print to a console nobody sees. "It just didn't load" is one
// of the worst bug reports to receive and the easiest to prevent.
//
// ── Why these are overlays and not windows ─────────────────────────────────
//
// An alert is conventionally a real top-level window, and for an application
// that is right. For a PLUGIN it is close to the worst thing you can do: a modal
// window owned by a plugin can end up behind the host's own window, where the
// user cannot see it and cannot dismiss it, and the host's message loop is
// waiting on a click that can never happen. Every DAW forum has this thread.
//
// So these live INSIDE the editor. An overlay covers the editor, takes every
// click, and cannot be lost behind anything -- because it is not a window, it
// is a rectangle in a window the host is already showing. It also means one
// implementation rather than three peers' worth, and it can be tested against a
// Bitmap with no display at all.
//
// The cost is honest and worth stating: an overlay cannot extend past the
// editor's edge, so a long message wraps rather than widening. A plugin editor
// is not the place for a long message.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "component.h"
#include "graphics.h"
#include "widgets.h"

namespace sonore {
namespace gfx {

/**
 * A component that covers its parent and swallows everything.
 *
 * The swallowing is the point. Without it the editor behind stays live: a
 * "discard your changes?" question with a working knob behind it is not a
 * question, and a click that lands on both is a click the user did not mean to
 * make twice.
 */
class Overlay : public Widget {
public:
  Overlay() { setVisible(false); }

  /** How dark the editor behind goes. Dimmed rather than hidden so the user can
   *  still see what they are being asked about. */
  void setDimAlpha(uint8_t alpha) {
    dim_ = alpha;
    repaint();
  }

  /** Called when the overlay wants to go away -- Escape, or a click outside a
   *  call-out. The owner decides what that means. */
  std::function<void()> onDismiss;

  /** Sized to the parent every time it is shown, since the editor may have been
   *  resized while it was hidden. */
  void coverParent() {
    if (Component* p = parent()) setBounds({0.0f, 0.0f, p->bounds().w, p->bounds().h});
  }

  void paint(Graphics& g) override {
    if (dim_ > 0) g.fillAll(Colour(0, 0, 0, dim_));
  }

  // Every pointer event stops here, including the ones that hit nothing.
  void mouseDown(const MouseEvent&) override {}
  void mouseUp(const MouseEvent&) override {}
  /** Returns TRUE -- "handled" -- so it does not bubble. A wheel that reached
   *  the editor underneath would scroll a viewport behind a question. */
  bool mouseWheel(const MouseEvent&, float) override { return true; }

  bool keyPressed(const KeyPress& key) override {
    if (key.is(KeyPress::Escape)) {
      if (onDismiss) onDismiss();
      return true;
    }
    // Nothing else escapes either: a shortcut reaching the editor underneath
    // while a question is up is the keyboard equivalent of the live knob.
    return true;
  }

protected:
  uint8_t dim_ = 120;
};

/**
 * A message, and one or more answers.
 *
 * Modal in behaviour and not in implementation: nothing blocks, and the answer
 * arrives through onChoice. A plugin that blocked its own editor thread would
 * stop the host's message loop, which is how a "harmless" dialog becomes a
 * hung DAW.
 */
class AlertOverlay : public Overlay {
public:
  AlertOverlay() {
    setWantsKeyboardFocus(true);
    onDismiss = [this]() { choose(cancelIndex()); };
  }

  /**
   * Put a question up.
   *
   * `buttons` reads left to right. The LAST is the cancel -- what Escape and a
   * dismissal mean -- and the FIRST is the default, which Return presses. That
   * ordering is a convention rather than a law, so setDefaultButton and
   * setCancelButton exist for the case where it is wrong; but a caller that
   * says nothing gets the arrangement that is right for "Discard / Keep".
   */
  void show(std::string title, std::string message, std::vector<std::string> buttons,
            std::function<void(int)> onChoice) {
    title_ = std::move(title);
    message_ = std::move(message);
    onChoice_ = std::move(onChoice);

    for (auto& b : buttons_) removeChild(b.get());
    buttons_.clear();
    for (size_t i = 0; i < buttons.size(); ++i) {
      auto button = std::make_unique<Button>(buttons[i]);
      const int index = (int) i;
      button->onClick = [this, index]() { choose(index); };
      addChild(button.get());
      buttons_.push_back(std::move(button));
    }
    defaultButton_ = buttons_.empty() ? -1 : 0;
    cancelButton_ = buttons_.empty() ? -1 : (int) buttons_.size() - 1;

    coverParent();
    setVisible(true);
    resized();
    if (MouseRouter* r = routerFor(this)) r->setFocus(this);
    repaint();
  }

  void setDefaultButton(int index) { defaultButton_ = index; }
  void setCancelButton(int index) { cancelButton_ = index; }

  bool isShowing() const { return isVisible(); }

  /** A Dialog, named by its title and reading its message. Which is the whole
   *  of what a reader needs here: the buttons are children and announce
   *  themselves. */
  AccessibleInfo accessibleInfo() const override {
    AccessibleInfo info = baseInfo(AccessibleRole::Dialog);
    if (info.name.empty()) info.name = title_;
    info.value = message_;
    return info;
  }

  /** The panel, centred in the overlay. Exposed so a test can assert where it
   *  landed rather than hunting for it in pixels. */
  Rect panelBounds() const {
    const Rect b = localBounds();
    const float w = b.w * 0.8f < kMaxWidth ? b.w * 0.8f : kMaxWidth;
    const float h = panelHeight(w);
    return {(b.w - w) * 0.5f, (b.h - h) * 0.5f, w, h};
  }

  void resized() override {
    const Rect panel = panelBounds();
    // Right to left from the panel's corner, so the FIRST button ends up
    // leftmost without needing the total width computed first.
    float x = panel.right() - kPad;
    for (size_t i = buttons_.size(); i-- > 0;) {
      const float w = buttonWidth(buttons_[i]->label());
      x -= w;
      buttons_[i]->setBounds({x, panel.bottom() - kPad - kButtonHeight, w, kButtonHeight});
      x -= kPad * 0.5f;
    }
  }

  void paint(Graphics& g) override {
    Overlay::paint(g);
    LookAndFeel& lf = lookAndFeel();
    const Rect panel = panelBounds();

    g.setColour(lf.background());
    g.fillRoundedRect(panel, kCorner);
    g.setColour(lf.outline());
    g.drawRoundedRect(panel, kCorner, 1.0f);

    const float textX = panel.x + kPad;
    const float textW = panel.w - kPad * 2.0f;
    float y = panel.y + kPad;
    if (!title_.empty()) {
      lf.drawLabel(g, Rect(textX, y, textW, font().lineHeight()), title_, font(), Justify::Left,
                   stateFor(false));
      y += font().lineHeight() + kPad * 0.5f;
    }
    if (!message_.empty()) {
      g.setColour(lf.text());
      font().drawWrapped(g, message_, Rect(textX, y, textW, panel.bottom() - y));
    }
  }

  bool keyPressed(const KeyPress& key) override {
    if (key.is(KeyPress::Return) && defaultButton_ >= 0) {
      choose(defaultButton_);
      return true;
    }
    return Overlay::keyPressed(key);
  }

  static constexpr float kMaxWidth = 360.0f;
  static constexpr float kPad = 14.0f;
  static constexpr float kButtonHeight = 26.0f;
  static constexpr float kCorner = 6.0f;

private:
  int cancelIndex() const { return cancelButton_; }

  void choose(int index) {
    if (!isVisible()) return;
    setVisible(false);
    if (MouseRouter* r = routerFor(this))
      if (r->focused() == this) r->setFocus(nullptr);
    // The callback LAST, and after the overlay is already down: a handler that
    // opens another question would otherwise be showing it into an overlay
    // that is about to hide itself.
    if (onChoice_) onChoice_(index);
    repaint();
  }

  float buttonWidth(const std::string& label) const {
    const float w = font().stringWidth(label) + kPad * 2.0f;
    return w < kMinButtonWidth ? kMinButtonWidth : w;
  }

  float panelHeight(float width) const {
    const float textW = width - kPad * 2.0f;
    float h = kPad * 2.0f + kButtonHeight + kPad;
    if (!title_.empty()) h += font().lineHeight() + kPad * 0.5f;
    if (!message_.empty()) h += font().wrappedHeight(message_, textW);
    return h;
  }

  static constexpr float kMinButtonWidth = 72.0f;

  std::string title_, message_;
  std::vector<std::unique_ptr<Button>> buttons_;
  std::function<void(int)> onChoice_;
  int defaultButton_ = -1, cancelButton_ = -1;
};

/**
 * A bubble anchored to a control.
 *
 * For the things a tooltip is too small for and a dialog is too much for: a
 * short explanation, a small set of options, a colour picker hanging off a
 * swatch. It points AT something, which is what makes it different from a
 * panel that happens to be nearby.
 *
 * Dismissed by a click anywhere outside the bubble, and by Escape.
 */
class CallOutBox : public Overlay {
public:
  CallOutBox() {
    // Not dimmed. A call-out explains the thing it points at, and darkening
    // that thing is the opposite of what it is for.
    setDimAlpha(0);
    setWantsKeyboardFocus(true);
    onDismiss = [this]() { dismiss(); };
  }

  /**
   * Show `content` in a bubble pointing at `target`, which is in the OVERLAY's
   * coordinates -- that is, the editor's.
   *
   * The content is not owned. It is a component the caller keeps, exactly like
   * Viewport's, so a call-out can show something with state that survives being
   * put away and brought back.
   */
  void show(Component* content, Rect target, float width, float height) {
    content_ = content;
    target_ = target;
    coverParent();
    place(width, height);
    if (content_) {
      addChild(content_);
      content_->setBounds({bubble_.x + kPad, bubble_.y + kPad, bubble_.w - kPad * 2.0f,
                           bubble_.h - kPad * 2.0f});
    }
    setVisible(true);
    if (MouseRouter* r = routerFor(this)) r->setFocus(this);
    repaint();
  }

  void dismiss() {
    if (!isVisible()) return;
    setVisible(false);
    if (content_) {
      removeChild(content_);
      content_ = nullptr;
    }
    if (MouseRouter* r = routerFor(this))
      if (r->focused() == this) r->setFocus(nullptr);
    if (onDismissed) onDismissed();
    repaint();
  }

  std::function<void()> onDismissed;

  /** A Group rather than a Dialog: a call-out explains something, it does not
   *  ask. A reader announcing "dialog" would tell the user they are trapped in
   *  something they are not. */
  AccessibleInfo accessibleInfo() const override { return baseInfo(AccessibleRole::Group); }

  Rect bubbleBounds() const { return bubble_; }
  /** Which way the arrow points: true when the bubble sits ABOVE its target,
   *  which happens when there is no room below. */
  bool pointsDown() const { return above_; }

  void mouseDown(const MouseEvent& e) override {
    // Outside the bubble dismisses. Inside is the content's business, and the
    // content is a child, so this is only reached when nothing took it.
    if (!bubble_.contains(e.position)) dismiss();
  }

  void paint(Graphics& g) override {
    Overlay::paint(g);
    LookAndFeel& lf = lookAndFeel();

    Path shape;
    shape.addRoundedRect(bubble_, kCorner);

    // The arrow, as a separate triangle unioned by overlap rather than by path
    // arithmetic: two filled shapes sharing an edge composite to the same
    // pixels, and building one outline around a rounded rectangle plus a
    // triangle is a lot of geometry for a shape nobody will look at twice.
    const float tipX = clampToBubble(target_.x + target_.w * 0.5f);
    Path arrow;
    if (above_) {
      arrow.moveTo(tipX, bubble_.bottom() + kArrow);
      arrow.lineTo(tipX - kArrow, bubble_.bottom() - 1.0f);
      arrow.lineTo(tipX + kArrow, bubble_.bottom() - 1.0f);
    } else {
      arrow.moveTo(tipX, bubble_.y - kArrow);
      arrow.lineTo(tipX - kArrow, bubble_.y + 1.0f);
      arrow.lineTo(tipX + kArrow, bubble_.y + 1.0f);
    }
    arrow.close();

    g.setColour(lf.background());
    g.fillPath(shape);
    g.fillPath(arrow);
    g.setColour(lf.outline());
    g.drawRoundedRect(bubble_, kCorner, 1.0f);
    g.strokePath(arrow, 1.0f);
  }

  static constexpr float kPad = 10.0f;
  static constexpr float kArrow = 8.0f;
  static constexpr float kCorner = 5.0f;

private:
  float clampToBubble(float x) const {
    const float lo = bubble_.x + kCorner + kArrow;
    const float hi = bubble_.right() - kCorner - kArrow;
    return x < lo ? lo : (x > hi ? hi : x);
  }

  /**
   * Below the target if it fits, above if it does not, and always inside the
   * editor.
   *
   * The same rule PopupMenu uses, and for the same reason: a bubble drawn half
   * outside the editor is clipped by the window, so the half with the buttons
   * on it is simply not there.
   */
  void place(float width, float height) {
    const Rect area = localBounds();
    const float w = width < area.w - kPad * 2.0f ? width : area.w - kPad * 2.0f;
    const float h = height < area.h - kPad * 2.0f ? height : area.h - kPad * 2.0f;

    const float below = target_.bottom() + kArrow;
    above_ = below + h > area.bottom() && target_.y - kArrow - h >= area.y;
    const float y = above_ ? target_.y - kArrow - h : below;

    float x = target_.x + target_.w * 0.5f - w * 0.5f;
    if (x < area.x) x = area.x;
    if (x + w > area.right()) x = area.right() - w;

    float clampedY = y;
    if (clampedY < area.y) clampedY = area.y;
    if (clampedY + h > area.bottom()) clampedY = area.bottom() - h;

    bubble_ = Rect(x, clampedY, w, h);
  }

  Component* content_ = nullptr;
  Rect target_;
  Rect bubble_;
  bool above_ = false;
};

} // namespace gfx
} // namespace sonore
