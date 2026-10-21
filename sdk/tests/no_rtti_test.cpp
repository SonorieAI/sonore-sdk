// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the native editor's widgets with RTTI turned off.
//
// A widget finds its look-and-feel and font by walking up to the nearest
// ancestor that has one. That walk was a dynamic_cast, which a project built
// with RTTI off (/GR-, -fno-rtti) compiled with a warning and then threw from:
// "Access violation - no RTTI data!" on the first lookup. This file is built
// without RTTI; that it runs and finds the parent's look is the test.
#include <sonore/gfx/widgets.h>

#include <cstdio>

int main() {
  sonore::gfx::LookAndFeel custom;
  sonore::gfx::Label parent, child;
  parent.addChild(&child);
  parent.setLookAndFeel(&custom);
  const bool inherited = &child.lookAndFeel() == &custom;
  std::printf("  %s a widget inherits its parent's look with RTTI off\n", inherited ? "ok  " : "FAIL");
  return inherited ? 0 : 1;
}
