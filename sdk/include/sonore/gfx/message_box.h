// SPDX-License-Identifier: Apache-2.0
//
// The operating system's own message boxes, for an application: a message,
// an icon, and up to any number of buttons, answered synchronously or through
// a callback, and closable from code.
//
// ── Not inside a plugin ────────────────────────────────────────────────────
//
// An OS modal dialog from inside a plugin blocks the HOST's message loop, can
// land behind the host's window, and leaves the user unable to reach the DAW
// to stop the transport. A plugin editor asks its questions with gfx/alert.h's
// AlertOverlay, which is drawn inside the editor and blocks nothing. This file
// is for programs that own their process: Application, DocumentWindow.
//
// ── Per platform ───────────────────────────────────────────────────────────
//
//   Windows  TaskDialogIndirect, which takes the buttons' own labels. It lives
//            in Common Controls 6, which a process gets only through an
//            activation context: when the program's manifest gave it none,
//            one is made from the manifest shell32 carries (resource 124).
//            Without it, MessageBoxW -- whose buttons cannot be relabelled,
//            so OK / OK-Cancel / Yes-No / Yes-No-Cancel are picked to suit.
//   macOS    NSAlert, with the labels, through the Objective-C runtime.
//   Linux    zenity, else kdialog, run as a child process -- the desktop's
//            own dialog without linking a toolkit.
//   Anywhere else, or when none of those can show this box (more than three
//   buttons for MessageBoxW or kdialog, no zenity or kdialog on the PATH): a
//   DRAWN box -- a small DocumentWindow of this SDK's holding an AlertOverlay.
//
// ── Synchronous and asynchronous ───────────────────────────────────────────
//
// show() waits, running the OS's modal loop on the calling thread (which
// keeps the message thread's queue moving: callAsync still runs). showAsync()
// returns at once and calls back on the message thread with the index of the
// button pressed; on Windows and Linux the box runs on a thread of its own, so
// nothing about the caller's loop is held. showScopedAsync() returns a
// ScopedMessageBox: destroying it, or calling close(), takes the box down and
// the callback is NOT called. Closing the box from its own close box or
// Escape answers with the LAST button (the cancel, as AlertOverlay has it).
// -1 means no box could be shown at all.
//
// RUN on Windows: TaskDialog, MessageBoxW and the drawn box, each async and
// dismissed by a posted click; the sync form dismissed from another thread;
// ScopedMessageBox closing a box and suppressing its callback. NSAlert and
// zenity/kdialog are compiled only.
#pragma once

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../message_manager.h"
#include "alert.h"
#include "document_window.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <cwchar>
#if defined(_MSC_VER)
#pragma comment(lib, "user32.lib")
#endif
#elif defined(__APPLE__)
#include <objc/message.h>
#include <objc/runtime.h>
#else
#include <cerrno>
#include <csignal>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace sonore {
namespace gfx {

/** Which icon a box shows. The reference's names. Windows shows each of the
 *  three; NSAlert, zenity and kdialog tell only a warning from the rest; the
 *  drawn box writes the word ("Warning", "Question", "Information") as its
 *  heading. */
enum class MessageBoxIconType {
  /** No icon. */
  NoIcon,
  /** A question mark: the system's on Windows. */
  QuestionIcon,
  /** The platform's warning icon or alert style. */
  WarningIcon,
  /** The platform's information icon. */
  InfoIcon
};

/**
 * What a box says and which buttons it has. Built by chaining, as in the
 * reference: MessageBoxOptions().withTitle("Save?").withButton("Save")...
 */
class MessageBoxOptions {
public:
  /** A copy with this icon; NoIcon by default. */
  MessageBoxOptions withIconType(MessageBoxIconType type) const {
    MessageBoxOptions o = *this;
    o.icon_ = type;
    return o;
  }
  /** A copy with this title: the window's caption (NSAlert's bold message
   *  text on macOS). UTF-8. */
  MessageBoxOptions withTitle(const std::string& title) const {
    MessageBoxOptions o = *this;
    o.title_ = title;
    return o;
  }
  /** A copy with this message, the box's body text. UTF-8. */
  MessageBoxOptions withMessage(const std::string& message) const {
    MessageBoxOptions o = *this;
    o.message_ = message;
    return o;
  }
  /** A copy with one more button, after those already added. Buttons read
   *  left to right and are answered by 0-based index; the first is the
   *  default, and the last is what the close box and Escape answer. With
   *  none added, the box has one OK button. */
  MessageBoxOptions withButton(const std::string& text) const {
    MessageBoxOptions o = *this;
    o.buttons_.push_back(text);
    return o;
  }
  /** The box is modal to this component's window (and centred on it). */
  MessageBoxOptions withAssociatedComponent(Component* component) const {
    MessageBoxOptions o = *this;
    o.associated_ = component;
    return o;
  }

