// SPDX-License-Identifier: Apache-2.0
//
// ThreadWithProgressWindow: a job on a thread of its own, with a window that
// shows how far it has got and a button that asks it to stop.
//
// ── For an application, not a plugin ──────────────────────────────────────
//
// A plugin must not open a window of its own over the host (see
// message_box.h); a sampler loading a large file composes ThreadPool, Overlay
// and ProgressBar inside its editor instead. This is the reference's object
// for programs that own their process.
//
// ── How the pieces talk ────────────────────────────────────────────────────
//
// The job runs run() on a std::thread and reports through setProgress() and
// setStatusMessage(), which only store (atomically, or under a small lock):
// the window copies them out on its own 33 ms clock, on the message thread,
// because a component may only be touched there. Cancel raises the job's
// ThreadExitSignal -- the same one a MessageManagerLock taken by the job gives
// up on, so a job blocked on the message thread is not a deadlock either.
//
// When run() returns, the thread queues the ending on the message thread: the
// window goes, and threadComplete(userPressedCancel) is called from the queue
// rather than from inside the window's clock or the button's click, so it may
// delete this object (the reference's usual "delete this" in threadComplete).
//
// A job that ignores the request to stop cannot be killed -- a std::thread
// has no safe way to be -- so after timeOutMsWhenCancelling the WINDOW goes,
// and that is all. threadComplete(true) still waits for run() to return:
// it is where this object may be deleted, and run() is running on it. Called
// "anyway", as it once was, a `delete this` there freed what the job was
// still using (AddressSanitizer: heap-use-after-free in run()). runThread
// likewise returns only once run() has.
//
// RUN on Windows: a modal runThread to the end with the bar checked in the
// window's own pixels; a launched job cancelled by a click on the drawn
// button; and the time-out. The window is a DocumentWindow, so it is compiled
// for macOS and X11 with the rest.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "../message_manager.h"
#include "containers.h"
#include "document_window.h"
#include "widgets.h"

namespace sonore {
namespace gfx {

namespace detail {
namespace progress {

/** The window: a status line, the bar, and Cancel. */
class Window : public DocumentWindow {
public:
  Window(const std::string& title, bool hasBar, bool hasCancel, const std::string& cancelText)
      : DocumentWindow(title, LookAndFeel::defaultLookAndFeel().background(), 0),
        hasBar_(hasBar), hasCancel_(hasCancel), cancel_(cancelText) {
    status_.setJustify(Justify::Left);
    content_.addChild(&status_);
    if (hasBar_) content_.addChild(&bar_);
    if (hasCancel_) content_.addChild(&cancel_);
    bar_.setSource(&shown_);
    setContentNonOwned(&content_, false);
  }

  ~Window() override {
    removeFromDesktop();
    clearContentComponent();
  }

  /** Never closed by its frame: the close request is a cancel. */
  std::function<void()> onCancel;

  void closeButtonPressed() override {
    if (hasCancel_ && onCancel) onCancel();
  }

  void resized() override {
    DocumentWindow::resized();
    const Rect b = content_.localBounds();
    const float pad = 14.0f;
    status_.setBounds(Rect(pad, pad, b.w - pad * 2.0f, 20.0f));
    bar_.setBounds(Rect(pad, pad + 28.0f, b.w - pad * 2.0f, 16.0f));
    const float bw = 96.0f, bh = 26.0f;
    cancel_.setBounds(Rect(b.w - pad - bw, b.h - pad - bh, bw, bh));
  }

  /** Copy what the job reported into the components. Message thread. */
  void show(double progress, const std::string& status) {
    shown_ = progress;
    bar_.tick();
    status_.setText(status);
  }

  ProgressBar& progressBar() { return bar_; }
  Label& statusLabel() { return status_; }
  Button& cancelButton() { return cancel_; }

