// SPDX-License-Identifier: Apache-2.0
//
// An icon in the system tray (the notification area, the macOS menu bar's
// status items), and the desktop's own local notifications.
//
// ── SystemTrayIconComponent ────────────────────────────────────────────────
//
// As in the reference it is a Component, and a click on the icon arrives as
// one: a left click as mouseDown (and onClick), a right click as
// contextMenu (and onRightClick), a double click with clickCount 2. Nothing
// is drawn by the tree; the image is handed to the OS.
//
//   Windows  Shell_NotifyIconW, version 4, on a hidden top-level window of
//            its own -- top-level because the "TaskbarCreated" broadcast that
//            says Explorer restarted (and every icon must be added again)
//            reaches no message-only window. The icon is made from a Bitmap
//            (straight alpha, as icons are). showInfoBubble is the balloon,
//            which Windows 10 and 11 show as a toast.
//   macOS    NSStatusItem in the system status bar, its button's image made
//            from the Bitmap (a template image when one is given), its action
//            telling a left click from a right one by the current event.
//            showInfoBubble goes through PushNotifications below.
//   Linux    UNAVAILABLE. The tray there is StatusNotifierItem, a D-Bus
//            protocol: an object registered on the session bus answering
//            property and method calls, which needs a D-Bus connection and
//            loop this SDK does not carry. isAvailable() says so.
//
// ── PushNotifications ──────────────────────────────────────────────────────
//
// The LOCAL half of the reference's class: post a notification from this
// program and hear when the user clicks it. Windows: the tray balloon (a
// toast) -- a full WinRT toast needs an AppUserModelID registered with a
// Start-menu shortcut, which is an installer's business, not a library's.
// macOS: UNUserNotificationCenter (which only works in a signed .app bundle).
// Linux: notify-send when it is on the PATH (no click is reported back).
// REMOTE push -- a server sending to a device -- needs APNs or FCM
// credentials and a server, and is not here: getDeviceToken() is empty.
//
// RUN on Windows: the icon added, modified, given a balloon and removed,
// with every Shell_NotifyIcon result counted; clicks and the balloon's click
// delivered by the callback messages the shell sends; Explorer's restart.
// macOS compiled only; Linux reports itself unavailable.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../message_manager.h"
#include "bitmap.h"
#include "component.h"
#include "document_window.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <cwchar>
#if defined(_MSC_VER)
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#endif
#elif defined(__APPLE__)
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>
#else
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace sonore {
namespace gfx {

namespace detail {
namespace tray {

/** Every Shell_NotifyIcon call this process made, and how it went: what a
 *  test counts, since the tray itself cannot be looked at. */
struct Stats {
  std::atomic<int> adds{0}, modifies{0}, deletes{0}, failures{0};
};
inline Stats& stats() {
  static Stats s;
  return s;
}

#if defined(__APPLE__)
template <typename Ret, typename... Args>
Ret objc(id target, const char* selector, Args... args) {
  using Fn = Ret (*)(id, SEL, Args...);
  return reinterpret_cast<Fn>(objc_msgSend)(target, sel_registerName(selector), args...);
}

inline id nsString(const std::string& s) {
  return objc<id>((id) objc_getClass("NSString"), "stringWithUTF8String:", s.c_str());
}

/** An NSImage (autoreleased) from a Bitmap: an NSBitmapImageRep the pixels
 *  are copied into -- premultiplied RGBA, which is what the rep is told. */
inline id imageFromBitmap(const Bitmap& bitmap, double pointSize) {
  if (bitmap.isEmpty()) return nullptr;
  Class repClass = objc_getClass("NSBitmapImageRep");
  Class imageClass = objc_getClass("NSImage");
  if (!repClass || !imageClass) return nullptr;
  using InitRep = id (*)(id, SEL, unsigned char**, long, long, long, long, BOOL, BOOL, id, long, long);
  id rep = reinterpret_cast<InitRep>(objc_msgSend)(
      objc<id>((id) repClass, "alloc"),
      sel_registerName("initWithBitmapDataPlanes:pixelsWide:pixelsHigh:bitsPerSample:"
                       "samplesPerPixel:hasAlpha:isPlanar:colorSpaceName:bytesPerRow:bitsPerPixel:"),
      (unsigned char**) nullptr, (long) bitmap.width(), (long) bitmap.height(), 8L, 4L, (BOOL) YES,
      (BOOL) NO, nsString("NSDeviceRGBColorSpace"), (long) bitmap.width() * 4, 32L);
  if (!rep) return nullptr;
  if (unsigned char* bytes = objc<unsigned char*>(rep, "bitmapData"))
    std::memcpy(bytes, bitmap.data(), bitmap.sizeInBytes());
  struct Size2 {
    double width, height;
  };
  using InitImage = id (*)(id, SEL, Size2);
  id image = reinterpret_cast<InitImage>(objc_msgSend)(objc<id>((id) imageClass, "alloc"),
                                                       sel_registerName("initWithSize:"),
                                                       Size2{pointSize, pointSize});
  if (image) {
    objc<void>(image, "addRepresentation:", rep);
    objc<id>(image, "autorelease");
  }
  objc<void>(rep, "release");
  return image;
}
#endif

} // namespace tray
} // namespace detail

/**
 * An icon in the system tray. See the header for each platform; on Linux it
 * is unavailable and every call does nothing.
 */
class SystemTrayIconComponent : public Component {
public:
  /** Not in the tray yet: the first setIconImage() puts it there. */
  SystemTrayIconComponent() = default;
  /** Takes the icon out of the tray, as removeFromTray() does. */
  ~SystemTrayIconComponent() override { removeFromTray(); }

  /** Not copyable: the object is the one entry in the tray, and the window
   *  (Windows) or status item (macOS) behind it. */
  SystemTrayIconComponent(const SystemTrayIconComponent&) = delete;
  /** Not assignable, for the same reason. */
  SystemTrayIconComponent& operator=(const SystemTrayIconComponent&) = delete;

  /** Whether this platform has a tray this class can use. */
  static bool isAvailable() {
#if defined(_WIN32) || defined(__APPLE__)
    return true;
#else
    return false;
#endif
  }

  /**
   * The icon. Setting the first image puts it in the tray. `templateImage`
   * is macOS's monochrome form (the system tints it); elsewhere it is used
   * only when the colour image is empty.
   */
  void setIconImage(const Bitmap& colourImage, const Bitmap& templateImage) {
    colour_ = colourImage;
    template_ = templateImage;
    const Bitmap& use = colour_.isEmpty() ? template_ : colour_;
    if (use.isEmpty()) return;
#if defined(_WIN32)
    HICON icon = detail::tlw::iconFromBitmap(use, GetSystemMetrics(SM_CXSMICON));
    if (!icon) return;
    if (icon_) DestroyIcon(icon_);
    icon_ = icon;
    if (!added_) addToTray();
    else modify(NIF_ICON);
#elif defined(__APPLE__)
    if (!item_) createStatusItem();
    applyImage();
#endif
  }

  /** The text shown while the pointer rests on the icon (UTF-8). Kept when
   *  the icon is not in the tray yet and used once it is. Windows cuts it at
   *  127 UTF-16 units, the shell's limit. Linux: nothing. */
  void setIconTooltip(const std::string& tooltip) {
    tooltip_ = tooltip;
#if defined(_WIN32)
    if (added_) modify(NIF_TIP | NIF_SHOWTIP);
#elif defined(__APPLE__)
    if (id button = statusButton())
      detail::tray::objc<void>(button, "setToolTip:", detail::tray::nsString(tooltip));
#endif
  }

  /** macOS: the button drawn pressed (while a menu is up). Elsewhere nothing. */
  void setHighlighted(bool shouldHighlight) {
#if defined(__APPLE__)
    if (id button = statusButton())
      detail::tray::objc<void>(button, "highlight:", (BOOL) (shouldHighlight ? YES : NO));
#else
    (void) shouldHighlight;
#endif
  }

  /** A notification from the icon: Windows' balloon (a toast on 10 and 11);
   *  on macOS a local notification. */
  void showInfoBubble(const std::string& title, const std::string& content);

  /** Windows: take the balloon showInfoBubble() put up away. Elsewhere it
   *  does nothing; a macOS notification is taken away by
   *  PushNotifications::removeAllDeliveredNotifications(). */
  void hideInfoBubble() {
#if defined(_WIN32)
    infoTitle_.clear();
    infoText_.clear();
    if (added_) modify(NIF_INFO);
#endif
  }

  /** Windows: the hidden window the shell talks to. macOS: the
   *  NSStatusItem. Linux: null. */
  void* getNativeHandle() const {
#if defined(_WIN32)
    return hwnd_;
#elif defined(__APPLE__)
    return (void*) item_;
#else
    return nullptr;
#endif
  }

  /** In the tray now. */
  bool isInTray() const {
#if defined(_WIN32)
    return added_;
#elif defined(__APPLE__)
    return item_ != nullptr;
#else
    return false;
#endif
  }

  /** Take the icon out of the tray (the destructor does). */
  void removeFromTray() {
#if defined(_WIN32)
    if (added_) {
      NOTIFYICONDATAW data = baseData();
      record(Shell_NotifyIconW(NIM_DELETE, &data), detail::tray::stats().deletes);
      added_ = false;
    }
    if (hwnd_) {
      SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
      DestroyWindow(hwnd_);
      hwnd_ = nullptr;
    }
    if (icon_) DestroyIcon(icon_);
    icon_ = nullptr;
#elif defined(__APPLE__)
    if (item_) {
      if (id bar = detail::tray::objc<id>((id) objc_getClass("NSStatusBar"), "systemStatusBar"))
        detail::tray::objc<void>(bar, "removeStatusItem:", item_);
      detail::tray::objc<void>(item_, "release");
      item_ = nullptr;
    }
    if (target_) {
      object_setInstanceVariable(target_, "sonoreTray", nullptr);
      detail::tray::objc<void>(target_, "release");
      target_ = nullptr;
    }
#endif
  }

  /** A left click, called after mouseDown(). A click the OS reports as a
   *  double click calls onDoubleClick instead. */
  std::function<void()> onClick;
  /** A left double click (clickCount 2), called after mouseDown(). */
  std::function<void()> onDoubleClick;
  /** A right click (or a control-click on macOS), called after
   *  contextMenu(). */
  std::function<void()> onRightClick;
  /** The user clicked the balloon or notification. */
  std::function<void()> onBalloonClicked;

#if defined(_WIN32)
  /** The message the shell sends clicks with, for a test that plays the
   *  shell's part. */
  static constexpr UINT kCallbackMessage = WM_APP + 0x7A1;
  /** The icon's uID in Shell_NotifyIcon, the HIWORD of a callback's lParam:
   *  for the same test. */
  static constexpr UINT kIconId = 1;
#endif

  /** Deliver an event as the OS does: for the platform glue and for tests. */
  void handleTrayClick(bool rightButton, int clickCount) {
    std::weak_ptr<char> token = alive_;
    MouseEvent e;
    e.clickCount = clickCount;
    e.rightButton = rightButton;
    if (rightButton) {
      contextMenu(e);
      if (token.expired()) return;
      if (onRightClick) onRightClick();
      return;
    }
    mouseDown(e);
    if (token.expired()) return;
    if (clickCount >= 2) {
      if (onDoubleClick) onDoubleClick();
    } else if (onClick) {
      onClick();
    }
  }

  /** Deliver a click on the balloon or notification as the OS does: calls
   *  onBalloonClicked. For the platform glue and for tests. */
  void handleBalloonClick() {
    if (onBalloonClicked) onBalloonClicked();
  }

private:
  friend class PushNotifications; // reads alive_: see setTrayIcon

#if defined(_WIN32)
  static HMODULE module() {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR) &SystemTrayIconComponent::module, &m);
    return m;
  }

  static const wchar_t* windowClass() {
    static std::wstring name;
    static ATOM atom = 0;
    if (atom) return name.c_str();
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &SystemTrayIconComponent::proc;
    wc.hInstance = module();
    for (int attempt = 0; attempt < 64 && !atom; ++attempt) {
      wchar_t buffer[96];
      std::swprintf(buffer, 96, L"SonoreTrayIcon_%p_%d", (void*) module(), attempt);
      name = buffer;
      wc.lpszClassName = name.c_str();
      atom = RegisterClassExW(&wc);
      if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) break;
    }
    return atom ? name.c_str() : nullptr;
  }

  static UINT taskbarCreatedMessage() {
    static const UINT message = RegisterWindowMessageW(L"TaskbarCreated");
    return message;
  }

  static void copyText(wchar_t* to, size_t capacity, const std::string& utf8) {
    to[0] = 0;
    if (utf8.empty()) return;
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, to, (int) capacity);
    if (n <= 0) {
      // Too long: as much as fits, ended.
      std::wstring wide((size_t) MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0), L'\0');
      if (!wide.empty()) MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wide[0], (int) wide.size());
      const size_t count = (std::min)(capacity - 1, std::wcslen(wide.c_str()));
      std::wmemcpy(to, wide.c_str(), count);
      to[count] = 0;
    }
  }

  NOTIFYICONDATAW baseData() const {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = kIconId;
    return data;
  }

  static void record(BOOL ok, std::atomic<int>& counter) {
    if (ok) counter.fetch_add(1);
    else detail::tray::stats().failures.fetch_add(1);
  }

  bool ensureWindow() {
    if (hwnd_) return true;
    const wchar_t* cls = windowClass();
    if (!cls) return false;
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, cls, L"SonoreTrayIcon", WS_POPUP, 0, 0, 0, 0,
                            nullptr, nullptr, module(), nullptr);
    if (hwnd_) SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR) this);
    return hwnd_ != nullptr;
  }

  void addToTray() {
    if (!ensureWindow()) return;
    NOTIFYICONDATAW data = baseData();
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = kCallbackMessage;
    data.hIcon = icon_;
    copyText(data.szTip, sizeof(data.szTip) / sizeof(data.szTip[0]), tooltip_);
    const BOOL ok = Shell_NotifyIconW(NIM_ADD, &data);
    record(ok, detail::tray::stats().adds);
    if (!ok) return;
    added_ = true;
    // Version 4: the event in LOWORD(lParam), the icon in HIWORD.
    data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data);
  }

  void modify(UINT flags) {
    NOTIFYICONDATAW data = baseData();
    data.uFlags = flags;
    data.hIcon = icon_;
    copyText(data.szTip, sizeof(data.szTip) / sizeof(data.szTip[0]), tooltip_);
    if (flags & NIF_INFO) {
      copyText(data.szInfoTitle, sizeof(data.szInfoTitle) / sizeof(data.szInfoTitle[0]), infoTitle_);
      copyText(data.szInfo, sizeof(data.szInfo) / sizeof(data.szInfo[0]), infoText_);
      data.dwInfoFlags = NIIF_INFO;
    }
    record(Shell_NotifyIconW(NIM_MODIFY, &data), detail::tray::stats().modifies);
  }

  static LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<SystemTrayIconComponent*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && message == taskbarCreatedMessage()) {
      // Explorer restarted and forgot every icon -- or the taskbar was
      // remade for a DPI change and kept them. Deleted first (quietly: it
      // fails in the first case), so the add succeeds in both.
      if (self->added_) {
        NOTIFYICONDATAW data = self->baseData();
        Shell_NotifyIconW(NIM_DELETE, &data);
        self->added_ = false;
        self->addToTray();
      }
      return 0;
    }
    if (self && message == kCallbackMessage) {
      try {
        switch (LOWORD(lp)) {
          case WM_LBUTTONUP: self->handleTrayClick(false, 1); break;
          case WM_LBUTTONDBLCLK: self->handleTrayClick(false, 2); break;
          case WM_RBUTTONUP: self->handleTrayClick(true, 1); break;
          case NIN_BALLOONUSERCLICK: self->handleBalloonClick(); break;
          default: break;
        }
      } catch (const std::exception& e) {
        MessageManager::sendUnhandledException(&e);
      } catch (...) {
        MessageManager::sendUnhandledException(nullptr);
      }
      return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
  }

  HWND hwnd_ = nullptr;
  HICON icon_ = nullptr;
  bool added_ = false;
  std::string infoTitle_, infoText_;