  /** The icon withIconType set. */
  MessageBoxIconType getIconType() const { return icon_; }
  /** The title withTitle set, or empty. */
  const std::string& getTitle() const { return title_; }
  /** The message withMessage set, or empty. */
  const std::string& getMessage() const { return message_; }
  /** The buttons added, 0 when none was (the box then shows OK). */
  int getNumButtons() const { return (int) buttons_.size(); }
  /** A button's label by 0-based index; empty when out of range. */
  std::string getButtonText(int index) const {
    return index >= 0 && index < (int) buttons_.size() ? buttons_[(size_t) index] : std::string();
  }
  /** The component whose window owns the box, or null. Not owned. */
  Component* getAssociatedComponent() const { return associated_; }

  /** One button, labelled `buttonText`: answer 0. */
  static MessageBoxOptions makeOptionsOk(MessageBoxIconType icon, const std::string& title,
                                         const std::string& message,
                                         const std::string& buttonText = "OK",
                                         Component* associated = nullptr) {
    return MessageBoxOptions().withIconType(icon).withTitle(title).withMessage(message)
        .withButton(buttonText).withAssociatedComponent(associated);
  }
  /** Two buttons: `button1` answers 0, `button2` (the cancel) answers 1. */
  static MessageBoxOptions makeOptionsOkCancel(MessageBoxIconType icon, const std::string& title,
                                               const std::string& message,
                                               const std::string& button1 = "OK",
                                               const std::string& button2 = "Cancel",
                                               Component* associated = nullptr) {
    return makeOptionsOk(icon, title, message, button1, associated).withButton(button2);
  }
  /** Two buttons: `button1` (Yes) answers 0, `button2` (No) answers 1, and
   *  the close box answers No. */
  static MessageBoxOptions makeOptionsYesNo(MessageBoxIconType icon, const std::string& title,
                                            const std::string& message,
                                            const std::string& button1 = "Yes",
                                            const std::string& button2 = "No",
                                            Component* associated = nullptr) {
    return makeOptionsOk(icon, title, message, button1, associated).withButton(button2);
  }
  /** Three buttons answering 0 (Yes), 1 (No) and 2 (Cancel, which the close
   *  box answers too). */
  static MessageBoxOptions makeOptionsYesNoCancel(MessageBoxIconType icon,
                                                  const std::string& title,
                                                  const std::string& message,
                                                  const std::string& button1 = "Yes",
                                                  const std::string& button2 = "No",
                                                  const std::string& button3 = "Cancel",
                                                  Component* associated = nullptr) {
    return makeOptionsYesNo(icon, title, message, button1, button2, associated).withButton(button3);
  }

private:
  MessageBoxIconType icon_ = MessageBoxIconType::NoIcon;
  std::string title_, message_;
  std::vector<std::string> buttons_;
  Component* associated_ = nullptr;
};

namespace detail {
namespace msgbox {

/** Which implementation answers. Automatic picks as the header says; the
 *  others exist so a test can reach each path on one machine. */
enum class Backend { automatic, taskDialog, classic, drawn };

inline std::atomic<Backend>& forcedBackend() {
  static std::atomic<Backend> backend{Backend::automatic};
  return backend;
}

class DrawnBox;

/**
 * One box in flight, shared by the caller's handle (ScopedMessageBox), the
 * thread or queue running it, and the callback. A box closed from code is
 * marked dismissed BEFORE it is taken down, so its answer -- whichever
 * arrives -- is dropped.
 */
struct Session {
  std::mutex mutex;
  bool dismissed = false;
  bool finished = false;
  std::function<void(int)> callback;
#if defined(_WIN32)
  HWND dialog = nullptr; // set once the box exists, on its own thread
  bool taskDialog = false;
#elif defined(__APPLE__)
  bool running = false; // inside runModal
#else
  pid_t child = -1;
#endif
  std::unique_ptr<DrawnBox> drawn; // message thread only
};

/** Deliver an answer on the message thread, unless the box was closed from
 *  code. Without a MessageManager it is delivered where it is. */
inline void deliver(const std::shared_ptr<Session>& session, int result) {
  {
    std::lock_guard<std::mutex> lock(session->mutex);
    session->finished = true;
#if defined(_WIN32)
    session->dialog = nullptr;
#endif
    if (session->dismissed) return;
  }
  auto run = [session, result]() {
    std::function<void(int)> callback;
    {
      std::lock_guard<std::mutex> lock(session->mutex);
      if (session->dismissed) return;
      callback = std::move(session->callback);
      session->callback = nullptr;
    }
    if (callback) callback(result);
  };
  if (MessageManager::getInstanceWithoutCreating() && MessageManager::callAsync(run)) return;
  run();
}

/**
 * The drawn box: a DocumentWindow with an AlertOverlay filling it. Used where
 * the platform has nothing that can show the box. Its answer is reported
 * through the queue; the window is deleted there too, never from inside the
 * click that answered.
 */
class DrawnBox : public DocumentWindow {
public:
  DrawnBox(const MessageBoxOptions& options, std::function<void(int)> onAnswer)
      : DocumentWindow(options.getTitle().empty() ? std::string("Message") : options.getTitle(),
                       LookAndFeel::defaultLookAndFeel().background(), closeButton),
        onAnswer_(std::move(onAnswer)) {
    buttons_ = (std::max)(1, options.getNumButtons());
    setContentNonOwned(&holder_, false);
    holder_.addChild(&alert_);
    alert_.setDimAlpha(0);
    std::vector<std::string> labels;
    for (int i = 0; i < options.getNumButtons(); ++i) labels.push_back(options.getButtonText(i));
    if (labels.empty()) labels.push_back("OK");
    std::string iconText;
    switch (options.getIconType()) {
      case MessageBoxIconType::WarningIcon: iconText = "Warning"; break;
      case MessageBoxIconType::QuestionIcon: iconText = "Question"; break;
      case MessageBoxIconType::InfoIcon: iconText = "Information"; break;
      case MessageBoxIconType::NoIcon: break;
    }
    // Sized to the panel the overlay will draw: shown once at a generous
    // size, measured, and the window fitted around it.
    holder_.setBounds(Rect(0.0f, 0.0f, kWidth, 1000.0f));
    alert_.show(iconText, options.getMessage(), labels, [this](int index) { answer(index); });
    const Rect panel = alert_.panelBounds();
    setAlwaysOnTop(true);
    centreWithSize(kWidth, panel.h + 24.0f);
    alert_.show(iconText, options.getMessage(), labels, [this](int index) { answer(index); });
  }

