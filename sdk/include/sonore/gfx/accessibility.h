// SPDX-License-Identifier: Apache-2.0
//
// Walking the tree a screen reader would want to see.
//
// The vocabulary -- roles, names, values, ranges -- is in accessible_info.h,
// which Component includes so it can declare accessibleInfo(). This is the
// traversal, which needs Components and so cannot live there.
//
// Everything here is testable against a component tree with no window at all.
// That is the reason for the split: the platform bridges are the part that
// cannot be tested on this machine, and they should be a mechanical mapping
// from this rather than a second source of truth about what a knob is called.
#pragma once

#include <string>
#include <vector>

#include "accessible_info.h"
#include "component.h"

namespace sonore {
namespace gfx {

/**
 * One entry in the flattened tree a bridge walks.
 *
 * Flattened rather than nested, because every bridge needs both -- a linear
 * order for "next element" and a parent link for hierarchy -- and computing one
 * from the other twice is how the two end up disagreeing.
 */
struct AccessibleNode {
  Component* component = nullptr;
  int parent = -1;
  int depth = 0;
  AccessibleInfo info;
};

/**
 * The accessible tree, in reading order.
 *
 * Skips components that are invisible or marked ignored, AND their subtrees for
 * the invisible ones -- a hidden panel's contents are not merely unannounced,
 * they are not there. An IGNORED component keeps its children: that is the
 * difference between "this thing is not worth describing" and "none of this
 * exists", and a layout container is the first and not the second.
 */
inline void collectAccessible(Component* c, std::vector<AccessibleNode>& out, int parent = -1,
                              int depth = 0, Point offset = {0.0f, 0.0f}) {
  if (!c || !c->isVisible()) return;
  const Point here{offset.x + c->bounds().x, offset.y + c->bounds().y};

  int index = parent;
  if (!c->isAccessibilityIgnored()) {
    AccessibleNode node;
    node.component = c;
    node.parent = parent;
    node.depth = depth;
    node.info = c->accessibleInfo();
    node.info.bounds = Rect(here.x, here.y, c->bounds().w, c->bounds().h);
    out.push_back(node);
    index = (int) out.size() - 1;
    ++depth;
  }

  for (Component* child : c->children()) collectAccessible(child, out, index, depth, here);
}

/** The whole tree as one string, one line per element, indented by depth.
 *
 *  For tests and for a developer asking "what would a screen reader say about
 *  my editor" -- which is a question that otherwise has no answer short of
 *  installing one. */
inline std::string describeAccessibleTree(Component* root) {
  std::vector<AccessibleNode> nodes;
  collectAccessible(root, nodes);
  std::string out;
  for (const AccessibleNode& n : nodes) {
    out.append((size_t) n.depth * 2, ' ');
    out += accessibleRoleName(n.info.role);
    if (!n.info.name.empty()) out += " \"" + n.info.name + "\"";
    if (!n.info.value.empty()) out += " = " + n.info.value;
    if (!n.info.enabled) out += " (disabled)";
    if (n.info.focused) out += " (focused)";
    out += "\n";
  }
  return out;
}

} // namespace gfx
} // namespace sonore