#elif defined(__APPLE__)
  id statusButton() const { return item_ ? detail::tray::objc<id>(item_, "button") : nullptr; }

  static void clicked(id self, SEL, id) {
    void* value = nullptr;
    object_getInstanceVariable(self, "sonoreTray", &value);
    auto* tray = static_cast<SystemTrayIconComponent*>(value);
    if (!tray) return;
    id app = detail::tray::objc<id>((id) objc_getClass("NSApplication"), "sharedApplication");
    id event = app ? detail::tray::objc<id>(app, "currentEvent") : nullptr;
    // NSEventTypeRightMouseUp is 4; a control-click is a right click too.
    const unsigned long type = event ? detail::tray::objc<unsigned long>(event, "type") : 0;
    const unsigned long flags = event ? detail::tray::objc<unsigned long>(event, "modifierFlags") : 0;
    const long clicks = event ? detail::tray::objc<long>(event, "clickCount") : 1;
    const bool right = type == 4 || (flags & (1ul << 18)) != 0;
    tray->handleTrayClick(right, (int) (clicks > 0 ? clicks : 1));
  }

  static Class targetClass() {
    static Class klass = [] {
      char name[96];
      std::snprintf(name, sizeof(name), "SonoreTrayTarget_%llx",
                    (unsigned long long) (uintptr_t) (void*) &SystemTrayIconComponent::clicked);
      if (Class existing = objc_getClass(name)) return existing;
      Class c = objc_allocateClassPair(objc_getClass("NSObject"), name, 0);
      if (!c) return objc_getClass(name);
      class_addIvar(c, "sonoreTray", sizeof(void*), 3, "^v");
      class_addMethod(c, sel_registerName("sonoreTrayClicked:"), (IMP) &SystemTrayIconComponent::clicked,
                      "v@:@");
      objc_registerClassPair(c);
      return c;
    }();
    return klass;
  }

  void createStatusItem() {
    id bar = detail::tray::objc<id>((id) objc_getClass("NSStatusBar"), "systemStatusBar");
    if (!bar) return;
    // NSVariableStatusItemLength is -1.
    item_ = detail::tray::objc<id>(bar, "statusItemWithLength:", -1.0);
    if (!item_) return;
    detail::tray::objc<id>(item_, "retain");
    target_ = detail::tray::objc<id>(detail::tray::objc<id>((id) targetClass(), "alloc"), "init");
    if (target_) object_setInstanceVariable(target_, "sonoreTray", this);
    if (id button = statusButton()) {
      detail::tray::objc<void>(button, "setTarget:", target_);
      detail::tray::objc<void>(button, "setAction:", sel_registerName("sonoreTrayClicked:"));
      // Left and right mouse up: NSEventMaskLeftMouseUp | NSEventMaskRightMouseUp.
      detail::tray::objc<long>(button, "sendActionOn:", (unsigned long long) ((1ull << 2) | (1ull << 4)));
      if (!tooltip_.empty())
        detail::tray::objc<void>(button, "setToolTip:", detail::tray::nsString(tooltip_));
    }
  }

  void applyImage() {
    id button = statusButton();
    if (!button) return;
    const bool useTemplate = !template_.isEmpty();
    id image = detail::tray::imageFromBitmap(useTemplate ? template_ : colour_, 18.0);
    if (!image) return;
    if (useTemplate) detail::tray::objc<void>(image, "setTemplate:", (BOOL) YES);
    detail::tray::objc<void>(button, "setImage:", image);
  }

  id item_ = nullptr;
  id target_ = nullptr;