  ~DrawnBox() override {
    removeFromDesktop();
    clearContentComponent(); // holder_ is ours, and goes before the base does
  }

  void resized() override {
    DocumentWindow::resized();
    alert_.coverParent();
    alert_.resized();
  }

  /** The window's close box answers with the last button. */
  void closeButtonPressed() override { answer(buttons_ - 1); }

  AlertOverlay& alert() { return alert_; }

private:
  void answer(int index) {
    if (answered_) return;
    answered_ = true;
    std::function<void(int)> report = std::move(onAnswer_);
    onAnswer_ = nullptr;
    if (report) report(index);
  }

  static constexpr float kWidth = 440.0f;
  Widget holder_;
  AlertOverlay alert_;
  std::function<void(int)> onAnswer_;
  int buttons_ = 1;
  bool answered_ = false;
};

/**
 * Show the drawn box for `session`. Message thread.
 *
 * The box's answer holds the session STRONGLY: nobody else may (showAsync
 * keeps no handle), and the box is what has to keep it alive until it is
 * answered. The cycle -- session, box, answer, session -- is broken when the
 * box is taken down: after the answer, or by dismiss().
 */
inline bool showDrawn(const MessageBoxOptions& options, const std::shared_ptr<Session>& session) {
  std::shared_ptr<Session> keep = session;
  std::unique_ptr<DrawnBox> box(new DrawnBox(options, [keep](int index) {
    // The window goes through the queue: this runs inside its own click.
    std::weak_ptr<Session> weak = keep;
    MessageManager::callAsync([weak]() {
      if (std::shared_ptr<Session> live = weak.lock()) live->drawn.reset();
    });
    deliver(keep, index);
  }));
  box->setVisible(true);
  if (!box->isWindowVisible()) return false;
  // The keyboard to the question, so Return and Escape answer it.
  if (MouseRouter* router = routerFor(&box->alert())) router->setFocus(&box->alert());
  session->drawn = std::move(box);
  return true;
}

/** Run the drawn box to its answer, pumping the message loop: the modal form
 *  where the platform offered none. */
inline int runDrawnModally(const MessageBoxOptions& options) {
  MessageManager* mm = MessageManager::getInstance();
  auto session = std::make_shared<Session>();
  auto answer = std::make_shared<int>(-2);
  session->callback = [answer](int index) { *answer = index; };
  if (!showDrawn(options, session)) return -1;
  while (*answer == -2) {
    if (!mm->runDispatchLoopUntil(10)) break;
  }
  session->drawn.reset();
  return *answer == -2 ? -1 : *answer;
}

#if defined(_WIN32)

inline std::wstring widen(const std::string& utf8) {
  if (utf8.empty()) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int) utf8.size(), nullptr, 0);
  std::wstring out((size_t) (n > 0 ? n : 0), L'\0');
  if (n > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int) utf8.size(), &out[0], n);
  return out;
}

/** The owner window for a box: the frame of the associated component's
 *  window, or none. */
inline HWND ownerFor(const MessageBoxOptions& options) {
  if (TopLevelWindow* w = TopLevelWindow::findTopLevelWindowFor(options.getAssociatedComponent()))
    return w->isOnDesktop() ? (HWND) w->getWindowHandle() : nullptr;
  return nullptr;
}

/** The activation context that brings in Common Controls 6, made once from
 *  shell32's own manifest; null where it cannot be made. */
inline HANDLE commonControlsContext() {
  static HANDLE context = [] {
    wchar_t system[MAX_PATH];
    const UINT n = GetSystemDirectoryW(system, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 16) return INVALID_HANDLE_VALUE;
    std::wstring shell32 = std::wstring(system) + L"\\shell32.dll";
    ACTCTXW ctx{};
    ctx.cbSize = sizeof(ctx);
    ctx.dwFlags = ACTCTX_FLAG_RESOURCE_NAME_VALID;
    ctx.lpSource = shell32.c_str();
    ctx.lpResourceName = MAKEINTRESOURCEW(124);
    return CreateActCtxW(&ctx);
  }();
  return context == INVALID_HANDLE_VALUE ? nullptr : context;
}