  std::function<void()> onWindowTick;
  void windowTick(double) override {
    if (onWindowTick) onWindowTick();
  }

private:
  bool hasBar_, hasCancel_;
  Widget content_;
  Label status_;
  ProgressBar bar_;
  Button cancel_;
  double shown_ = 0.0;
};

} // namespace progress
} // namespace detail

/**
 * Run run() on a thread while a window shows its progress.
 *
 * runThread() waits (pumping the message loop) and says whether the job got
 * to the end; launchThread() returns at once and threadComplete() says it
 * later. Message thread, except where a method says otherwise.
 */
class ThreadWithProgressWindow {
public:
  /**
   * Nothing runs and no window shows until runThread() or launchThread().
   * Without `hasProgressBar` the window has only the status line; without
   * `hasCancelButton` it has no Cancel and its close box does nothing.
   * `timeOutMsWhenCancelling` is how long after Cancel, in milliseconds, the
   * window waits for run() to return before it goes anyway. The window is
   * centred over the TopLevelWindow `componentToCentreAround` is in, or on
   * the main display when that is null or in no window; not owned, and read
   * each time the window opens.
   */
  ThreadWithProgressWindow(const std::string& windowTitle, bool hasProgressBar,
                           bool hasCancelButton, int timeOutMsWhenCancelling = 10000,
                           const std::string& cancelButtonText = "Cancel",
                           Component* componentToCentreAround = nullptr)
      : title_(windowTitle), hasBar_(hasProgressBar), hasCancel_(hasCancelButton),
        timeoutMs_(timeOutMsWhenCancelling), cancelText_(cancelButtonText),
        centreAround_(componentToCentreAround) {}

  /** Stops the job (asks, then waits for it). */
  virtual ~ThreadWithProgressWindow() {
    alive_.reset();
    exit_.signalThreadShouldExit();
    if (thread_.joinable()) thread_.join();
    window_.reset();
  }

  /** Not copyable: it owns the job's thread and its window. */
  ThreadWithProgressWindow(const ThreadWithProgressWindow&) = delete;
  /** Not assignable, for the same reason. */
  ThreadWithProgressWindow& operator=(const ThreadWithProgressWindow&) = delete;

  /** The job. On its own thread; check threadShouldExit() often. */
  virtual void run() = 0;

  /** The job has ended -- or was given up on after the time-out. Message
   *  thread, from the queue: this may delete the object. */
  virtual void threadComplete(bool userPressedCancel) { (void) userPressedCancel; }

  /**
   * Show the window, run the job and wait for it, pumping the message loop.
   * True when run() returned without Cancel having been pressed.
   */
  bool runThread() {
    std::shared_ptr<int> result = std::make_shared<int>(-1);
    result_ = result;
    launchThread();
    MessageManager* mm = MessageManager::getInstance();
    // The result is kept outside this object: threadComplete may delete it.
    while (*result < 0) {
      if (!mm->runDispatchLoopUntil(10)) break;
    }
    return *result == 1;
  }

  /** Show the window and start the job; returns at once. During a run --
   *  or before its ending has been delivered -- it stops that run (asks,
   *  then waits for it) and starts again: the run it replaced gets no
   *  threadComplete, the new one does. */
  void launchThread() {
    if (thread_.joinable()) {
      exit_.signalThreadShouldExit();
      thread_.join();
    }
    exit_.reset();
    finished_ = false;
    completed_ = false;
    cancelled_ = false;
    progress_.store(hasBar_ ? 0.0 : -1.0);
    // Which run this is. The old run's ending may already be in the queue,
    // and delivered now it closed THIS run's window and spent its completion.
    const uint64_t run = ++run_;
    openWindow();
    std::weak_ptr<char> token = alive_;
    ThreadWithProgressWindow* self = this;
    thread_ = std::thread([self, token, run]() {
      try {
        self->run();
      } catch (const std::exception& e) {
        // Reported on the message thread, where the application's hook runs.
        const std::string what = e.what();
        MessageManager::callAsync([what]() {
          const std::runtime_error error(what);
          MessageManager::sendUnhandledException(&error);
        });
      } catch (...) {
        MessageManager::callAsync([]() { MessageManager::sendUnhandledException(nullptr); });
      }
      self->finished_.store(true);
      // The ending, on the message thread. `self` is read there only while
      // the token says the object lives.
      MessageManager::callAsync([self, token, run]() {
        if (!token.expired()) self->complete(run);
      });
    });
  }

  // ── From the job's thread ─────────────────────────────────────────────────

  /** 0..1; negative shows an indeterminate bar. Any thread. */
  void setProgress(double newProgress) { progress_.store(newProgress); }
  /** The line above the bar. Any thread. */
  void setStatusMessage(const std::string& message) {
    std::lock_guard<std::mutex> lock(statusMutex_);
    status_ = message;
  }
  /** True once Cancel was pressed (or the object is being destroyed). */
  bool threadShouldExit() const { return exit_.threadShouldExit(); }
  /** For a MessageManagerLock the job takes: it gives up when cancelled. */
  ThreadExitSignal& getExitSignal() { return exit_; }