#endif

  Bitmap colour_, template_;
  std::string tooltip_;
  std::shared_ptr<char> alive_ = std::make_shared<char>('\0');
};

/**
 * Local notifications: post one, hear it clicked. The reference's
 * PushNotifications without the remote half (see the header). One per
 * process; message thread.
 */
class PushNotifications {
public:
  /** One local notification: an identifier and a title, both required, and
   *  optional subtitle and body text (UTF-8). A click is reported with the
   *  notification as it was posted, found by its identifier. */
  struct Notification {
    /** Required: what a click is reported with. */
    std::string identifier;
    /** Required: the headline. */
    std::string title;
    /** Optional second line. Windows puts it after the title, as
     *  "title - subtitle"; Linux leaves it out. */
    std::string subtitle;
    /** Optional text under the title. */
    std::string body;
    /** Both the identifier and the title are set: sendLocalNotification()
     *  posts nothing otherwise. */
    bool isValid() const { return !identifier.empty() && !title.empty(); }
  };

  /** Hears clicks on this program's notifications and the answer to
   *  requestPermissions(), on the message thread. Both default to doing
   *  nothing. Not owned: remove one from the instance before it dies. */
  class Listener {
  public:
    /** Does not remove it: call removeListener() first. */
    virtual ~Listener() = default;
    /** The user clicked a notification this program posted. */
    virtual void handleNotification(bool isLocalNotification, const Notification& n) {
      (void) isLocalNotification;
      (void) n;
    }
    /** The answer to requestPermissions(). */
    virtual void notificationSettingsReceived(bool allowed) { (void) allowed; }
  };

