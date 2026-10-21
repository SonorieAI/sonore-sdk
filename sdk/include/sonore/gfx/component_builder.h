// SPDX-License-Identifier: Apache-2.0
//
// ComponentBuilder: a component tree built from a data tree, and kept in step
// with it.
//
// ── What it is for ──────────────────────────────────────────────────────────
//
// An interface described as DATA -- a layout a designer edits, a skin loaded
// with a preset, a panel a plugin assembles from what the user configured --
// has to become components, and has to stay right when the data changes. The
// second half is the hard half: rebuilding the whole tree on every edit loses
// focus, hover, scroll positions and every animation in flight, and a user
// typing in a field that was rebuilt under them loses their text. So the
// builder RECONCILES: a node whose id and type are unchanged keeps its
// component and is only told its new state; a new node gets a new component;
// a node that has gone takes its component with it; the order of children is
// made to match.
//
// ── The data ────────────────────────────────────────────────────────────────
//
// A JsonValue object per component, json.h's tree being the SDK's data tree:
//
//   { "type": "Panel", "id": "main", "bounds": [0, 0, 400, 300],
//     "children": [ { "type": "Knob", "id": "gain", ... }, ... ] }
//
//   type      chooses the TypeHandler that makes and updates it -- required
//   id        how a node is matched across edits (Component::componentID);
//             a node with none is matched by its position among the id-less
//             siblings of its type
//   bounds    optional: [x, y, w, h], or a RelativeRectangle string --
//             "parent.width - 110, 10, parent.width - 10, 40" -- which is
//             applied as a live relative position
//   children  optional: the nodes inside it, in z-order
//
// Everything else is the handler's to read. JsonValue is a VALUE -- there is
// no listener on it to tell the builder what changed -- so a change is a new
// tree handed to setState(), and the diff above is how only the changed parts
// are touched.
//
// The builder OWNS what it builds; Component itself does not own its
// children, and the tree here is the one place that decides their lifetime.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../json.h"
#include "component.h"
#include "relative_position.h"

namespace sonore {
namespace gfx {

/**
 * Builds a component tree from a JsonValue tree, and reconciles it when a new
 * tree is handed over. The reference's class of the same name, over json.h's
 * tree instead of its own; the file comment gives the node format and how a
 * node is matched across edits.
 *
 * Register a TypeHandler for every "type" first. getManagedComponent() builds
 * the tree on its first call, and setState() brings it up to date after that.
 * The builder OWNS every component it makes: a pointer from
 * getManagedComponent() or findComponentByID() dies with the builder, and with
 * the node -- a setState() that drops a node, or changes its type, destroys its
 * component. Message thread only.
 */
class ComponentBuilder {
public:
  /** The property a node is identified by. */
  static constexpr const char* idProperty = "id";

  /** Makes and updates the components for one "type". */
  class TypeHandler {
  public:
    /** Handles the nodes whose "type" is `type`. */
    explicit TypeHandler(std::string type) : type_(std::move(type)) {}
    virtual ~TypeHandler() = default;

    /** The "type" this handler builds. */
    const std::string& getType() const { return type_; }

    /** A new component for `state`. Not yet added to anything; the builder
     *  owns it and puts it in `parent` (null for the root). */
    virtual std::unique_ptr<Component> addNewComponentFromState(const JsonValue& state,
                                                                Component* parent) = 0;

    /** Make `component` match `state`. Called after creation and after every
     *  change to the node -- and ONLY then: an unchanged node is not told. */
    virtual void updateComponentFromState(Component& component, const JsonValue& state) = 0;

    /** The builder it is registered with; null before registerTypeHandler. */
    ComponentBuilder* getBuilder() const { return builder_; }

  private:
    friend class ComponentBuilder;
    std::string type_;
    ComponentBuilder* builder_ = nullptr;
  };

  /** No state: nothing is built until setState gives a tree. */
  ComponentBuilder() = default;
  /** Builds from `state`, on the first getManagedComponent(). */
  explicit ComponentBuilder(const JsonValue& state) : state_(state) {}

  /** Not copyable: it owns every component it built. */
  ComponentBuilder(const ComponentBuilder&) = delete;
  ComponentBuilder& operator=(const ComponentBuilder&) = delete;

  /** Take ownership of `handler`. One for a type already registered replaces
   *  it; null is ignored. Components already built are not rebuilt. */
  void registerTypeHandler(std::unique_ptr<TypeHandler> handler) {
    if (!handler) return;
    handler->builder_ = this;
    for (auto& h : handlers_)
      if (h->getType() == handler->getType()) {
        h = std::move(handler);
        return;
      }
    handlers_.push_back(std::move(handler));
  }

  /** How many types have a handler. */
  int getNumHandlers() const { return (int) handlers_.size(); }
  /** Handler `i` in registration order, or null out of range. Owned by the
   *  builder. */
  TypeHandler* getHandler(int i) const {
    return (i >= 0 && i < (int) handlers_.size()) ? handlers_[(size_t) i].get() : nullptr;
  }
  /** The handler for `state`'s "type", or null. */
  TypeHandler* getHandlerForState(const JsonValue& state) const {
    const std::string type = state["type"].asString();
    for (const auto& h : handlers_)
      if (h->getType() == type) return h.get();
    return nullptr;
  }

  /** The root component, built on first call. Null if the root's type has no
   *  handler. */
  Component* getManagedComponent() {
    if (!root_) reconcile(root_, state_, nullptr);
    return root_ ? root_->component.get() : nullptr;
  }