  // ── State ────────────────────────────────────────────────────────────────

  /** run() has been started by launchThread() and has not returned yet. */
  bool isThreadRunning() const { return thread_.joinable() && !finished_.load(); }
  /** Cancel was pressed, or cancel() called, in this run; launchThread()
   *  clears it. */
  bool wasCancelled() const { return cancelled_; }

  /** The window while it is up, for a test or a theme; null otherwise. */
  DocumentWindow* getProgressWindow() { return window_.get(); }
  /** The same window with its status label, bar and Cancel button reachable,
   *  for a test; null while no window is up. */
  detail::progress::Window* getWindowDetail() { return window_.get(); }

  /** Press Cancel from code. */
  void cancel() { cancelPressed(); }

private:
  void openWindow() {
    window_.reset(new detail::progress::Window(title_, hasBar_, hasCancel_, cancelText_));
    window_->onCancel = [this]() { cancelPressed(); };
    window_->cancelButton().onClick = [this]() { cancelPressed(); };
    window_->onWindowTick = [this]() { refresh(); };
    const float w = 400.0f, h = hasBar_ ? 124.0f : 96.0f;
    if (TopLevelWindow* around = TopLevelWindow::findTopLevelWindowFor(centreAround_)) {
      const Rect r = around->getWindowBounds();
      window_->setWindowBounds(Rect(r.x + (r.w - w) * 0.5f, r.y + (r.h - h) * 0.5f, w, h));
    } else {
      window_->centreWithSize(w, h);
    }
    window_->setAlwaysOnTop(true);
    refresh();
    window_->setVisible(true);
  }

  void refresh() {
    if (!window_) return;
    std::string status;
    {
      std::lock_guard<std::mutex> lock(statusMutex_);
      status = status_;
    }
    window_->show(progress_.load(), status);
    // Given up on: the job ignored Cancel for longer than it was allowed.
    // The window goes (from the queue: this is its own tick); completion
    // waits for the job, see the header.
    if (cancelled_ && !completed_ && !finished_.load() &&
        std::chrono::steady_clock::now() - cancelledAt_ >= std::chrono::milliseconds(timeoutMs_)) {
      std::weak_ptr<char> token = alive_;
      ThreadWithProgressWindow* self = this;
      const uint64_t run = run_;
      MessageManager::callAsync([self, token, run]() {
        if (token.expired() || run != self->run_ || !self->window_) return;
        self->window_->setVisible(false);
        self->window_.reset();
      });
    }
  }

  void cancelPressed() {
    if (cancelled_ || completed_) return;
    cancelled_ = true;
    cancelledAt_ = std::chrono::steady_clock::now();
    exit_.signalThreadShouldExit();
    if (window_) window_->cancelButton().setEnabled(false);
  }

  /** Once per run: the window goes, the answer is recorded, threadComplete
   *  runs. Not for a run launchThread() has since replaced. */
  void complete(uint64_t run) {
    if (run != run_ || completed_) return;
    completed_ = true;
    if (finished_.load() && thread_.joinable()) thread_.join();
    if (window_) window_->setVisible(false);
    window_.reset();
    const bool cancelled = cancelled_;
    if (std::shared_ptr<int> result = result_.lock()) *result = cancelled ? 0 : 1;
    threadComplete(cancelled); // last: it may delete this
  }

  std::string title_;
  bool hasBar_, hasCancel_;
  int timeoutMs_;
  std::string cancelText_;
  Component* centreAround_;

  std::thread thread_;
  ThreadExitSignal exit_;
  std::atomic<double> progress_{0.0};
  std::mutex statusMutex_;
  std::string status_;
  std::atomic<bool> finished_{false};
  uint64_t run_ = 0; // launches so far: what a queued ending is checked against
  bool completed_ = false;
  bool cancelled_ = false;
  std::chrono::steady_clock::time_point cancelledAt_{};
  std::unique_ptr<detail::progress::Window> window_;
  std::weak_ptr<int> result_;
  std::shared_ptr<char> alive_ = std::make_shared<char>('\0');
};

} // namespace gfx
} // namespace sonore
