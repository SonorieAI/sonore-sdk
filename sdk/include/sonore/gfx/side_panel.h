// SPDX-License-Identifier: Apache-2.0
//
// SidePanel: a drawer that slides in over the editor from one edge.
//
// ── What it is for here ─────────────────────────────────────────────────────
//
// Settings, a preset list, a burger menu: the things an editor wants on hand
// without giving them permanent room. The drawer comes in over the content
// rather than pushing it aside, so opening it never re-lays out the controls
// under it -- a knob the user was about to grab stays where it was.
//
// ── The pieces ──────────────────────────────────────────────────────────────
//
// The SidePanel covers its parent, like an Overlay, and dims what is behind
// in proportion to how far the drawer is in. A press on the dimmed part
// closes it, as does Escape and the close button on its title bar, and
// dragging the title bar back towards the edge throws it away.
//
// ── The clock is an argument ────────────────────────────────────────────────
//
// The slide is Animator's, and like every timed thing in this SDK it moves
// only when tick(now) is called -- from the editor's 33 ms clock, which also
// passes setInputClock(now) so that a click starts its slide from the right
// moment. Which is what makes "a fifth of the way through closing, the drawer
// is here and the dim is this dark" an exact test rather than a sleep.
#pragma once

#include <functional>
#include <string>
#include <utility>

#include "alert.h"
#include "animator.h"
#include "widgets.h"

namespace sonore {
namespace gfx {

namespace detail {

/** A small button with a cross, for anything that closes. */
class CloseButton : public Widget {
public:
  CloseButton() {
    setWantsKeyboardFocus(true);
    setAccessibleName("Close");
    setCursor(MouseCursor::Pointing);
  }
  std::function<void()> onClick;

  void paint(Graphics& g) override {
    lookAndFeel().drawCloseButton(g, localBounds(), stateFor(pressed_));
    paintFocusRing(g);
  }
  void mouseDown(const MouseEvent&) override {
    pressed_ = true;
    repaint();
  }
  void mouseUp(const MouseEvent& e) override {
    const bool was = pressed_;
    pressed_ = false;
    repaint();
    if (was && localBounds().contains(e.position) && onClick) onClick();
  }
  bool keyPressed(const KeyPress& key) override {
    if (!key.is(KeyPress::Return) && key.character != ' ') return false;
    if (onClick) onClick();
    return true;
  }
  AccessibleInfo accessibleInfo() const override { return baseInfo(AccessibleRole::Button); }

private:
  bool pressed_ = false;
};

} // namespace detail

/**
 * A drawer with a title bar that slides in over its parent from the left or
 * right edge, dimming what is behind -- the reference's SidePanel, drawn
 * inside the editor. The file comment covers how it closes and why it has no
 * clock of its own.
 *
 * Add it as a child of the component it should cover (usually the editor's
 * root) and open it with showOrHide(true, now). The owner calls
 * setInputClock(now) and tick(now) on every frame, or the slide never moves.
 * The content is not owned and must outlive the panel or be replaced first.
 * Message thread only.
 */
class SidePanel : public Overlay {
public:
  /** The title bar's height until setTitleBarHeight, in logical pixels. */
  static constexpr float kDefaultTitleHeight = 30.0f;
  /** How long a slide takes until setSlideDuration, in seconds. */
  static constexpr double kDefaultSlideSeconds = 0.2;

  /** `width` is the drawer's, in logical pixels (at least 1, and no wider
   *  than the parent); `positionOnLeft` picks the edge it comes from;
   *  `content` is not owned and may be set later. Hidden until showOrHide;
   *  open, it dims the parent with alpha 100. */
  SidePanel(std::string title, float width, bool positionOnLeft, Component* content = nullptr)
      : title_(std::move(title)), width_(width > 1.0f ? width : 1.0f), onLeft_(positionOnLeft) {
    setDimAlpha(100);
    setAccessibleName(title_);
    body_.owner = this;
    close_.onClick = [this]() { showOrHide(false, clock_); };
    body_.addChild(&close_);
    addChild(&body_);
    setContent(content);
  }

