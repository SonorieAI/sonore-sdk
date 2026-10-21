// SPDX-License-Identifier: Apache-2.0
//
// SplashScreen: a borderless window that shows an image while an application
// starts, and fades away once it has been up for long enough.
//
// ── The reference's shape ──────────────────────────────────────────────────
//
// Made with `new` as the application starts; shown at once, centred, above
// everything and off the taskbar. When the application is ready it calls
// deleteAfterDelay(minimum, removeOnMouseClick): the splash stays until it has
// been on screen for at least `minimum` (so a fast start does not flash it),
// then fades out and deletes ITSELF. With removeOnMouseClick a click starts
// the fade at once. Nothing else may delete it after deleteAfterDelay.
//
// ── Time as an argument ────────────────────────────────────────────────────
//
// The clock is advance(elapsedMs), as everywhere in this SDK that times
// something: the window's 33 ms tick drives it, and a test drives it with
// exact numbers -- "at 999 ms it is still opaque, at 1000 the fade begins"
// is an assertion, not a sleep.
//
// The fade is the window's alpha (a layered window on Windows, alphaValue on
// macOS, _NET_WM_WINDOW_OPACITY on X11, which a compositor may ignore). The
// deletion goes through the message queue, never from inside the tick or the
// click that ended it.
//
// RUN on Windows: style, placement, the timing at its edges, the fade's alpha
// read back from the window, the click, and the self-deletion.
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "../message_manager.h"
#include "document_window.h"

namespace sonore {
namespace gfx {

/**
 * A borderless window showing an image while an application starts, that
 * fades out and deletes itself -- the reference's SplashScreen, with the same
 * ownership. See the file comment for the timing and the fade.
 *
 * Make it with `new`; it shows itself, centred and on top, from the
 * constructor. Once deleteAfterDelay() is called it OWNS ITSELF: nothing else
 * may delete it, and it is deleted from the message queue after the fade,
 * with onDismissed called first. An application's window, not a plugin's.
 * Message thread only.
 */
class SplashScreen : public TopLevelWindow {
public:
  /** How long the fade takes once it starts. */
  static constexpr double kFadeMs = 300.0;

  /** Shows `image` at its own size, centred on the main display. */
  SplashScreen(const std::string& title, const Bitmap& image, bool useDropShadow)
      : TopLevelWindow(title, true), image_(image), shadow_(useDropShadow) {
    start((float) (image.width() > 0 ? image.width() : 1),
          (float) (image.height() > 0 ? image.height() : 1));
  }

  /** No image: a subclass paints, at this size. */
  SplashScreen(const std::string& title, float width, float height, bool useDropShadow)
      : TopLevelWindow(title, true), shadow_(useDropShadow) {
    start(width, height);
  }

  /** Takes the window off the desktop. Before deleteAfterDelay() the
   *  creator may delete it; after it, only the splash deletes itself. */
  ~SplashScreen() override { removeFromDesktop(); }

  int getDesktopWindowStyleFlags() const override {
    return windowIsTemporary | (shadow_ ? windowHasDropShadow : 0);
  }

  /**
   * Go once shown for at least `minimumMsToDisplay` (counted from when the
   * splash appeared), fading, then deleting this object. With
   * `removeOnMouseClick`, a click starts the fade straight away.
   */
  void deleteAfterDelay(double minimumMsToDisplay, bool removeOnMouseClick) {
    deleteRequested_ = true;
    minimumMs_ = minimumMsToDisplay;
    removeOnClick_ = removeOnMouseClick;
    advance(0.0);
  }

  /** The splash's clock. Called by its window's tick; callable with exact
   *  numbers by a test. */
  void advance(double elapsedMs) {
    if (gone_) return;
    shownMs_ += elapsedMs > 0.0 ? elapsedMs : 0.0;
    if (!fading_ && deleteRequested_ && shownMs_ >= minimumMs_) beginFade();
    if (!fading_) return;
    fadedMs_ += elapsedMs > 0.0 ? elapsedMs : 0.0;
    const double left = 1.0 - fadedMs_ / kFadeMs;
    setWindowAlpha((float) (left > 0.0 ? left : 0.0));
    if (left <= 0.0) finish();
  }

  /** Milliseconds it has been up: the sum of advance()'s steps since the
   *  constructor showed it. */
  double getMillisecondsShown() const { return shownMs_; }
  /** The fade has begun: the minimum time is up, or a click started it. */
  bool isFading() const { return fading_; }
  /** The fade is over and the window hidden; the object is deleted from the
   *  message queue next. */
  bool hasFinished() const { return gone_; }

  /** Just before it deletes itself, on the message thread. */
  std::function<void()> onDismissed;

  /** The look-and-feel's background, then the image over the whole window.
   *  A subclass made with a size instead of an image paints its own. */
  void paint(Graphics& g) override {
    g.fillAll(lookAndFeel().background());
    if (!image_.isEmpty()) g.drawImage(image_, localBounds());
  }

  /** Starts the fade if deleteAfterDelay() asked for removeOnMouseClick;
   *  before that call a click does nothing. */
  void mouseDown(const MouseEvent&) override {
    if (removeOnClick_ && deleteRequested_ && !fading_) beginFade();
  }

  void windowTick(double elapsedMs) override { advance(elapsedMs); }

private:
  void start(float width, float height) {
    setAlwaysOnTop(true);
    centreWithSize(width, height);
    setVisible(true);
  }

  void beginFade() {
    fading_ = true;
    fadedMs_ = 0.0;
  }

  void finish() {
    gone_ = true;
    setVisible(false);
    // From the queue: this may be running inside the window's own tick.
    std::weak_ptr<char> token = lifetimeToken();
    SplashScreen* self = this;
    auto end = [self, token]() {
      if (token.expired()) return;
      if (self->onDismissed) self->onDismissed();
      delete self;
    };
    if (!MessageManager::callAsync(end)) end();
  }

  Bitmap image_;
  bool shadow_ = false;
  bool deleteRequested_ = false;
  bool removeOnClick_ = false;
  bool fading_ = false;
  bool gone_ = false;
  double minimumMs_ = 0.0;
  double shownMs_ = 0.0;
  double fadedMs_ = 0.0;
};

} // namespace gfx
} // namespace sonore