  /** The one instance, made on the first call. Never null. */
  static PushNotifications* getInstance() {
    static PushNotifications instance;
    return &instance;
  }

  /** Add a listener; null, or one already added, is ignored. Not owned. */
  void addListener(Listener* l) {
    if (l && std::find(listeners_.begin(), listeners_.end(), l) == listeners_.end())
      listeners_.push_back(l);
  }
  /** Remove a listener; one not added is ignored. A listener removed while
   *  a click or the permission answer is being reported is not called with
   *  it. */
  void removeListener(Listener* l) {
    listeners_.erase(std::remove(listeners_.begin(), listeners_.end(), l), listeners_.end());
  }

  /** Can this program post notifications here at all. */
  static bool isAvailable() {
#if defined(_WIN32)
    return true;
#elif defined(__APPLE__)
    return userNotifications() != nullptr;
#else
    return !findNotifySend().empty();
#endif
  }

  /** Ask the user (macOS); elsewhere answered yes at once. The answer
   *  arrives at notificationSettingsReceived. */
  void requestPermissions();
  /** The last answer to requestPermissions(): false until one arrives. */
  bool areNotificationsEnabled() const { return enabled_; }

  /**
   * Post one. Windows: as the balloon of `trayIcon` if one was set, else of
   * an icon of this class's own, which stays in the tray until
   * removeAllDeliveredNotifications(). False if it could not be posted.
   */
  bool sendLocalNotification(const Notification& n);