  /** The content is not owned, as nothing is here. Null removes it. */
  void setContent(Component* content) {
    if (content_ == content) return;
    if (content_ && content_->parent() == &body_) body_.removeChild(content_);
    content_ = content;
    if (content_) body_.addChild(content_);
    close_.toFront();
    layoutBody();
  }
  /** The content, or null. Not owned. */
  Component* content() const { return content_; }

  /** The title bar's text, which is also the panel's accessible name. */
  void setTitle(std::string title) {
    title_ = std::move(title);
    setAccessibleName(title_);
    body_.repaint();
  }
  /** The title bar's text. */
  const std::string& title() const { return title_; }

  /** The drawer's width in logical pixels, at least 1; it is drawn no wider
   *  than the parent. A slide in progress stops, and the drawer goes to
   *  where it is heading. */
  void setPanelWidth(float width) {
    width_ = width > 1.0f ? width : 1.0f;
    placeAtRest();
  }
  /** The width asked for, in logical pixels; the drawer is narrower when the
   *  parent is. */
  float panelWidth() const { return width_; }

  /** Which edge the drawer comes from. A slide in progress stops, and the
   *  drawer goes to where it is heading on the new edge. */
  void setPositionOnLeft(bool onLeft) {
    onLeft_ = onLeft;
    placeAtRest();
  }
  /** True for the left edge, false for the right. */
  bool isPanelOnLeft() const { return onLeft_; }

  /** The title bar's height in logical pixels. 0 removes the bar and with it
   *  the drag to close; the close button shows only above 4. */
  void setTitleBarHeight(float h) {
    titleHeight_ = h >= 0.0f ? h : 0.0f;
    layoutBody();
  }
  /** The title bar's height, in logical pixels. */
  float titleBarHeight() const { return titleHeight_; }

  /** 0 snaps open and shut. */
  void setSlideDuration(double seconds) { slideSeconds_ = seconds > 0.0 ? seconds : 0.0; }

  /** What time it is, for a slide started by a click or a key. */
  void setInputClock(double now) { clock_ = now; }

  /**
   * Open or close, sliding from wherever the drawer is now.
   *
   * Opening covers the parent first -- it may have been resized while the
   * drawer was away -- and moves the keyboard into the drawer, so Tab and
   * Escape act on what the user is now looking at. Closing hands the keyboard
   * back to whoever had it, if they are still there.
   */
  void showOrHide(bool show, double now) {
    clock_ = now;
    if (show == showing_ && (show ? isVisible() : true)) return;
    showing_ = show;
    if (show) {
      coverParent();
      if (!isVisible()) {
        body_.setBounds(restingBounds(false));
        setVisible(true);
        toFront();
      }
      MouseRouter* router = routerFor(this);
      if (router) {
        // Not when the keyboard is already in here -- reopening mid-slide --
        // or the drawer would remember itself as where to go back to.
        if (!isInside(router->focused())) focusBefore_ = router->focused();
        if (Component* first = firstFocusable(&body_)) router->setFocus(first);
      }
    }
    animator_.animateBounds(&body_, restingBounds(show), slideSeconds_, now, show ? Easing::Out : Easing::In);
    if (!show) {
      // Handed back if the keyboard is in the drawer -- or nowhere, which is
      // where a click on the dimmed editor leaves it: the router takes focus
      // away before the drawer hears the press.
      MouseRouter* router = routerFor(this);
      if (router && (router->focused() == nullptr || isInside(router->focused()))) {
        Component* restore = focusBefore_;
        focusBefore_ = nullptr;
        router->setFocus(stillInTree(restore) ? restore : nullptr);
      }
    }
    finishIfClosed();
    repaint();
    if (onPanelShowHide) onPanelShowHide(show);
  }

  /** The state the drawer is heading for: true from the moment it starts
   *  opening until it starts closing. */
  bool isPanelShowing() const { return showing_; }
  /** Whether a slide is in progress, the spring back after a short drag of
   *  the title bar included. */
  bool isAnimating() const { return animator_.isAnimating(); }

