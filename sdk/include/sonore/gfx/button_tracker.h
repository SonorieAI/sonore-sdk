// SPDX-License-Identifier: Apache-2.0
//
// ButtonTracker: an analytics event every time a button is clicked.
//
// The reference's tracker listens to a button and logs a named event, with
// the button's on/off state added when it is a toggle. This is that, for the
// buttons here -- Button, IconButton, or anything else with an `onClick` --
// and it sends nothing anywhere by itself: it calls Analytics::logEvent, which
// drops the event unless the plugin has added a destination (analytics.h says
// why that is the default).
//
// ── How it attaches, and the two rules that follow ──────────────────────────
//
// A button here has ONE onClick, a std::function, rather than a list of
// listeners. So the tracker wraps whatever onClick held when the tracker was
// constructed: the event is logged, then the previous handler runs.
//
// Logged FIRST, deliberately. The click a plugin most wants to count is often
// the one whose handler closes something -- and a handler that destroys the
// button, or the panel holding the tracker, must be the last thing that runs.
// Nothing touches the button or the tracker after it.
//
// Rule one: assign onClick BEFORE constructing the tracker. Assigning it
// afterwards replaces the wrapper, tracking stops, and nothing can notice.
//
// Rule two is a non-rule: destroy the two in any order. The wrapper and the
// tracker share a small block of state; a destroyed tracker marks it inactive
// and the wrapper becomes a plain call to the previous handler. Destroying the
// tracker does NOT put the old onClick back, because "restore what was there"
// would also throw away any handler assigned since.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#include "../analytics.h"

namespace sonore {
namespace gfx {

namespace detail {

template <typename T, typename = void>
struct HasToggleState : std::false_type {};

template <typename T>
struct HasToggleState<T, std::void_t<decltype(std::declval<const T&>().isToggleable()),
                                     decltype(std::declval<const T&>().isToggled())>>
    : std::true_type {};

/** The reference's parameter name and values, so a dashboard built against
 *  one reads the other. Added only for a button that IS a toggle: a push
 *  button reporting "Off" on every click would be reporting a state it does
 *  not have. */
template <typename ButtonType>
inline void addTrackedToggleState(const ButtonType& button, AnalyticsParameters& p, std::true_type) {
  if (button.isToggleable()) p["ButtonState"] = button.isToggled() ? "On" : "Off";
}

template <typename ButtonType>
inline void addTrackedToggleState(const ButtonType&, AnalyticsParameters&, std::false_type) {}

} // namespace detail

/**
 * Logs an analytics event on every click of one button, as the reference's
 * class of the same name does. See the file comment for how it attaches and
 * its one rule: set the button's onClick BEFORE constructing this.
 *
 * Message thread, like the clicks it counts. The tracker keeps no pointer to
 * the button, so the two may be destroyed in either order. Not copyable: a
 * copy would share the wrapper's state, and its destructor would switch the
 * original's tracking off.
 */
class ButtonTracker {
public:
  /**
   * Start logging `eventName` with `parameters` on every click of `button`.
   *
   * `analytics` defaults to Analytics::instance(); one passed in must outlive
   * the button's clicks.
   */
  template <typename ButtonType>
  ButtonTracker(ButtonType& button, std::string eventName, AnalyticsParameters parameters = {},
                int eventType = 0, Analytics* analytics = nullptr)
      : state_(std::make_shared<State>()) {
    state_->eventName = std::move(eventName);
    state_->parameters = std::move(parameters);
    state_->eventType = eventType;
    state_->analytics = analytics ? analytics : &Analytics::instance();

    std::shared_ptr<State> state = state_;
    std::function<void()> previous = std::move(button.onClick);
    ButtonType* target = &button;
    button.onClick = [state, previous, target]() {
      if (state->active) {
        AnalyticsParameters p = state->parameters;
        detail::addTrackedToggleState(*target, p, detail::HasToggleState<ButtonType>{});
        ++state->clicks;
        state->analytics->logEvent(state->eventName, p, state->eventType);
      }
      // A COPY, called last. The handler may destroy the button, and with it
      // this closure and everything it captured; a local copy is the one
      // thing still alive while it runs.
      std::function<void()> next = previous;
      if (next) next();
    };
  }

  /** Stops the logging. The handler the button had before goes on being
   *  called, through the wrapper this installed. */
  ~ButtonTracker() { state_->active = false; }

  ButtonTracker(const ButtonTracker&) = delete;
  ButtonTracker& operator=(const ButtonTracker&) = delete;

  /** The event logged on each click. */
  const std::string& eventName() const { return state_->eventName; }
  /** The parameters logged with it; for a toggle, each click's copy also
   *  gets ButtonState, On or Off. */
  const AnalyticsParameters& parameters() const { return state_->parameters; }
  /** The event type handed to Analytics::logEvent. */
  int eventType() const { return state_->eventType; }

  /** Clicks seen while this tracker was alive -- logged, whether or not
   *  Analytics had anywhere to send them. */
  int clicksTracked() const { return state_->clicks; }

private:
  /** Shared by the tracker and the onClick wrapper it installed, so either
   *  can go first: the tracker's destructor clears `active`, and the wrapper
   *  then only calls the previous handler. */
  struct State {
    std::string eventName;
    AnalyticsParameters parameters;
    int eventType = 0;
    Analytics* analytics = nullptr;
    int clicks = 0;
    bool active = true;
  };
  std::shared_ptr<State> state_;
};

} // namespace gfx
} // namespace sonore