/** Common Controls 6 active on this thread for as long as it lives. */
class ScopedCommonControls {
public:
  ScopedCommonControls() {
    if (HANDLE ctx = commonControlsContext())
      if (!ActivateActCtx(ctx, &cookie_)) cookie_ = 0;
  }
  ~ScopedCommonControls() {
    if (cookie_) DeactivateActCtx(0, cookie_);
  }
  ScopedCommonControls(const ScopedCommonControls&) = delete;
  ScopedCommonControls& operator=(const ScopedCommonControls&) = delete;

private:
  ULONG_PTR cookie_ = 0;
};

using TaskDialogIndirectFn = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);

/** TaskDialogIndirect, or null on a system or process where Common Controls
 *  6 cannot be had. Loaded inside the activation context, which is what
 *  redirects the load to version 6. */
inline TaskDialogIndirectFn taskDialogIndirect() {
  static const TaskDialogIndirectFn fn = []() -> TaskDialogIndirectFn {
    if (HMODULE loaded = GetModuleHandleW(L"comctl32.dll"))
      if (FARPROC f = GetProcAddress(loaded, "TaskDialogIndirect"))
        return (TaskDialogIndirectFn) (void*) f;
    ScopedCommonControls v6;
    HMODULE module = LoadLibraryExW(L"comctl32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) return nullptr;
    return (TaskDialogIndirectFn) (void*) GetProcAddress(module, "TaskDialogIndirect");
  }();
  return fn;
}

/** Take a box down from any thread: it is posted, never sent, so a closing
 *  thread never waits on the box's. */
inline void postDismiss(HWND dialog, bool taskDialog) {
  if (!dialog) return;
  if (taskDialog) {
    PostMessageW(dialog, WM_CLOSE, 0, 0); // with cancellation allowed: IDCANCEL
    return;
  }
  // A MessageBox closes on a click of any of its buttons; WM_CLOSE only
  // works on the ones that have a Cancel.
  const int ids[] = {IDCANCEL, IDNO, IDOK, IDYES};
  for (int id : ids)
    if (HWND button = GetDlgItem(dialog, id)) {
      PostMessageW(dialog, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), (LPARAM) button);
      return;
    }
  PostMessageW(dialog, WM_CLOSE, 0, 0);
}

/** The box's window, recorded on its own thread the moment it exists; a box
 *  already closed from code is taken straight back down. */
inline void recordDialog(Session* session, HWND hwnd, bool taskDialog) {
  if (!session) return;
  bool dismiss = false;
  {
    std::lock_guard<std::mutex> lock(session->mutex);
    session->dialog = hwnd;
    session->taskDialog = taskDialog;
    dismiss = session->dismissed;
  }
  if (dismiss) postDismiss(hwnd, taskDialog);
}

inline HRESULT CALLBACK taskDialogCallback(HWND hwnd, UINT notification, WPARAM, LPARAM,
                                           LONG_PTR data) {
  if (notification == TDN_CREATED) recordDialog(reinterpret_cast<Session*>(data), hwnd, true);
  return S_OK;
}

/** The first button's id; TaskDialog's own ids (IDOK and friends) are below. */
constexpr int kFirstButtonId = 1000;

/** Returns the index pressed, -1 if the dialog could not be shown, or -2 if
 *  TaskDialog itself is unavailable (try the next backend). */
inline int runTaskDialog(const MessageBoxOptions& options, HWND owner, Session* session) {
  const TaskDialogIndirectFn indirect = taskDialogIndirect();
  if (!indirect) return -2;
  const int count = options.getNumButtons();
  std::vector<std::wstring> labels;
  for (int i = 0; i < count; ++i) labels.push_back(widen(options.getButtonText(i)));
  if (labels.empty()) labels.push_back(L"OK");
  std::vector<TASKDIALOG_BUTTON> buttons(labels.size());
  for (size_t i = 0; i < labels.size(); ++i) {
    buttons[i].nButtonID = kFirstButtonId + (int) i;
    buttons[i].pszButtonText = labels[i].c_str();
  }
  const std::wstring title = widen(options.getTitle());
  const std::wstring message = widen(options.getMessage());
  TASKDIALOGCONFIG config{};
  config.cbSize = sizeof(config);
  config.hwndParent = owner;
  // Escape and the close box answer IDCANCEL: the last button, by the
  // convention in the header.
  config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT |
                   (owner ? TDF_POSITION_RELATIVE_TO_WINDOW : 0);
  config.pszWindowTitle = title.c_str();
  config.pszContent = message.c_str();
  config.pButtons = buttons.data();
  config.cButtons = (UINT) buttons.size();
  config.nDefaultButton = kFirstButtonId;
  switch (options.getIconType()) {
    case MessageBoxIconType::WarningIcon: config.pszMainIcon = TD_WARNING_ICON; break;
    case MessageBoxIconType::InfoIcon: config.pszMainIcon = TD_INFORMATION_ICON; break;
    case MessageBoxIconType::QuestionIcon:
      // TaskDialog has no question icon of its own; the system's is used.
      config.dwFlags |= TDF_USE_HICON_MAIN;
      config.hMainIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32514)); // IDI_QUESTION
      break;
    case MessageBoxIconType::NoIcon: break;
  }
  config.pfCallback = &taskDialogCallback;
  config.lpCallbackData = (LONG_PTR) session;
  ScopedCommonControls v6;
  int pressed = 0;
  if (FAILED(indirect(&config, &pressed, nullptr, nullptr))) return -1;
  if (pressed >= kFirstButtonId && pressed < kFirstButtonId + (int) labels.size())
    return pressed - kFirstButtonId;
  return (int) labels.size() - 1; // IDCANCEL: Escape or the close box
}

