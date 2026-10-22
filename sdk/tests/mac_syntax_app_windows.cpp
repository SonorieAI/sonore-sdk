// SPDX-License-Identifier: Apache-2.0
// The application windows' macOS halves, compiled on Windows: the NSWindow
// frame and its delegate, the AppKit application delegate and
// SONORE_START_APPLICATION, NSAlert, the NSStatusItem tray icon,
// UNUserNotificationCenter, and NSViewComponent. Nothing here runs; the
// compiler reading it is the test, against tests/au_shim -- which proves
// every name exists and every call fits its declaration, not that the
// declarations match Apple's ABI (au_shim/README.md).
//
// Built like sonore_mac_syntax_app_infra: __APPLE__ defined, _WIN32
// undefined, SONORE_APPLE_SYNTAX_CHECK, the au_shim and au_shim/msvc include
// paths and posix_compat.h force-included. An OBJECT library: the main()
// the macro writes is compiled, never linked.
#if !defined(__APPLE__) || defined(_WIN32)
#error "build with __APPLE__ defined and _WIN32 undefined: see sonore_mac_syntax_* in CMakeLists.txt"
#endif

#include "sonore/application.h"
#include "sonore/gfx/document_window.h"
#include "sonore/gfx/message_box.h"
#include "sonore/gfx/native_embed.h"
#include "sonore/gfx/progress_window.h"
#include "sonore/gfx/splash_screen.h"
#include "sonore/gfx/system_tray.h"

#include <string>

// Proves the Cocoa branch was the one read: this class exists only there.
static_assert(sizeof(sonore::gfx::NSViewComponent) > 0, "the macOS branch");

namespace {

struct App : sonore::Application {
  const std::string getApplicationName() override { return "x"; }
  const std::string getApplicationVersion() override { return "1"; }
  bool moreThanOneInstanceAllowed() override { return false; }
  void initialise(const std::string&) override {}
  void shutdown() override {}
};

struct Job : sonore::gfx::ThreadWithProgressWindow {
  Job() : ThreadWithProgressWindow("x", true, true) {}
  void run() override { setProgress(0.5); }
};

} // namespace

SONORE_START_APPLICATION(App)

// Referenced so every inline body above is instantiated.
void sonoreMacSyntaxAppWindows() {
  using namespace sonore::gfx;
  // MacTypes.h's global Rect (declared by the shim as by Apple) would make
  // the unqualified name ambiguous; a local alias hides it.
  using Rect = sonore::gfx::Rect;
  DocumentWindow w("x", Colour(0, 0, 0), DocumentWindow::allButtons);
  w.setResizable(true, true);
  w.setResizeLimits(100, 100, 800, 600);
  w.setFixedAspectRatio(1.5f);
  w.setWindowBounds(Rect(10, 10, 300, 200));
  w.centreWithSize(300, 200);
  w.setVisible(true);
  w.setFullScreen(true);
  w.setMinimised(true);
  w.setKioskMode(true);
  w.setAlwaysOnTop(true);
  w.setWindowAlpha(0.5f);
  w.setName("y");
  w.setUsingNativeTitleBar(false);
  w.setMenuBar(nullptr);
  w.toFront(true);
  (void) w.getWindowBounds();
  (void) w.isActiveWindow();
  Bitmap icon(16, 16);
  w.setIcon(icon);
  w.removeFromDesktop();

  NativeMessageBox::show(MessageBoxOptions::makeOptionsOk(MessageBoxIconType::InfoIcon, "t", "m"));
  ScopedMessageBox scoped = NativeMessageBox::showScopedAsync(
      MessageBoxOptions::makeOptionsYesNo(MessageBoxIconType::QuestionIcon, "t", "m"), [](int) {});
  scoped.close();

  Job job;
  job.launchThread();

  SplashScreen* splash = new SplashScreen("s", icon, true);
  splash->deleteAfterDelay(100.0, true);

  SystemTrayIconComponent tray;
  tray.setIconImage(icon, icon);
  tray.setIconTooltip("t");
  tray.setHighlighted(true);
  tray.showInfoBubble("a", "b");
  PushNotifications::Notification n;
  n.identifier = "i";
  n.title = "t";
  PushNotifications::getInstance()->requestPermissions();
  PushNotifications::getInstance()->sendLocalNotification(n);
  PushNotifications::getInstance()->removeAllDeliveredNotifications();

  NSViewComponent view;
  view.setView(nullptr);
  view.resizeToFitView();
  view.updateBounds();
}
