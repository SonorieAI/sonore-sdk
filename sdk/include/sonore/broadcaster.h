// SPDX-License-Identifier: Apache-2.0
//
// "Something changed": told to whoever asked, without either side knowing the
// other.
//
// ── What it is for ──────────────────────────────────────────────────────────
//
// A preset was loaded, and four parts of the editor need to redraw. A sample
// finished loading on a worker thread, and the waveform, the length readout and
// the keyboard map all care. Without something like this, whatever performs the
// change has to hold a pointer to every part of the interface that reacts to
// it -- which is how a DSP object ends up including a header for a scroll bar.
//
// ── The important half is the COALESCING ───────────────────────────────────
//
// A synchronous callback is four lines and nobody needs a class for it. What is
// actually worth having is the asynchronous form: mark that something changed,
// as often as you like, and have the listeners told ONCE on the next UI tick.
//
// That is the difference between a preset load notifying forty parameter
// listeners forty times -- each one repainting -- and notifying them once. The
// synchronous version of a "reload everything" handler called forty times is
// the classic reason loading a preset takes a visible second.
//
// ── What it is not ──────────────────────────────────────────────────────────
//
// Not thread-safe for REGISTRATION. Listeners are added and removed on the UI
// thread, which is where components live, and a mutex around that would be
// protecting a race nobody has. `markChanged` IS callable from another thread:
// it sets one atomic flag and nothing else, so a worker finishing a load can
// say so without touching the listener list.
#pragma once

#include <algorithm>
#include <atomic>
#include <functional>
#include <vector>

namespace sonore {

/**
 * Anything that wants to hear about a change.
 *
 * A plain interface rather than a std::function, because a listener has to be
 * REMOVABLE -- and two std::functions cannot be compared, so a list of them can
 * be added to and never pruned. A component that outlived its subscription
 * would be called through a dangling pointer, which is the one failure this
 * class exists to make impossible.
 */
class ChangeListener {
public:
  virtual ~ChangeListener() = default;
  virtual void changeListenerCallback(class ChangeBroadcaster* source) = 0;
};

class ChangeBroadcaster {
public:
  virtual ~ChangeBroadcaster() = default;

  void addChangeListener(ChangeListener* listener) {
    if (!listener) return;
    // Once only. A listener registered twice is told twice, and the second
    // removal leaves the first registration behind -- which is a dangling
    // pointer that only fires on the change AFTER the one that removed it.
    if (std::find(listeners_.begin(), listeners_.end(), listener) != listeners_.end()) return;
    listeners_.push_back(listener);
  }

  void removeChangeListener(ChangeListener* listener) {
    listeners_.erase(std::remove(listeners_.begin(), listeners_.end(), listener),
                     listeners_.end());
  }

  void removeAllChangeListeners() { listeners_.clear(); }

  int numChangeListeners() const { return (int) listeners_.size(); }

  /**
   * Tell everyone NOW.
   *
   * For a change whose listeners must have seen it before the next line runs.
   * Rarer than it looks -- most callers want the coalescing form below.
   */
  void sendSynchronousChangeMessage() {
    pending_.store(false, std::memory_order_release);
    notify();
  }

  /**
   * Mark that something changed. Listeners hear about it on the next
   * dispatchPendingChanges(), once, however many times this was called.
   *
   * Callable from any thread: it sets one atomic flag and touches nothing
   * else, so a worker finishing a load can say so without the listener list
   * needing a lock it does not otherwise want.
   */
  void sendChangeMessage() { pending_.store(true, std::memory_order_release); }

  bool hasPendingChange() const { return pending_.load(std::memory_order_acquire); }

  /**
   * Deliver a pending change, if there is one. Returns whether it delivered.
   *
   * Called from the editor's clock -- the same 33 ms tick that already syncs
   * parameters -- so a broadcaster needs no timer of its own and a change from
   * any thread arrives on the one thread components may be touched from.
   */
  bool dispatchPendingChanges() {
    if (!pending_.exchange(false, std::memory_order_acq_rel)) return false;
    notify();
    return true;
  }

private:
  void notify() {
    // Over a COPY. A listener may remove itself -- a panel closing because of
    // the very change it was told about is the ordinary case -- and iterating
    // the live vector while it is edited underneath is the crash that follows.
    const std::vector<ChangeListener*> copy = listeners_;
    for (ChangeListener* listener : copy) {
      // Still subscribed? One listener's callback can remove another, and the
      // copy would call the removed one anyway.
      if (std::find(listeners_.begin(), listeners_.end(), listener) == listeners_.end()) continue;
      listener->changeListenerCallback(this);
    }
  }

  std::vector<ChangeListener*> listeners_;
  std::atomic<bool> pending_{false};
};

/**
 * A listener made from a lambda, for the common case where the thing reacting
 * is not a class that wants a base.
 *
 * It has to be OWNED by whoever it belongs to -- a member, not a temporary --
 * because the broadcaster holds a pointer. That is the same rule every
 * listener has and the reason this is a named object rather than an overload
 * of addChangeListener taking a function.
 */
class LambdaChangeListener : public ChangeListener {
public:
  explicit LambdaChangeListener(std::function<void()> onChange)
      : onChange_(std::move(onChange)) {}

  void changeListenerCallback(ChangeBroadcaster*) override {
    if (onChange_) onChange_();
  }

private:
  std::function<void()> onChange_;
};

} // namespace sonore