/** The CBT hook that hands a MessageBox's window over: thread-local, as the
 *  hook is. */
inline Session*& classicSession() {
  static thread_local Session* session = nullptr;
  return session;
}
inline HHOOK& classicHook() {
  static thread_local HHOOK hook = nullptr;
  return hook;
}

inline LRESULT CALLBACK classicCbt(int code, WPARAM wp, LPARAM lp) {
  if (code == HCBT_ACTIVATE) {
    wchar_t cls[16] = {0};
    GetClassNameW((HWND) wp, cls, 16);
    if (std::wcscmp(cls, L"#32770") == 0 && classicSession()) {
      recordDialog(classicSession(), (HWND) wp, false);
      classicSession() = nullptr; // the first dialog is ours; leave any later one
    }
  }
  return CallNextHookEx(classicHook(), code, wp, lp);
}

/** The classic box: -1 when it cannot take this many buttons. */
inline int runClassic(const MessageBoxOptions& options, HWND owner, Session* session) {
  const int count = (std::max)(1, options.getNumButtons());
  if (count > 3) return -1;
  auto lower = [](std::string s) {
    for (char& c : s)
      if (c >= 'A' && c <= 'Z') c = (char) (c - 'A' + 'a');
    return s;
  };
  UINT type = MB_OK;
  if (count == 2)
    type = lower(options.getButtonText(0)) == "yes" || lower(options.getButtonText(1)) == "no"
               ? MB_YESNO
               : MB_OKCANCEL;
  if (count == 3) type = MB_YESNOCANCEL;
  switch (options.getIconType()) {
    case MessageBoxIconType::WarningIcon: type |= MB_ICONWARNING; break;
    case MessageBoxIconType::InfoIcon: type |= MB_ICONINFORMATION; break;
    case MessageBoxIconType::QuestionIcon: type |= MB_ICONQUESTION; break;
    case MessageBoxIconType::NoIcon: break;
  }
  if (!owner) type |= MB_TASKMODAL | MB_SETFOREGROUND;
  classicSession() = session;
  classicHook() = SetWindowsHookExW(WH_CBT, &classicCbt, nullptr, GetCurrentThreadId());
  const int answer = MessageBoxW(owner, widen(options.getMessage()).c_str(),
                                 widen(options.getTitle()).c_str(), type);
  if (classicHook()) UnhookWindowsHookEx(classicHook());
  classicHook() = nullptr;
  classicSession() = nullptr;
  switch (answer) {
    case IDOK:
    case IDYES: return 0;
    case IDNO: return 1;
    case IDCANCEL: return count - 1;
    default: return -1;
  }
}

/** The native box, on the calling thread: TaskDialog, then MessageBoxW.
 *  -2 means neither can show it (the drawn box should). */
inline int runNative(const MessageBoxOptions& options, HWND owner, Session* session) {
  const Backend backend = forcedBackend().load();
  if (backend == Backend::drawn) return -2;
  if (backend != Backend::classic) {
    const int result = runTaskDialog(options, owner, session);
    if (result != -2 || backend == Backend::taskDialog) return result;
  }
  const int result = runClassic(options, owner, session);
  return result == -1 && options.getNumButtons() > 3 ? -2 : result;
}

#elif defined(__APPLE__)

template <typename Ret, typename... Args>
Ret objc(id target, const char* selector, Args... args) {
  using Fn = Ret (*)(id, SEL, Args...);
  return reinterpret_cast<Fn>(objc_msgSend)(target, sel_registerName(selector), args...);
}