  /** Drive the slide. Returns whether anything moved, so a still drawer
   *  costs no repaint. */
  bool tick(double now) {
    clock_ = now;
    const bool moved = animator_.tick(now);
    if (moved) {
      repaint();
      if (onPanelMove) onPanelMove();
    }
    finishIfClosed();
    return moved;
  }

  /** Where the drawer is now, in this component's coordinates. */
  Rect panelBounds() const { return body_.bounds(); }

  /** How far in the drawer is: 0 out of sight, 1 fully open. */
  float openFraction() const {
    const Rect b = body_.bounds();
    const Rect area = localBounds();
    if (b.w <= 0.0f) return 0.0f;
    const float inside = onLeft_ ? b.right() - area.x : area.right() - b.x;
    const float f = inside / b.w;
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
  }

  /** When the drawer starts heading somewhere else: true as it starts to
   *  open, false as it starts to close -- by showOrHide, a click on the dim,
   *  Escape, the close button or a throw. Not when the slide ends. */
  std::function<void(bool isShowing)> onPanelShowHide;
  /** Each frame the drawer moves, for a caller that follows it. */
  std::function<void()> onPanelMove;

  /** The close button, in this component's coordinates. */
  Rect closeButtonBounds() const {
    const Rect b = close_.bounds();
    return b.translated(body_.bounds().x, body_.bounds().y);
  }

  // ── Component ────────────────────────────────────────────────────────────

  /** Puts the drawer at rest for the new size, unless it is sliding. */
  void resized() override {
    if (!animator_.isAnimating()) body_.setBounds(restingBounds(showing_));
    layoutBody();
  }

  /** The dim, as dark as openFraction() says: none closed, the full dim
   *  alpha open. The drawer is a child and draws itself. */
  void paint(Graphics& g) override {
    const float f = openFraction();
    if (dim_ > 0 && f > 0.0f) g.fillAll(Colour(0, 0, 0, (uint8_t) ((float) dim_ * f + 0.5f)));
  }

  /** A press on the dimmed editor closes the drawer, and goes no further --
   *  it is a dismissal, not a click on whatever was underneath. */
  void mouseDown(const MouseEvent& e) override {
    if (!body_.bounds().contains(e.position) && showing_) showOrHide(false, clock_);
  }

  bool keyPressed(const KeyPress& key) override {
    if (key.is(KeyPress::Escape) && showing_) {
      showOrHide(false, clock_);
      return true;
    }
    // Everything else is the editor's: the drawer is not a dialog, and a
    // shortcut pressed while it is open still means what it always means.
    return false;
  }

  AccessibleInfo accessibleInfo() const override {
    AccessibleInfo info = baseInfo(AccessibleRole::Group);
    info.value = showing_ ? "open" : "closed";
    return info;
  }

private:
  /** The drawer itself: title bar, close button, content. */
  class Body : public Widget {
  public:
    SidePanel* owner = nullptr;

    void paint(Graphics& g) override {
      LookAndFeel& lf = lookAndFeel();
      lf.drawSidePanelBackground(g, localBounds(), owner->onLeft_);
      if (owner->titleHeight_ > 0.0f)
        lf.drawSidePanelTitle(g, Rect(0.0f, 0.0f, localBounds().w, owner->titleHeight_), owner->title_, font());
    }
    void resized() override { owner->layoutBody(); }