  /** Take what was posted away (Windows: the balloon and our own icon). */
  void removeAllDeliveredNotifications();

  /** Windows: post through this icon rather than one of our own. Not owned,
   *  and it may be destroyed first: it is then forgotten (and our own icon
   *  used again), never called. */
  void setTrayIcon(SystemTrayIconComponent* icon) {
    trayIcon_ = icon;
    trayIconAlive_ = icon ? std::weak_ptr<char>(icon->alive_) : std::weak_ptr<char>();
    if (icon) hookIcon(*icon);
  }

  /** Remote push is not implemented: there is never a device token. */
  std::string getDeviceToken() const { return std::string(); }

  /** Report a click, as the platform glue does. Message thread. */
  void notificationClicked(const std::string& identifier) {
    Notification found;
    found.identifier = identifier;
    for (const Notification& n : posted_)
      if (n.identifier == identifier) found = n;
    const std::vector<Listener*> copy = listeners_;
    for (Listener* l : copy)
      if (std::find(listeners_.begin(), listeners_.end(), l) != listeners_.end())
        l->handleNotification(true, found);
  }

  /** The tray icon our own notifications ride on (Windows), for a test. */
  SystemTrayIconComponent* ownTrayIcon() { return ownIcon_.get(); }

private:
  PushNotifications() = default;