/** NSAlert, run modally on the main thread. -1 if AppKit is not there. */
inline int runAlert(const MessageBoxOptions& options, Session* session) {
  Class alertClass = objc_getClass("NSAlert");
  Class stringClass = objc_getClass("NSString");
  if (!alertClass || !stringClass) return -1;
  id pool = objc<id>(objc<id>((id) objc_getClass("NSAutoreleasePool"), "alloc"), "init");
  auto ns = [stringClass](const std::string& s) {
    return objc<id>((id) stringClass, "stringWithUTF8String:", s.c_str());
  };
  id alert = objc<id>(objc<id>((id) alertClass, "alloc"), "init");
  int result = -1;
  if (alert) {
    // NSAlertStyleWarning 0, Informational 1, Critical 2.
    objc<void>(alert, "setAlertStyle:",
               (unsigned long) (options.getIconType() == MessageBoxIconType::WarningIcon ? 0 : 1));
    objc<void>(alert, "setMessageText:", ns(options.getTitle()));
    objc<void>(alert, "setInformativeText:", ns(options.getMessage()));
    const int count = (std::max)(1, options.getNumButtons());
    for (int i = 0; i < count; ++i)
      objc<id>(alert, "addButtonWithTitle:",
               ns(options.getNumButtons() ? options.getButtonText(i) : std::string("OK")));
    if (session) {
      std::lock_guard<std::mutex> lock(session->mutex);
      session->running = true;
    }
    // NSAlertFirstButtonReturn is 1000 and the rest follow; abortModal gives
    // NSModalResponseAbort (-1001).
    const long response = objc<long>(alert, "runModal");
    if (session) {
      std::lock_guard<std::mutex> lock(session->mutex);
      session->running = false;
    }
    const long index = response - 1000;
    result = index >= 0 && index < count ? (int) index : count - 1;
    objc<void>(alert, "release");
  }
  if (pool) objc<void>(pool, "drain");
  return result;
}

#else

/** A program on the PATH, or empty. */
inline std::string findOnPath(const char* program) {
  const char* path = std::getenv("PATH");
  if (!path) return std::string();
  std::string all(path);
  size_t start = 0;
  while (start <= all.size()) {
    size_t end = all.find(':', start);
    if (end == std::string::npos) end = all.size();
    std::string dir = all.substr(start, end - start);
    if (!dir.empty()) {
      const std::string candidate = dir + "/" + program;
      if (access(candidate.c_str(), X_OK) == 0) return candidate;
    }
    start = end + 1;
  }
  return std::string();
}

/** Run a dialog program, recording its pid so close() can end it; returns
 *  the exit status (or -1) and what it printed. */
inline int runProcess(const std::vector<std::string>& args, Session* session, std::string* out) {
  std::vector<char*> argv;
  for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  int pipeFds[2] = {-1, -1};
  if (pipe(pipeFds) != 0) return -1;
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, pipeFds[1], 1);
  posix_spawn_file_actions_addclose(&actions, pipeFds[0]);
  pid_t pid = -1;
  const int spawned = posix_spawn(&pid, args[0].c_str(), &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(pipeFds[1]);
  if (spawned != 0) {
    close(pipeFds[0]);
    return -1;
  }
  bool dismiss = false;
  if (session) {
    std::lock_guard<std::mutex> lock(session->mutex);
    session->child = pid;
    dismiss = session->dismissed;
  }
  if (dismiss) kill(pid, SIGTERM);
  char buffer[512];
  for (;;) {
    const ssize_t n = read(pipeFds[0], buffer, sizeof(buffer));
    if (n <= 0) break;
    if (out && out->size() < 4096) out->append(buffer, (size_t) n);
  }
  close(pipeFds[0]);
  int status = 0;
  bool reaped = true;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno == EINTR) continue;
    reaped = false; // reaped elsewhere (SIGCHLD ignored): no answer to read
    break;
  }
  if (session) {
    // Before anything could reuse the pid: close() must never signal it.
    std::lock_guard<std::mutex> lock(session->mutex);
    session->child = -1;
  }
  return reaped && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/** zenity or kdialog: the index pressed, -1 if it could not be shown, -2 if
 *  neither program can show this box (the drawn one should). */
inline int runDesktopDialog(const MessageBoxOptions& options, Session* session) {
  if (forcedBackend().load() == Backend::drawn) return -2;
  const int count = (std::max)(1, options.getNumButtons());
  auto label = [&options](int i) {
    return options.getNumButtons() ? options.getButtonText(i) : std::string("OK");
  };
  const std::string zenity = findOnPath("zenity");
  if (!zenity.empty()) {
    std::vector<std::string> args{zenity};
    const MessageBoxIconType icon = options.getIconType();
    if (count == 1)
      args.push_back(icon == MessageBoxIconType::WarningIcon ? "--warning" : "--info");
    else
      args.push_back("--question");
    args.push_back("--no-markup");
    args.push_back("--title=" + options.getTitle());
    args.push_back("--text=" + options.getMessage());
    args.push_back("--ok-label=" + label(0));
    if (count > 1) args.push_back("--cancel-label=" + label(count - 1));
    // The middle buttons are "extra" buttons, which print their label.
    for (int i = 1; i < count - 1; ++i) args.push_back("--extra-button=" + label(i));
    std::string printed;
    const int status = runProcess(args, session, &printed);
    if (status < 0) return -1;
    if (status == 0) return 0;
    while (!printed.empty() && (printed.back() == '\n' || printed.back() == '\r')) printed.pop_back();
    for (int i = 1; i < count - 1; ++i)
      if (printed == label(i)) return i;
    return count - 1;
  }
  const std::string kdialog = findOnPath("kdialog");
  if (!kdialog.empty() && count <= 3) {
    std::vector<std::string> args{kdialog, "--title", options.getTitle()};
    if (count == 1) {
      args.push_back(options.getIconType() == MessageBoxIconType::WarningIcon ? "--sorry" : "--msgbox");
      args.push_back(options.getMessage());
    } else {
      args.push_back(count == 2 ? "--yesno" : "--yesnocancel");
      args.push_back(options.getMessage());
      args.push_back("--yes-label");
      args.push_back(label(0));
      args.push_back("--no-label");
      args.push_back(label(1));
      if (count == 3) {
        args.push_back("--cancel-label");
        args.push_back(label(2));
      }
    }
    const int status = runProcess(args, session, nullptr);
    if (status < 0) return -1;
    return count == 1 ? 0 : (status >= 0 && status < count ? status : count - 1);
  }
  return -2;
}

