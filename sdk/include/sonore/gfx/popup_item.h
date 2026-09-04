// SPDX-License-Identifier: Apache-2.0
//
// One row of a popup menu.
//
// Its own header for a dull, specific reason: LookAndFeel draws a PopupItem,
// PopupMenu is built out of widgets, and widgets are drawn by LookAndFeel. Any
// two of those can include each other; all three cannot.
//
// A forward declaration in lookandfeel.h would break the cycle and leave its
// popup methods declared but not defined -- which is fine until a translation
// unit uses a LookAndFeel without ever including popup.h, and then it is a
// link error about a vtable, in a header-only SDK, which is the least
// diagnosable thing this could possibly be. A struct with no dependencies but
// <string> costs nothing to include from both sides.
#pragma once

#include <string>

namespace sonore {
namespace gfx {

/**
 * A separator is an item rather than a separate list, so the index a hit-test
 * returns is the index the caller sees. Two parallel lists that have to stay
 * aligned is the shape of that bug.
 */
struct PopupItem {
  /** What a caller gets back. Zero is reserved for "dismissed", as it is
   *  everywhere: a menu whose items start at 1 can tell nothing-chosen from the
   *  first item, and one that starts at 0 cannot. */
  int id = 0;
  std::string text;
  bool enabled = true;
  bool ticked = false;
  bool isSeparator = false;
  bool isHeader = false;
};

} // namespace gfx
} // namespace sonore