  void hookIcon(SystemTrayIconComponent& icon) {
    icon.onBalloonClicked = [this]() {
      if (!lastIdentifier_.empty()) notificationClicked(lastIdentifier_);
    };
  }

  /** The application's icon, or null once it has been destroyed: a raw
   *  pointer kept past that was a heap-use-after-free on the next post. */
  SystemTrayIconComponent* liveTrayIcon() {
    if (trayIcon_ && trayIconAlive_.expired()) trayIcon_ = nullptr;
    return trayIcon_;
  }

  void settingsReceived(bool allowed) {
    enabled_ = allowed;
    // Each one re-checked, as for a click: a listener may remove -- and
    // delete -- another while it hears the answer.
    const std::vector<Listener*> copy = listeners_;
    for (Listener* l : copy)
      if (std::find(listeners_.begin(), listeners_.end(), l) != listeners_.end())
        l->notificationSettingsReceived(allowed);
  }

#if defined(__APPLE__)
  static id userNotifications() {
    // The framework is loaded on demand; the centre exists only in a bundle.
    static void* framework = dlopen(
        "/System/Library/Frameworks/UserNotifications.framework/UserNotifications", RTLD_LAZY);
    if (!framework) return nullptr;
    Class centre = objc_getClass("UNUserNotificationCenter");
    if (!centre) return nullptr;
    id bundle = detail::tray::objc<id>((id) objc_getClass("NSBundle"), "mainBundle");
    id bundleId = bundle ? detail::tray::objc<id>(bundle, "bundleIdentifier") : nullptr;
    if (!bundleId) return nullptr; // not an .app: the centre would throw
    return detail::tray::objc<id>((id) centre, "currentNotificationCenter");
  }

