// SPDX-License-Identifier: Apache-2.0
// The application infrastructure's macOS halves, compiled on Windows: the
// message thread's CFRunLoopSource and NSApp loop, the distributed-
// notification broadcast, NSDocumentController's recent list and the NSAlert
// questions. Nothing here runs; the compiler reading it is the test, against
// tests/au_shim -- which proves every name exists and every call fits its
// declaration, not that the declarations match Apple's ABI (au_shim/README.md).
//
// Built like sonore_mac_syntax_plugin: __APPLE__ defined, _WIN32 undefined,
// SONORE_APPLE_SYNTAX_CHECK, the au_shim and au_shim/msvc include paths and
// posix_compat.h force-included.
#if !defined(__APPLE__) || defined(_WIN32)
#error "build with __APPLE__ defined and _WIN32 undefined: see sonore_mac_syntax_* in CMakeLists.txt"
#endif

#include "sonore/application_properties.h"
#include "sonore/file_document.h"
#include "sonore/message_manager.h"
#include "sonore/recent_files.h"
#include "sonore/gfx/popup.h"
#include "sonore/gfx/preferences_panel.h"

#include <string>

namespace {

struct Doc : sonore::FileBasedDocument {
  Doc() : FileBasedDocument(".x", "*.x", "Open", "Save") {}
  std::string getDocumentTitle() override { return "t"; }
  Result loadDocument(const std::string&) override { return Result::ok(); }
  Result saveDocument(const std::string&) override { return Result::ok(); }
};

} // namespace

// Referenced so every template and inline body above is instantiated.
void sonoreMacSyntaxAppInfra() {
  sonore::MessageManager* mm = sonore::MessageManager::getInstance();
  mm->runDispatchLoopUntil(1);
  mm->stopDispatchLoop();
  mm->runDispatchLoop();
  mm->setCurrentThreadAsMessageThread();
  sonore::MessageManager::broadcastMessage("x");
  sonore::ThreadExitSignal signal;
  { sonore::MessageManagerLock lock(signal); }
  sonore::LockingAsyncUpdater updater([]() {});
  updater.triggerAsyncUpdate();
  sonore::MessageManager::deleteInstance();

  sonore::RecentlyOpenedFilesList recent;
  sonore::gfx::PopupMenu menu;
  recent.createPopupMenuItems(menu, 1, false, false);
  sonore::RecentlyOpenedFilesList::registerRecentFileNatively("/tmp/x");
  sonore::RecentlyOpenedFilesList::forgetRecentFileNatively("/tmp/x");
  sonore::RecentlyOpenedFilesList::clearRecentFilesNatively();

  sonore::PropertiesFile::Options options;
  options.applicationName = "x";
  sonore::ApplicationProperties props;
  props.setStorageParameters(options);
  props.getCommonSettings(true);

  Doc doc;
  doc.saveIfNeededAndUserAgrees();
  sonore::gfx::PreferencesPanel panel;
  panel.addSettingsPage("General");
}
