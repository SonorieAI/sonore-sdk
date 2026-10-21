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
#include <memory>
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
  /** Does NOT deregister: remove it from every broadcaster first. */
  virtual ~ChangeListener() = default;
  /** `source` changed, on the thread that dispatched it -- the UI thread,
   *  as ChangeBroadcaster says. */
  virtual void changeListenerCallback(class ChangeBroadcaster* source) = 0;
};

/**
 * A list of ChangeListeners, told that something changed: at once
 * (sendSynchronousChangeMessage) or once per dispatch however many changes
 * were marked (sendChangeMessage). Mirrors the reference's class of the same
 * name; see the file comment for why the coalescing form is the one that
 * matters.
 *
 * Nothing dispatches by itself, unlike there: whoever owns the broadcaster
 * calls dispatchPendingChanges() from a UI-thread tick. sendChangeMessage is
 * the only call safe from any thread; registration and both deliveries
 * belong to the UI thread. Listeners are not owned -- one that is destroyed
 * removes itself first. A listener may remove itself or another, or delete
 * the broadcaster, from inside its callback; after a delete the rest are not
 * called. Usually a base class, as FileBasedDocument and PropertiesFile use
 * it.
 */
class ChangeBroadcaster {
public:
  /** Calls nobody. A delete from inside a listener's callback stops the
   *  delivery in progress safely. */
  virtual ~ChangeBroadcaster() = default;

  /** Registers `listener`, not owned; it stays until removed. Null, or a
   *  listener already registered, is ignored. UI thread. */
  void addChangeListener(ChangeListener* listener) {
    if (!listener) return;
    // Once only. A listener registered twice is told twice, and the second
    // removal leaves the first registration behind -- which is a dangling
    // pointer that only fires on the change AFTER the one that removed it.
    if (std::find(listeners_.begin(), listeners_.end(), listener) != listeners_.end()) return;
    listeners_.push_back(listener);
  }

  /** Unregisters `listener`; one not registered is ignored. Safe from inside
   *  a callback, its own included. UI thread. */
  void removeChangeListener(ChangeListener* listener) {
    listeners_.erase(std::remove(listeners_.begin(), listeners_.end(), listener),
                     listeners_.end());
  }

  /** Unregisters every listener. UI thread. */
  void removeAllChangeListeners() { listeners_.clear(); }

  /** How many listeners are registered. */
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

  /** True when sendChangeMessage() was called since the last delivery. Any
   *  thread; a lock-free load. */
  bool hasPendingChange() const { return pending_.load(std::memory_order_acquire); }

  /**
   * Deliver a pending change, if there is one. Returns whether it delivered.
   *
   * The owner calls this from a UI-thread tick: an editor's editorTick(), a
   * NativeWindow's onTick, an application's timer. Nothing in the SDK calls it
   * by itself -- sendChangeMessage has to stay safe on the audio thread, so it
   * can only set a flag, and someone has to look. That way a broadcaster needs
   * no timer of its own, and a change from any thread arrives on the one
   * thread components may be touched from.
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
    // A listener may also DELETE the broadcaster: a player dropping its
    // transport once told it has stopped, a window closing its document. The
    // next turn of the loop read the list of the destroyed object -- freed
    // memory, or whatever had been built there since, whose listeners were
    // then told of a change it never made.
    const std::weak_ptr<char> alive = lifetime_;
    for (ChangeListener* listener : copy) {
      if (alive.expired()) return;
      // Still subscribed? One listener's callback can remove another, and the
      // copy would call the removed one anyway.
      if (std::find(listeners_.begin(), listeners_.end(), listener) == listeners_.end()) continue;
      listener->changeListenerCallback(this);
    }
  }

  std::vector<ChangeListener*> listeners_;
  std::atomic<bool> pending_{false};
  /** Gone with this object: how notify() knows a callback deleted it. */
  std::shared_ptr<char> lifetime_ = std::make_shared<char>('\0');
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
  /** Calls `onChange` for every change it hears; an empty function makes a
   *  listener that does nothing. */
  explicit LambdaChangeListener(std::function<void()> onChange)
      : onChange_(std::move(onChange)) {}

  /** Calls the function; which broadcaster changed is not passed on. */
  void changeListenerCallback(ChangeBroadcaster*) override {
    if (onChange_) onChange_();
  }

private:
  std::function<void()> onChange_;
};

} // namespace sonore