  /** A block literal as clang lays one out, for the one completion handler
   *  that cannot be nil: no captures, so a global block. */
  struct BlockDescriptor {
    unsigned long reserved;
    unsigned long size;
  };
  struct AuthorisationBlock {
    void* isa;
    int flags;
    int reserved;
    void (*invoke)(AuthorisationBlock*, BOOL, id);
    const BlockDescriptor* descriptor;
  };
  static void authorisationAnswered(AuthorisationBlock*, BOOL granted, id) {
    // On a queue of the system's: the answer goes to the message thread.
    const bool allowed = granted != NO;
    MessageManager::callAsync([allowed]() { getInstance()->settingsReceived(allowed); });
  }
#else
  static std::string findNotifySend() {
#if defined(_WIN32)
    return std::string();
#else
    const char* path = std::getenv("PATH");
    if (!path) return std::string();
    std::string all(path);
    size_t start = 0;
    while (start <= all.size()) {
      size_t end = all.find(':', start);
      if (end == std::string::npos) end = all.size();
      const std::string candidate = all.substr(start, end - start) + "/notify-send";
      if (end > start && access(candidate.c_str(), X_OK) == 0) return candidate;
      start = end + 1;
    }
    return std::string();
#endif
  }
#endif

  std::vector<Listener*> listeners_;
  std::vector<Notification> posted_;
  std::string lastIdentifier_;
  SystemTrayIconComponent* trayIcon_ = nullptr;
  std::weak_ptr<char> trayIconAlive_;
  std::unique_ptr<SystemTrayIconComponent> ownIcon_;
  bool enabled_ = false;
};

// ── Out of line: the two depend on each other ────────────────────────────────

inline void SystemTrayIconComponent::showInfoBubble(const std::string& title,
                                                    const std::string& content) {
#if defined(_WIN32)
  infoTitle_ = title;
  infoText_ = content;
  if (added_) modify(NIF_INFO);
#elif defined(__APPLE__)
  PushNotifications::Notification n;
  n.identifier = "sonore-tray-bubble";
  n.title = title;
  n.body = content;
  PushNotifications::getInstance()->sendLocalNotification(n);
#else
  (void) title;
  (void) content;
#endif
}