#endif

/** Take a box down from code (message thread). */
inline void dismiss(const std::shared_ptr<Session>& session) {
  if (!session) return;
#if defined(_WIN32)
  HWND dialog = nullptr;
  bool taskDialog = false;
#elif !defined(__APPLE__)
  pid_t child = -1;
#endif
  bool running = false;
  {
    std::lock_guard<std::mutex> lock(session->mutex);
    if (session->dismissed) return;
    session->dismissed = true;
    session->callback = nullptr;
#if defined(_WIN32)
    dialog = session->dialog;
    taskDialog = session->taskDialog;
#elif defined(__APPLE__)
    running = session->running;
#else
    child = session->child;
#endif
  }
  (void) running;
  session->drawn.reset();
#if defined(_WIN32)
  postDismiss(dialog, taskDialog);
#elif defined(__APPLE__)
  // The alert is inside runModal on this thread, which is running this
  // (a queued call reaches it in the common modes).
  if (running)
    if (id app = objc<id>((id) objc_getClass("NSApplication"), "sharedApplication"))
      objc<void>(app, "abortModal");
#else
  if (child > 0) kill(child, SIGTERM);
#endif
}

} // namespace msgbox
} // namespace detail

/**
 * A box opened by showScopedAsync. Destroying it, or close(), takes the box
 * down and its callback is not called. Movable, not copyable; an empty one
 * does nothing.
 */
class ScopedMessageBox {
public:
  /** Empty: holds no box. */
  ScopedMessageBox() = default;
  /** What showScopedAsync makes; a caller has no session to pass. */
  explicit ScopedMessageBox(std::shared_ptr<detail::msgbox::Session> session)
      : session_(std::move(session)) {}
  /** Closes the box if it is still up, without calling its callback. */
  ~ScopedMessageBox() { close(); }

  /** Takes the box over; `other` is left empty. */
  ScopedMessageBox(ScopedMessageBox&& other) noexcept : session_(std::move(other.session_)) {}
  /** Closes this handle's box first, then takes over `other`'s. */
  ScopedMessageBox& operator=(ScopedMessageBox&& other) noexcept {
    if (this != &other) {
      close();
      session_ = std::move(other.session_);
    }
    return *this;
  }
  /** Not copyable: one handle closes one box. */
  ScopedMessageBox(const ScopedMessageBox&) = delete;
  ScopedMessageBox& operator=(const ScopedMessageBox&) = delete;

  /** Take the box down now, without an answer. Message thread. */
  void close() {
    std::shared_ptr<detail::msgbox::Session> session = std::move(session_);
    session_.reset();
    detail::msgbox::dismiss(session);
  }

  /** Still showing: not answered, not closed. */
  bool isOpen() const {
    if (!session_) return false;
    std::lock_guard<std::mutex> lock(session_->mutex);
    return !session_->finished && !session_->dismissed;
  }

private:
  std::shared_ptr<detail::msgbox::Session> session_;
};

/** The OS's message boxes. See the header for what runs where. */
class NativeMessageBox {
public:
  /**
   * Show the box and wait for the answer: the index of the button pressed
   * (the last one for the close box or Escape), or -1 if no box could be
   * shown. Message thread.
   */
  static int show(const MessageBoxOptions& options) {
#if defined(_WIN32)
    const int result = detail::msgbox::runNative(options, detail::msgbox::ownerFor(options), nullptr);
    return result == -2 ? detail::msgbox::runDrawnModally(options) : result;
#elif defined(__APPLE__)
    const int result = detail::msgbox::runAlert(options, nullptr);
    return result == -1 ? detail::msgbox::runDrawnModally(options) : result;
#else
    const int result = detail::msgbox::runDesktopDialog(options, nullptr);
    return result == -2 ? detail::msgbox::runDrawnModally(options) : result;
#endif
  }

  /** Show the box and return at once; `callback` gets the answer on the
   *  message thread. */
  static void showAsync(const MessageBoxOptions& options, std::function<void(int)> callback) {
    start(options, std::move(callback));
  }

  /** As showAsync, with a handle that closes the box (no callback) when it
   *  is destroyed. */
  static ScopedMessageBox showScopedAsync(const MessageBoxOptions& options,
                                          std::function<void(int)> callback) {
    return ScopedMessageBox(start(options, std::move(callback)));
  }

  // ── The reference's convenience forms ────────────────────────────────────

  /** One OK button, waited for. */
  static void showMessageBox(MessageBoxIconType icon, const std::string& title,
                             const std::string& message, Component* associated = nullptr) {
    show(MessageBoxOptions::makeOptionsOk(icon, title, message, "OK", associated));
  }