    /** The title bar drags: back towards the edge far enough and the drawer
     *  is thrown away, short of that it springs back. */
    void mouseDown(const MouseEvent& e) override {
      dragging_ = e.position.y < owner->titleHeight_;
      startX_ = bounds().x;
      // In ROOT coordinates: the drawer moves under the pointer as it is
      // dragged, so a distance measured in its own coordinates would chase
      // itself.
      downRootX_ = e.rootPosition.x;
    }
    void mouseDrag(const MouseEvent& e) override {
      if (!dragging_) return;
      const Rect rest = owner->restingBounds(true);
      float x = startX_ + (e.rootPosition.x - downRootX_);
      // Only outwards: a drawer pulled further in than it opens would be a
      // drawer that comes away from its edge.
      x = owner->onLeft_ ? (std::min)(x, rest.x) : (std::max)(x, rest.x);
      owner->animator_.cancel(this, false);
      setBounds({x, rest.y, rest.w, rest.h});
      owner->repaint();
    }
    void mouseUp(const MouseEvent&) override {
      if (!dragging_) return;
      dragging_ = false;
      const Rect rest = owner->restingBounds(true);
      const float pulled = owner->onLeft_ ? rest.x - bounds().x : bounds().x - rest.x;
      if (pulled > rest.w * 0.3f) owner->showOrHide(false, owner->clock_);
      else owner->animator_.animateBounds(this, rest, owner->slideSeconds_ * 0.5, owner->clock_);
    }
    AccessibleInfo accessibleInfo() const override {
      AccessibleInfo info = baseInfo(AccessibleRole::Unknown);
      return info;
    }

  private:
    bool dragging_ = false;
    float startX_ = 0.0f;
    float downRootX_ = 0.0f;
  };

  Rect restingBounds(bool open) const {
    const Rect area = localBounds();
    const float w = width_ < area.w ? width_ : area.w;
    float x;
    if (onLeft_) x = open ? area.x : area.x - w;
    else x = open ? area.right() - w : area.right();
    return {x, area.y, w, area.h};
  }

  void placeAtRest() {
    animator_.cancel(&body_, false);
    body_.setBounds(restingBounds(showing_));
    layoutBody();
    repaint();
  }

  void layoutBody() {
    const Rect b = body_.localBounds();
    const float t = titleHeight_;
    const float side = t > 4.0f ? t - 8.0f : 0.0f;
    close_.setVisible(t > 4.0f);
    close_.setBounds({b.w - side - 4.0f, 4.0f, side, side});
    if (content_) content_->setBounds({0.0f, t, b.w, (std::max)(0.0f, b.h - t)});
  }

  /** Once a closing slide has landed, the whole thing goes away -- hidden,
   *  so the editor underneath takes clicks again. */
  void finishIfClosed() {
    if (!showing_ && isVisible() && !animator_.isAnimating(&body_)) setVisible(false);
  }

  bool isInside(const Component* c) const {
    for (const Component* p = c; p; p = p->parent())
      if (p == this) return true;
    return false;
  }

  /** Whether `c` is still in this tree, outside the drawer. It may have been
   *  deleted since it was remembered, so it is FOUND first, by address among
   *  the living, and only then read: asking a deleted control for its parent
   *  was a heap-use-after-free. */
  bool stillInTree(const Component* c) const {
    if (!c) return false;
    const Component* top = this;
    while (top->parent()) top = top->parent();
    return contains(top, c) && !isInside(c);
  }

  static bool contains(const Component* root, const Component* target) {
    if (root == target) return true;
    for (const Component* child : root->children())
      if (contains(child, target)) return true;
    return false;
  }

  /** The content's first focusable component, and the close button only when
   *  the content has none: the drawer was opened to use what is in it. */
  Component* firstFocusable(Component* c) {
    if (!c || !c->isVisible()) return nullptr;
    for (Component* child : c->children()) {
      if (child == &close_ || !child->isVisible()) continue;
      if (child->wantsKeyboardFocus()) return child;
      if (Component* deeper = firstFocusable(child)) return deeper;
    }
    return c == &body_ && close_.isVisible() ? &close_ : nullptr;
  }

  std::string title_;
  float width_;
  bool onLeft_;
  float titleHeight_ = kDefaultTitleHeight;
  double slideSeconds_ = kDefaultSlideSeconds;
  double clock_ = 0.0;
  bool showing_ = false;
  Component* content_ = nullptr;
  Component* focusBefore_ = nullptr;
  Animator animator_;
  Body body_;
  detail::CloseButton close_;
};

} // namespace gfx
} // namespace sonore