  /** The tree as last handed over. */
  const JsonValue& getState() const { return state_; }

  /** Move the tree to `state`, touching only what differs. */
  void setState(const JsonValue& state) {
    state_ = state;
    if (root_) reconcile(root_, state_, nullptr);
  }

  /** Anywhere in the built tree, by id. */
  Component* findComponentByID(const std::string& id) const {
    return root_ ? find(*root_, id) : nullptr;
  }

  /** Told about a node that could not be built -- no type, no handler, a
   *  bounds string that did not parse. The node is skipped, not fatal: a skin
   *  written for a newer version must still load. */
  std::function<void(const std::string&)> onError;

  /** How many components were made and how many updated, ever. */
  int componentsCreated() const { return created_; }
  /** How many times a handler was told to update a component -- creation
   *  included. */
  int componentsUpdated() const { return updated_; }

private:
  /** One built node: the id and type it was matched by, the component it
   *  owns, its own properties as last applied (compared on the next setState
   *  to decide whether the handler is told), and its children in z-order. */
  struct Node {
    std::string id, type;
    std::unique_ptr<Component> component;
    JsonValue state; // as last applied, children excluded
    std::vector<std::unique_ptr<Node>> children;
  };

  static JsonValue withoutChildren(const JsonValue& state) {
    JsonValue out = JsonValue::object();
    for (const auto& m : state.members())
      if (m.first != "children") out.set(m.first, m.second);
    return out;
  }

  void error(const std::string& why) {
    if (onError) onError(why);
  }

  /**
   * Make `slot` match `state`, inside `parent`. Creates, recreates (type
   * changed) or updates; then the same for the children, matched by id.
   */
  void reconcile(std::unique_ptr<Node>& slot, const JsonValue& state, Component* parent) {
    if (!state.isObject()) {
      error("a component node must be an object");
      slot.reset();
      return;
    }
    TypeHandler* handler = getHandlerForState(state);
    if (!handler) {
      error("no handler for type '" + state["type"].asString() + "'");
      slot.reset();
      return;
    }
    const std::string id = state[idProperty].asString();
    const JsonValue own = withoutChildren(state);

    if (slot && slot->type != handler->getType()) slot.reset(); // a new type is a new component
    if (!slot) {
      auto node = std::make_unique<Node>();
      node->type = handler->getType();
      node->component = handler->addNewComponentFromState(state, parent);
      if (!node->component) {
        error("the handler for '" + node->type + "' made no component");
        return;
      }
      ++created_;
      node->id = id;
      node->component->setComponentID(id);
      if (parent) parent->addChild(node->component.get());
      node->state = own;
      handler->updateComponentFromState(*node->component, state);
      ++updated_;
      applyBounds(*node->component, state);
      slot = std::move(node);
    } else {
      if (slot->component->parent() != parent && parent) parent->addChild(slot->component.get());
      slot->id = id;
      slot->component->setComponentID(id);
      // Compared as serialised text: JsonValue has no ==, and its objects
      // keep their keys sorted, so equal values write identical text.
      if (slot->state.toString() != own.toString()) {
        slot->state = own;
        handler->updateComponentFromState(*slot->component, state);
        ++updated_;
        applyBounds(*slot->component, state);
      }
    }
    reconcileChildren(*slot, state["children"]);
  }

  void reconcileChildren(Node& node, const JsonValue& children) {
    std::vector<std::unique_ptr<Node>> old = std::move(node.children);
    node.children.clear();
    const size_t count = children.isArray() ? children.size() : 0;
    for (size_t i = 0; i < count; ++i) {
      const JsonValue& childState = children.at(i);
      const std::string id = childState[idProperty].asString();
      const std::string type = childState["type"].asString();
      // The old node this one continues: same id, or -- for id-less nodes --
      // the first id-less one of the same type still unclaimed.
      std::unique_ptr<Node> match;
      for (auto& candidate : old) {
        if (!candidate) continue;
        const bool same = id.empty() ? (candidate->id.empty() && candidate->type == type)
                                     : candidate->id == id;
        if (same) {
          match = std::move(candidate);
          break;
        }
      }
      reconcile(match, childState, node.component.get());
      if (match) node.children.push_back(std::move(match));
    }
    // Whatever was not matched is gone from the data, and goes from the tree:
    // destroying the component takes it out of its parent.
    old.clear();
    // Z-order to match the data: each child brought to the front in turn
    // leaves them in data order, last on top.
    for (auto& child : node.children) child->component->toFront();
  }

  void applyBounds(Component& c, const JsonValue& state) {
    const JsonValue& b = state["bounds"];
    if (b.isArray() && b.size() == 4) {
      RelativeCoordinatePositionerBase::removeFrom(c);
      c.setBounds(Rect(b.at(0).asFloat(), b.at(1).asFloat(), b.at(2).asFloat(), b.at(3).asFloat()));
    } else if (b.isString()) {
      const RelativeRectangle r(b.asString());
      if (!r.isValid()) {
        error("bounds '" + b.asString() + "': " + r.error());
        return;
      }
      r.applyToComponent(c);
    }
  }

  static Component* find(const Node& node, const std::string& id) {
    if (node.id == id && node.component) return node.component.get();
    for (const auto& child : node.children)
      if (Component* c = find(*child, id)) return c;
    return nullptr;
  }

  JsonValue state_;
  std::vector<std::unique_ptr<TypeHandler>> handlers_;
  std::unique_ptr<Node> root_;
  int created_ = 0, updated_ = 0;
};

} // namespace gfx
} // namespace sonore