  /** One OK button, not waited for; `onDismissed` (if any) when it goes. */
  static void showMessageBoxAsync(MessageBoxIconType icon, const std::string& title,
                                  const std::string& message, Component* associated = nullptr,
                                  std::function<void()> onDismissed = nullptr) {
    showAsync(MessageBoxOptions::makeOptionsOk(icon, title, message, "OK", associated),
              [onDismissed](int) {
                if (onDismissed) onDismissed();
              });
  }

  /** OK/Cancel. Without a callback it waits and returns true for OK; with
   *  one it returns false at once and the callback gets 1 (OK) or 0. */
  static bool showOkCancelBox(MessageBoxIconType icon, const std::string& title,
                              const std::string& message, Component* associated = nullptr,
                              std::function<void(int)> callback = nullptr) {
    const MessageBoxOptions o =
        MessageBoxOptions::makeOptionsOkCancel(icon, title, message, "OK", "Cancel", associated);
    if (!callback) return show(o) == 0;
    showAsync(o, [callback](int index) { callback(index == 0 ? 1 : 0); });
    return false;
  }

  /** Yes/No/Cancel: 1 yes, 2 no, 0 cancel -- returned when waited for (no
   *  callback), passed to the callback otherwise (and 0 returned). */
  static int showYesNoCancelBox(MessageBoxIconType icon, const std::string& title,
                                const std::string& message, Component* associated = nullptr,
                                std::function<void(int)> callback = nullptr) {
    const MessageBoxOptions o = MessageBoxOptions::makeOptionsYesNoCancel(
        icon, title, message, "Yes", "No", "Cancel", associated);
    auto map = [](int index) { return index == 0 ? 1 : (index == 1 ? 2 : 0); };
    if (!callback) return map(show(o));
    showAsync(o, [callback, map](int index) { callback(map(index)); });
    return 0;
  }

  /** Yes/No: 1 yes, 0 no -- as above. */
  static int showYesNoBox(MessageBoxIconType icon, const std::string& title,
                          const std::string& message, Component* associated = nullptr,
                          std::function<void(int)> callback = nullptr) {
    const MessageBoxOptions o =
        MessageBoxOptions::makeOptionsYesNo(icon, title, message, "Yes", "No", associated);
    if (!callback) return show(o) == 0 ? 1 : 0;
    showAsync(o, [callback](int index) { callback(index == 0 ? 1 : 0); });
    return 0;
  }

private:
  static std::shared_ptr<detail::msgbox::Session> start(const MessageBoxOptions& options,
                                                        std::function<void(int)> callback) {
    auto session = std::make_shared<detail::msgbox::Session>();
    session->callback = std::move(callback);
#if defined(_WIN32)
    if (detail::msgbox::forcedBackend().load() != detail::msgbox::Backend::drawn) {
      HWND owner = detail::msgbox::ownerFor(options);
      // A thread of its own: the box's modal loop then holds nothing of the
      // caller's. The thread owns a reference to the session, so a closed
      // handle cannot pull it away from under the box.
      std::thread([options, owner, session]() {
        const int result = detail::msgbox::runNative(options, owner, session.get());
        if (result == -2) {
          // Neither native box can show it: the drawn one, on the message
          // thread where windows of this SDK live.
          if (!MessageManager::callAsync([options, session]() {
                {
                  std::lock_guard<std::mutex> lock(session->mutex);
                  if (session->dismissed) return;
                }
                if (!detail::msgbox::showDrawn(options, session))
                  detail::msgbox::deliver(session, -1);
              }))
            detail::msgbox::deliver(session, -1);
          return;
        }
        detail::msgbox::deliver(session, result);
      }).detach();
      return session;
    }
    if (!detail::msgbox::showDrawn(options, session)) detail::msgbox::deliver(session, -1);
#elif defined(__APPLE__)
    // NSAlert must run on the main thread: queued, so this returns at once,
    // and run modally from the queue (which keeps draining under it).
    const bool queued = MessageManager::callAsync([options, session]() {
      {
        std::lock_guard<std::mutex> lock(session->mutex);
        if (session->dismissed) return;
      }
      const int result = detail::msgbox::runAlert(options, session.get());
      if (result >= 0) {
        detail::msgbox::deliver(session, result);
        return;
      }
      if (!detail::msgbox::showDrawn(options, session)) detail::msgbox::deliver(session, -1);
    });
    if (!queued) detail::msgbox::deliver(session, -1);
#else
    if (detail::msgbox::forcedBackend().load() != detail::msgbox::Backend::drawn &&
        (!detail::msgbox::findOnPath("zenity").empty() ||
         (!detail::msgbox::findOnPath("kdialog").empty() && options.getNumButtons() <= 3))) {
      std::thread([options, session]() {
        detail::msgbox::deliver(session, detail::msgbox::runDesktopDialog(options, session.get()));
      }).detach();
      return session;
    }
    if (!detail::msgbox::showDrawn(options, session)) detail::msgbox::deliver(session, -1);
#endif
    return session;
  }
};

} // namespace gfx
} // namespace sonore