inline void PushNotifications::requestPermissions() {
#if defined(__APPLE__)
  id centre = userNotifications();
  if (!centre) {
    settingsReceived(false);
    return;
  }
  static void* globalBlockClass = dlsym(RTLD_DEFAULT, "_NSConcreteGlobalBlock");
  static const BlockDescriptor descriptor{0, sizeof(AuthorisationBlock)};
  static AuthorisationBlock block{globalBlockClass, 1 << 28 /* BLOCK_IS_GLOBAL */, 0,
                                  &PushNotifications::authorisationAnswered, &descriptor};
  // UNAuthorizationOptionBadge | Sound | Alert.
  detail::tray::objc<void>(centre, "requestAuthorizationWithOptions:completionHandler:",
                           (unsigned long) (1 | 2 | 4), (void*) &block);
#else
  settingsReceived(true);
#endif
}

inline bool PushNotifications::sendLocalNotification(const Notification& n) {
  if (!n.isValid()) return false;
  posted_.push_back(n);
  if (posted_.size() > 64) posted_.erase(posted_.begin());
  lastIdentifier_ = n.identifier;
#if defined(_WIN32)
  SystemTrayIconComponent* icon = liveTrayIcon();
  if (!icon) {
    if (!ownIcon_) {
      ownIcon_.reset(new SystemTrayIconComponent());
      // A plain mark for the icon: the application can set its own on
      // ownTrayIcon(), or pass its icon with setTrayIcon.
      Bitmap mark(16, 16);
      mark.clear(Colour(64, 120, 220));
      ownIcon_->setIconImage(mark, Bitmap());
      hookIcon(*ownIcon_);
    }
    icon = ownIcon_.get();
  }
  if (!icon->isInTray()) return false;
  const int before = detail::tray::stats().modifies.load();
  icon->showInfoBubble(n.subtitle.empty() ? n.title : n.title + " - " + n.subtitle, n.body);
  return detail::tray::stats().modifies.load() > before;
#elif defined(__APPLE__)
  id centre = userNotifications();
  if (!centre) return false;
  id content = detail::tray::objc<id>(
      detail::tray::objc<id>((id) objc_getClass("UNMutableNotificationContent"), "alloc"), "init");
  if (!content) return false;
  detail::tray::objc<void>(content, "setTitle:", detail::tray::nsString(n.title));
  if (!n.subtitle.empty())
    detail::tray::objc<void>(content, "setSubtitle:", detail::tray::nsString(n.subtitle));
  detail::tray::objc<void>(content, "setBody:", detail::tray::nsString(n.body));
  id request = detail::tray::objc<id>((id) objc_getClass("UNNotificationRequest"),
                                      "requestWithIdentifier:content:trigger:",
                                      detail::tray::nsString(n.identifier), content, (id) nullptr);
  detail::tray::objc<void>(content, "release");
  if (!request) return false;
  detail::tray::objc<void>(centre, "addNotificationRequest:withCompletionHandler:", request,
                           (void*) nullptr);
  return true;
#else
  const std::string program = findNotifySend();
  if (program.empty()) return false;
  std::vector<std::string> args{program, "--app-name=Sonore", n.title, n.body};
  std::vector<char*> argv;
  for (std::string& a : args) argv.push_back(&a[0]);
  argv.push_back(nullptr);
  pid_t pid = -1;
  if (posix_spawn(&pid, program.c_str(), nullptr, nullptr, argv.data(), environ) != 0) return false;
  // Reaped on a thread of its own: notify-send returns at once, but a
  // child left unreaped is a zombie for the life of the program.
  std::thread([pid]() {
    int status = 0;
    waitpid(pid, &status, 0);
  }).detach();
  return true;
#endif
}

inline void PushNotifications::removeAllDeliveredNotifications() {
  posted_.clear();
  lastIdentifier_.clear();
#if defined(_WIN32)
  if (SystemTrayIconComponent* icon = liveTrayIcon()) icon->hideInfoBubble();
  ownIcon_.reset();
#elif defined(__APPLE__)
  if (id centre = userNotifications())
    detail::tray::objc<void>(centre, "removeAllDeliveredNotifications");
#endif
}

} // namespace gfx
} // namespace sonore
