// SPDX-License-Identifier: Apache-2.0
//
// Several clocks in one object.
//
// ── What it is for ──────────────────────────────────────────────────────────
//
// An editor wants a meter at 30 Hz, a blinking record light at 2 Hz, and a
// "save the settings" nudge every ten seconds. Three periods, one object, and
// no three separate timer subscriptions to keep in step with each other's
// lifetimes.
//
// ── It takes the elapsed time as an ARGUMENT ───────────────────────────────
//
// The same decision the Animator and the TooltipManager already made here, for
// the same reason. A timer that read a clock could only be tested by sleeping,
// and a test that sleeps is slow, flaky on a loaded machine, and unable to
// check the interesting cases at all -- what happens when a single tick is
// longer than the interval, what happens after the editor was hidden for four
// seconds.
//
// Driven from the editor's own 33 ms tick, alongside everything else that
// crosses that boundary.
//
// ── Catching up, and deciding not to ───────────────────────────────────────
//
// A host that stalls for two seconds leaves a 30 Hz timer sixty ticks behind.
// Firing sixty times to catch up is almost never what anybody wants: sixty
// repaints back to back, and for a blinking light sixty state changes nobody
// sees. So a late timer fires ONCE and resets. That is a decision rather than
// an oversight, and it is the one a caller most needs to know about.
#pragma once

#include <functional>
#include <vector>

namespace sonore {

class MultiTimer {
public:
  /** Called with whichever timer came due. One callback rather than one per
   *  timer, so a caller can log or guard every tick in a single place. */
  std::function<void(int timerId)> onTimer;

  /**
   * Start or re-start timer `id`.
   *
   * Re-starting an already-running timer resets its phase, which is what
   * "restart" means everywhere else and what a caller changing a rate expects.
   * An interval of zero or less stops it rather than spinning.
   */
  void startTimer(int id, double intervalMs) {
    if (intervalMs <= 0.0) {
      stopTimer(id);
      return;
    }
    for (Timer& timer : timers_) {
      if (timer.id != id) continue;
      timer.interval = intervalMs;
      timer.elapsed = 0.0;
      timer.running = true;
      return;
    }
    Timer timer;
    timer.id = id;
    timer.interval = intervalMs;
    timers_.push_back(timer);
  }

  void stopTimer(int id) {
    for (Timer& timer : timers_)
      if (timer.id == id) timer.running = false;
  }

  void stopAllTimers() {
    for (Timer& timer : timers_) timer.running = false;
  }

  bool isTimerRunning(int id) const {
    for (const Timer& timer : timers_)
      if (timer.id == id) return timer.running;
    return false;
  }

  double intervalFor(int id) const {
    for (const Timer& timer : timers_)
      if (timer.id == id) return timer.interval;
    return 0.0;
  }

  int numTimers() const {
    int running = 0;
    for (const Timer& timer : timers_)
      if (timer.running) ++running;
    return running;
  }

  /**
   * Move every running timer forward, and fire whatever came due.
   *
   * Returns how many callbacks it made, which is what a caller uses to decide
   * whether anything needs repainting -- and what makes "nothing happened"
   * measurable rather than assumed.
   *
   * A timer that is more than one interval behind fires ONCE. See the header:
   * catching up would give sixty repaints after a two-second stall, and for a
   * blinking light sixty state changes nobody sees.
   */
  int advance(double elapsedMs) {
    if (elapsedMs <= 0.0) return 0;
    int fired = 0;
    // Over a COPY of the ids, because a callback may start or stop a timer --
    // an editor closing a panel from inside its own tick is the ordinary case
    // -- and that can reallocate the vector underneath the loop.
    const size_t count = timers_.size();
    for (size_t i = 0; i < count && i < timers_.size(); ++i) {
      if (!timers_[i].running) continue;
      timers_[i].elapsed += elapsedMs;
      if (timers_[i].elapsed < timers_[i].interval) continue;
      timers_[i].elapsed = 0.0;
      const int id = timers_[i].id;
      ++fired;
      if (onTimer) onTimer(id);
    }
    return fired;
  }

private:
  struct Timer {
    int id = 0;
    double interval = 0.0;
    double elapsed = 0.0;
    bool running = true;
  };

  std::vector<Timer> timers_;
};

} // namespace sonore
