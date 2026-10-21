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
// Driven by its owner: advance() from an editor's editorTick(), a NativeWindow's
// onTick or an application's timer. Nothing in the SDK calls it by itself.
//
// ── Catching up, and deciding not to ───────────────────────────────────────
//
// A host that stalls for two seconds leaves a 30 Hz timer sixty ticks behind.
// Firing sixty times to catch up is almost never what anybody wants: sixty
// repaints back to back, and for a blinking light sixty state changes nobody
// sees. So a late timer fires ONCE and resets. That is a decision rather than
// an oversight, and it is the one a caller most needs to know about.
#pragma once
#include <cmath>

#include <functional>
#include <memory>
#include <vector>

namespace sonore {

/**
 * Several timers in one object, each known by an int id and all reported
 * through one onTimer callback. Mirrors the reference's class of the same
 * name, with the difference that matters: it has no clock of its own. Its
 * owner calls advance() with the milliseconds that passed -- from the
 * editor's tick, or from a test -- and the callbacks run inside that call.
 * See the file comment for why, and for why a late timer fires once rather
 * than catching up.
 *
 * Intervals are in milliseconds. One thread, the one calling advance(),
 * usually the UI thread: nothing here is locked. A callback may start and
 * stop timers, its own included, or delete the MultiTimer; nothing else
 * fires in that advance() then.
 */
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

  /** Stops timer `id`; it keeps its interval and can be started again. An
   *  unknown id does nothing. */
  void stopTimer(int id) {
    for (Timer& timer : timers_)
      if (timer.id == id) timer.running = false;
  }

  /** Stops every timer. */
  void stopAllTimers() {
    for (Timer& timer : timers_) timer.running = false;
  }

  /** True while timer `id` is started; false once stopped, and for an
   *  unknown id. */
  bool isTimerRunning(int id) const {
    for (const Timer& timer : timers_)
      if (timer.id == id) return timer.running;
    return false;
  }

  /** Timer `id`'s interval in milliseconds, running or stopped; 0 for an id
   *  never started. */
  double intervalFor(int id) const {
    for (const Timer& timer : timers_)
      if (timer.id == id) return timer.interval;
    return 0.0;
  }

  /** How many timers are RUNNING; stopped ones are not counted. */
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
    // A callback may also DELETE this object -- a splash or a notice closing
    // itself on its own timer, the window that owns the MultiTimer going with
    // it. The loop then read the dead object's timers: freed memory, or
    // another MultiTimer built there since, whose timers it advanced and
    // fired from a call that was never made on it.
    const std::weak_ptr<char> alive = lifetime_.token;
    // By index, never by reference or iterator, and only over the timers
    // there were at the start: a callback may start or stop a timer -- an
    // editor closing a panel from inside its own tick is the ordinary case
    // -- and that can reallocate the vector underneath the loop.
    const size_t count = timers_.size();
    for (size_t i = 0; i < count && i < timers_.size(); ++i) {
      if (!timers_[i].running) continue;
      timers_[i].elapsed += elapsedMs;
      if (timers_[i].elapsed < timers_[i].interval) continue;
      // The REMAINDER carries over -- and only the remainder, so a stall
      // still fires once. Zeroing it threw away the part of the tick past the
      // deadline: a 100 ms timer on the 33 ms editor clock fired every fourth
      // tick, 132 ms, and a 30 Hz one at 15 Hz.
      timers_[i].elapsed = std::fmod(timers_[i].elapsed - timers_[i].interval,
                                     timers_[i].interval);
      const int id = timers_[i].id;
      ++fired;
      if (onTimer) onTimer(id);
      if (alive.expired()) break;
    }
    return fired;
  }

private:
  /** Gone with this object: how advance() knows a callback deleted it. A
   *  copy of a MultiTimer gets a token of its own: one shared with a copy
   *  would not expire when this object is deleted. */
  struct Lifetime {
    std::shared_ptr<char> token = std::make_shared<char>('\0'); ///< expires with this object
    /** A fresh token. */
    Lifetime() = default;
    /** A fresh token, not the other's: see above. */
    Lifetime(const Lifetime&) {}
    /** Keeps this object's own token. */
    Lifetime& operator=(const Lifetime&) { return *this; }
  };

  /** One timer: its interval, how far it is into the current one, and
   *  whether it runs. A stopped timer keeps its slot and its interval, which
   *  intervalFor() still reports. */
  struct Timer {
    int id = 0;            ///< the caller's id for it
    double interval = 0.0; ///< milliseconds, always above 0
    double elapsed = 0.0;  ///< milliseconds into the current interval
    bool running = true;   ///< false once stopped
  };

  std::vector<Timer> timers_;
  Lifetime lifetime_;
};

} // namespace sonore
