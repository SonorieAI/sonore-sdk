// SPDX-License-Identifier: Apache-2.0
//
// Relative positioning: a component's edges as EXPRESSIONS against its parent,
// its siblings and named markers -- "parent.width - 20", "slider.right + 5" --
// re-evaluated whenever anything they mention moves.
//
// ── What it is for ──────────────────────────────────────────────────────────
//
// The reference had this before it had FlexBox and Grid, and layouts written
// in it are still out there: component bounds stored as four strings, in a
// file or a data tree, so a designer could move things without a rebuild.
// A port of such an editor needs those strings to mean the same thing here.
// For new layouts FlexBox and Grid (layout.h) are the better tool, and nothing
// here replaces them.
//
// ── The pieces ──────────────────────────────────────────────────────────────
//
//   RelativeCoordinate  one number, as an expression
//   RelativePoint       two of them, "x, y"
//   RelativeRectangle   four, "left, top, right, bottom", and applyToComponent
//   RelativeParallelogram  three points: an image's corners under a shear
//   RelativePointPath   a Path whose points are RelativePoints
//   MarkerList          named coordinates a parent publishes for its children
//   RelativeCoordinatePositionerBase  what keeps a component where its
//                       expressions say, listening to everything they mention
//
// ── The names an expression can use ─────────────────────────────────────────
//
//   parent.left / right / top / bottom / x / y / width / height / centreX /
//     centreY -- the parent, in its OWN coordinates (left and top are 0)
//   <id>.<edge>   a sibling found by Component::componentID, in the parent's
//                 coordinates, which is where the positioned component lives
//   this.<edge>   the component's own current bounds
//   <marker>      a marker from a MarkerList given to the positioner
//   numbers, + - * / and parentheses, and min(a, b), max(a, b), abs(a)
//
// ── Cycles ──────────────────────────────────────────────────────────────────
//
// Two kinds, both detected rather than looped on. A marker defined in terms
// of itself, directly or through others, fails to evaluate with the chain
// named. Two components each positioned against the other is subtler: each
// move notifies the other, and the pair would chase each other forever. A
// positioner re-entered while it is still applying -- which is exactly that
// chase -- checks whether its answer has CHANGED: if not, the layout has
// converged and nothing is wrong (A's x from B and B's y from A is a valid
// layout); if so, it stops, and every positioner in the loop reports
// hasCycle().
//
// ── The evaluator ───────────────────────────────────────────────────────────
//
// detail::RelativeExpression is a small recursive-descent parser and tree
// evaluator written for this header, because the SDK has no general
// Expression class. It takes untrusted text (a layout read from a file), so it
// caps length and nesting and never recurses on input it has not bounded. If a
// core Expression class lands later, RelativeExpression's interface --
// parse(text, &error), evaluate(resolver, &error), toString(), symbols() -- is
// the seam to swap it behind; nothing outside this header touches the tree.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "component.h"
#include "path.h"

namespace sonore {
namespace gfx {

class MarkerList;

namespace detail {

/** A parsed expression. See the header comment for the grammar. */
class RelativeExpression {
public:
  /** Looks a name up: `object` empty for a bare name. Returns false with
   *  `error` set if it cannot. */
  using Resolver = std::function<bool(const std::string& object, const std::string& member,
                                      double* value, std::string* error)>;

  static constexpr size_t kMaxLength = 4096;
  static constexpr int kMaxDepth = 64;
  /** Operators, function calls and signs in one expression. Nesting alone
   *  does not bound the tree: "1+1+1+..." nests nothing and is as deep as it
   *  is long, so 4096 characters of it were a tree 2048 levels deep -- one
   *  the evaluator cut off at 256 levels and silently answered 256 for, and
   *  that printing, copying and freeing recursed through to the end of a
   *  debug build's stack. Counting every node bounds the depth everywhere. */
  static constexpr int kMaxOperations = kMaxDepth * 4;

  RelativeExpression() = default;
  explicit RelativeExpression(double constant) {
    root_.kind = Node::Number;
    root_.value = constant;
  }

  static RelativeExpression parse(const std::string& text, std::string* error) {
    RelativeExpression out;
    std::string why;
    if (text.size() > kMaxLength) {
      why = "expression longer than " + std::to_string(kMaxLength) + " characters";
    } else {
      Parser p{text, 0, {}, 0};
      Node n;
      if (p.expression(n, 0) && p.skipSpace() && p.at == text.size()) out.root_ = std::move(n);
      else why = p.error.empty() ? "unexpected '" + text.substr(p.at, 1) + "' at " + std::to_string(p.at) : p.error;
    }
    if (!why.empty()) out.root_ = Node{};
    out.valid_ = why.empty();
    if (error) *error = why;
    return out;
  }

  bool isValid() const { return valid_; }

  /** Whether anything in it is a name rather than a number. */
  bool usesSymbols() const { return usesSymbols(root_); }

  /** Every (object, member) it names, objects empty for bare names. */
  std::vector<std::pair<std::string, std::string>> symbols() const {
    std::vector<std::pair<std::string, std::string>> out;
    collect(root_, out);
    return out;
  }

  double evaluate(const Resolver& resolve, std::string* error) const {
    std::string why;
    const double v = eval(root_, resolve, why, 0);
    if (why.empty() && !std::isfinite(v)) why = "the result is not a finite number";
    if (error) *error = why;
    return why.empty() ? v : 0.0;
  }

  std::string toString() const { return print(root_); }

private:
  struct Node {
    enum Kind { Number, Symbol, Negate, Add, Subtract, Multiply, Divide, Call };
    Kind kind = Number;
    double value = 0.0;
    std::string object, member; // Symbol: object.member or member alone; Call: member
    std::vector<Node> kids;
  };

  struct Parser {
    const std::string& s;
    size_t at;
    std::string error;
    int operations;

    /** One more node above the leaves; see kMaxOperations. */
    bool operation() {
      if (++operations > kMaxOperations)
        return fail("more than " + std::to_string(kMaxOperations) + " operations in one expression");
      return true;
    }

    bool skipSpace() {
      while (at < s.size() && (s[at] == ' ' || s[at] == '\t' || s[at] == '\n' || s[at] == '\r')) ++at;
      return true;
    }
    bool peek(char c) {
      skipSpace();
      return at < s.size() && s[at] == c;
    }
    bool fail(const std::string& why) {
      if (error.empty()) error = why;
      return false;
    }
    static bool nameStart(char c) {
      return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    }
    static bool nameChar(char c) { return nameStart(c) || (c >= '0' && c <= '9'); }

    bool expression(Node& out, int depth) {
      if (depth > kMaxDepth) return fail("expression nested too deeply");
      if (!term(out, depth + 1)) return false;
      while (peek('+') || peek('-')) {
        const char op = s[at++];
        if (!operation()) return false;
        Node rhs;
        if (!term(rhs, depth + 1)) return false;
        Node n;
        n.kind = op == '+' ? Node::Add : Node::Subtract;
        n.kids.push_back(std::move(out));
        n.kids.push_back(std::move(rhs));
        out = std::move(n);
      }
      return true;
    }

    bool term(Node& out, int depth) {
      if (depth > kMaxDepth) return fail("expression nested too deeply");
      if (!factor(out, depth + 1)) return false;
      while (peek('*') || peek('/')) {
        const char op = s[at++];
        if (!operation()) return false;
        Node rhs;
        if (!factor(rhs, depth + 1)) return false;
        Node n;
        n.kind = op == '*' ? Node::Multiply : Node::Divide;
        n.kids.push_back(std::move(out));
        n.kids.push_back(std::move(rhs));
        out = std::move(n);
      }
      return true;
    }

    bool factor(Node& out, int depth) {
      if (depth > kMaxDepth) return fail("expression nested too deeply");
      skipSpace();
      if (at >= s.size()) return fail("expression ends where a value was expected");
      const char c = s[at];
      if (c == '-' || c == '+') {
        ++at;
        Node inner;
        if (!factor(inner, depth + 1)) return false;
        if (c == '+') {
          out = std::move(inner);
          return true;
        }
        if (!operation()) return false;
        out.kind = Node::Negate;
        out.kids.push_back(std::move(inner));
        return true;
      }
      if (c == '(') {
        ++at;
        if (!expression(out, depth + 1)) return false;
        if (!peek(')')) return fail("missing ')'");
        ++at;
        return true;
      }
      if ((c >= '0' && c <= '9') || c == '.') return number(out);
      if (nameStart(c)) return name(out, depth);
      return fail(std::string("unexpected '") + c + "' at " + std::to_string(at));
    }

    /** By hand rather than strtod, whose decimal point is the C locale's --
     *  a host that set a German locale would read "0.5" as 0. */
    bool number(Node& out) {
      double mantissa = 0.0;
      int digits = 0, exponent = 0;
      while (at < s.size() && s[at] >= '0' && s[at] <= '9') {
        if (digits < 17) mantissa = mantissa * 10.0 + (double) (s[at] - '0');
        else ++exponent;
        ++digits;
        ++at;
      }
      if (at < s.size() && s[at] == '.') {
        ++at;
        while (at < s.size() && s[at] >= '0' && s[at] <= '9') {
          if (digits < 17) {
            mantissa = mantissa * 10.0 + (double) (s[at] - '0');
            --exponent;
          }
          ++digits;
          ++at;
        }
      }
      if (digits == 0) return fail("a number with no digits");
      if (at < s.size() && (s[at] == 'e' || s[at] == 'E')) {
        size_t look = at + 1;
        int sign = 1;
        if (look < s.size() && (s[look] == '+' || s[look] == '-')) {
          if (s[look] == '-') sign = -1;
          ++look;
        }
        if (look < s.size() && s[look] >= '0' && s[look] <= '9') {
          int e = 0;
          while (look < s.size() && s[look] >= '0' && s[look] <= '9') {
            if (e < 10000) e = e * 10 + (s[look] - '0');
            ++look;
          }
          exponent += sign * e;
          at = look;
        }
      }
      out.kind = Node::Number;
      out.value = exponent == 0 ? mantissa : mantissa * std::pow(10.0, (double) exponent);
      // "1e999" is a number nobody can lay anything out at, and an infinity
      // in the tree would print as "inf", which does not parse back.
      if (!std::isfinite(out.value)) return fail("a number too large to be a coordinate");
      return true;
    }

    bool identifier(std::string& out) {
      const size_t start = at;
      while (at < s.size() && nameChar(s[at])) ++at;
      out = s.substr(start, at - start);
      return !out.empty();
    }

    bool name(Node& out, int depth) {
      std::string first;
      identifier(first);
      if (peek('(')) {
        ++at;
        if (!operation()) return false;
        out.kind = Node::Call;
        out.member = first;
        if (!peek(')')) {
          for (;;) {
            Node arg;
            if (!expression(arg, depth + 1)) return false;
            out.kids.push_back(std::move(arg));
            if (out.kids.size() > 8) return fail("too many arguments to " + first);
            if (peek(',')) {
              ++at;
              continue;
            }
            break;
          }
        }
        if (!peek(')')) return fail("missing ')' after the arguments to " + first);
        ++at;
        const size_t n = out.kids.size();
        if (!((first == "min" || first == "max") ? n == 2 : (first == "abs" ? n == 1 : false)))
          return fail("unknown function '" + first + "' with " + std::to_string(n) + " argument(s)");
        return true;
      }
      out.kind = Node::Symbol;
      if (at < s.size() && s[at] == '.') {
        ++at;
        std::string second;
        if (!identifier(second)) return fail("a name is needed after '" + first + ".'");
        out.object = first;
        out.member = second;
      } else {
        out.member = first;
      }
      return true;
    }
  };

  static bool usesSymbols(const Node& n) {
    if (n.kind == Node::Symbol) return true;
    for (const Node& k : n.kids)
      if (usesSymbols(k)) return true;
    return false;
  }

  static void collect(const Node& n, std::vector<std::pair<std::string, std::string>>& out) {
    if (n.kind == Node::Symbol) out.push_back({n.object, n.member});
    for (const Node& k : n.kids) collect(k, out);
  }

  /** Depth-bounded by construction: the parser refused a tree of more than
   *  kMaxOperations nodes above its leaves, so this recursion is too. The
   *  check below is the backstop, and says so rather than answering 0. */
  static double eval(const Node& n, const Resolver& resolve, std::string& why, int depth) {
    if (!why.empty()) return 0.0;
    if (depth > kMaxOperations) {
      why = "expression nested too deeply";
      return 0.0;
    }
    switch (n.kind) {
      case Node::Number: return n.value;
      case Node::Symbol: {
        double v = 0.0;
        if (!resolve) {
          why = "nothing to look up '" + n.member + "' in";
          return 0.0;
        }
        if (!resolve(n.object, n.member, &v, &why) && why.empty())
          why = "unknown name '" + (n.object.empty() ? n.member : n.object + "." + n.member) + "'";
        return v;
      }
      case Node::Negate: return -eval(n.kids[0], resolve, why, depth + 1);
      case Node::Add:
        return eval(n.kids[0], resolve, why, depth + 1) + eval(n.kids[1], resolve, why, depth + 1);
      case Node::Subtract:
        return eval(n.kids[0], resolve, why, depth + 1) - eval(n.kids[1], resolve, why, depth + 1);
      case Node::Multiply:
        return eval(n.kids[0], resolve, why, depth + 1) * eval(n.kids[1], resolve, why, depth + 1);
      case Node::Divide: {
        const double a = eval(n.kids[0], resolve, why, depth + 1);
        const double b = eval(n.kids[1], resolve, why, depth + 1);
        if (why.empty() && b == 0.0) why = "division by zero";
        return why.empty() ? a / b : 0.0;
      }
      case Node::Call: {
        const double a = eval(n.kids[0], resolve, why, depth + 1);
        if (n.member == "abs") return std::fabs(a);
        const double b = eval(n.kids[1], resolve, why, depth + 1);
        return n.member == "min" ? (a < b ? a : b) : (a > b ? a : b);
      }
    }
    return 0.0;
  }

  static std::string printNumber(double v) {
    char text[64];
    std::snprintf(text, sizeof(text), "%.10g", v);
    // %g in a host's locale may write a comma; the grammar wants a point.
    for (char* p = text; *p; ++p)
      if (*p == ',') *p = '.';
    return text;
  }

  static std::string print(const Node& n) {
    switch (n.kind) {
      case Node::Number: return printNumber(n.value);
      case Node::Symbol: return n.object.empty() ? n.member : n.object + "." + n.member;
      case Node::Negate: return "-(" + print(n.kids[0]) + ")";
      case Node::Add: return "(" + print(n.kids[0]) + " + " + print(n.kids[1]) + ")";
      case Node::Subtract: return "(" + print(n.kids[0]) + " - " + print(n.kids[1]) + ")";
      case Node::Multiply: return "(" + print(n.kids[0]) + " * " + print(n.kids[1]) + ")";
      case Node::Divide: return "(" + print(n.kids[0]) + " / " + print(n.kids[1]) + ")";
      case Node::Call: {
        std::string out = n.member + "(";
        for (size_t i = 0; i < n.kids.size(); ++i) out += (i ? ", " : "") + print(n.kids[i]);
        return out + ")";
      }
    }
    return {};
  }

  Node root_;
  bool valid_ = true;
};

/** Split "a, b, c" at TOP-LEVEL commas -- not those inside min(a, b). */
inline std::vector<std::string> splitRelativeList(const std::string& text) {
  std::vector<std::string> out;
  int depth = 0;
  std::string current;
  for (char c : text) {
    if (c == '(') ++depth;
    else if (c == ')' && depth > 0) --depth;
    if (c == ',' && depth == 0) {
      out.push_back(current);
      current.clear();
      continue;
    }
    current += c;
  }
  out.push_back(current);
  return out;
}

} // namespace detail

/** Where an expression's names are looked up: the parent whose coordinates
 *  everything is in, the component being positioned (for "this"), and the
 *  marker lists in force. */
struct RelativeScope {
  /** Whose coordinates the answer is in: what parent.left and the like, and
   *  a sibling's ID, refer to. Null where there is none. Not owned. */
  const Component* parent = nullptr;
  /** What this.left and the like refer to; may be null. Not owned. */
  const Component* self = nullptr;
  /** Searched in order for a bare name. Not owned. */
  std::vector<const MarkerList*> markers;
};

/**
 * One coordinate, as an expression.
 *
 * A constant is the degenerate case and the common one; it still goes through
 * the same type so that a layout mixing "10" and "parent.width - 10" needs no
 * second representation.
 */
class RelativeCoordinate {
public:
  /** The constant 0. */
  RelativeCoordinate() : expr_(0.0) {}
  /** A constant, in logical pixels. */
  RelativeCoordinate(double absolute) : expr_(absolute) {}
  /** Parses; an expression that does not parse is the constant 0 with
   *  error() saying why. */
  explicit RelativeCoordinate(const std::string& text)
      : expr_(detail::RelativeExpression::parse(text, &error_)) {}
  /** As the string form; null reads as an empty string. */
  explicit RelativeCoordinate(const char* text) : RelativeCoordinate(std::string(text ? text : "")) {}

  /** Whether the text parsed. */
  bool isValid() const { return error_.empty(); }
  /** Why the text did not parse; empty when it did. */
  const std::string& error() const { return error_; }
  /** Depends on something other than numbers. */
  bool isDynamic() const { return expr_.usesSymbols(); }
  /** The expression written out as text again. */
  std::string toString() const { return expr_.toString(); }
  /** Every (object, member) name the expression uses: the object empty for a
   *  marker, else "parent", "this" or a sibling's ID. */
  std::vector<std::pair<std::string, std::string>> symbols() const { return expr_.symbols(); }

  /** The value in `scope`; 0 with `error` set if a name cannot be found, a
   *  marker refers back to itself, or the result is not finite. */
  double resolve(const RelativeScope& scope, std::string* error = nullptr) const;

  /** The same, against a component's parent -- the common case. */
  double resolve(const Component& positioned, const std::vector<const MarkerList*>& markers = {},
                 std::string* error = nullptr) const {
    RelativeScope scope;
    scope.parent = positioned.parent();
    scope.self = &positioned;
    scope.markers = markers;
    return resolve(scope, error);
  }

  /** The parsed expression. */
  const detail::RelativeExpression& expression() const { return expr_; }

private:
  friend class MarkerList;
  /** With a chain of markers already being evaluated, for cycle detection. */
  double resolveWithin(const RelativeScope& scope, std::vector<std::string>& evaluating,
                       std::string* error) const;

  // error_ FIRST: the parsing constructor writes it from expr_'s
  // initialiser, and a member is constructed in declaration order -- the
  // other way round, the parse assigned into a string not yet constructed.
  std::string error_;
  detail::RelativeExpression expr_;
};

/**
 * Named coordinates a parent publishes for the components inside it: "the
 * gutter", "the centre line", defined once and used by every child's
 * expression. A marker may itself be an expression, including one in terms of
 * other markers; a chain that loops is reported, not followed.
 */
class MarkerList {
public:
  /** One named coordinate. `position` is an expression, resolved each time
   *  it is used, so a marker defined as "parent.width - 20" follows the
   *  parent. */
  struct Marker {
    /** What expressions call it. */
    std::string name;
    /** Its value, as an expression resolved where it is used. */
    RelativeCoordinate position;
  };

  /** Told when a marker is added, moved or removed, and when the list is
   *  being destroyed -- on the thread that made the change, which for a
   *  layout is the message thread. Not owned; a copy of the list does not
   *  copy its listeners. */
  class Listener {
  public:
    virtual ~Listener() = default;
    /** A marker was added, moved or removed, or the whole list was assigned. */
    virtual void markersChanged(MarkerList* list) = 0;
    /** The list is being destroyed: drop every pointer to it. */
    virtual void markerListBeingDeleted(MarkerList* list) { (void) list; }
  };

  /** No markers, no listeners. */
  MarkerList() = default;
  /** Copies the markers, not the listeners. */
  MarkerList(const MarkerList& other) : markers_(other.markers_) {}
  /** Takes `other`'s markers, keeps this list's listeners, and tells them. */
  MarkerList& operator=(const MarkerList& other) {
    if (this != &other) {
      markers_ = other.markers_;
      changed();
    }
    return *this;
  }
  /** Tells every listener markerListBeingDeleted. */
  ~MarkerList() {
    const std::vector<Listener*> told = listeners_;
    for (Listener* l : told) l->markerListBeingDeleted(this);
  }

  /** How many markers there are. */
  int getNumMarkers() const { return (int) markers_.size(); }
  /** The marker at `index`, or null out of range. Valid until the list
   *  changes. */
  const Marker* getMarker(int index) const {
    return (index >= 0 && index < (int) markers_.size()) ? &markers_[(size_t) index] : nullptr;
  }
  /** The marker called `name`, or null. Valid until the list changes. */
  const Marker* getMarker(const std::string& name) const {
    for (const Marker& m : markers_)
      if (m.name == name) return &m;
    return nullptr;
  }

  /** Add, or move an existing one. Listeners are told either way. */
  void setMarker(const std::string& name, const RelativeCoordinate& position) {
    for (Marker& m : markers_)
      if (m.name == name) {
        m.position = position;
        changed();
        return;
      }
    markers_.push_back({name, position});
    changed();
  }

  /** Remove the marker called `name`; listeners are told only if there was
   *  one. */
  void removeMarker(const std::string& name) {
    const size_t before = markers_.size();
    markers_.erase(std::remove_if(markers_.begin(), markers_.end(),
                                  [&](const Marker& m) { return m.name == name; }),
                   markers_.end());
    if (markers_.size() != before) changed();
  }

  /** A marker's value in `scope`. */
  double getMarkerPosition(const Marker& marker, const RelativeScope& scope,
                           std::string* error = nullptr) const {
    return marker.position.resolve(scope, error);
  }

  /** Not owned; adding one twice is a no-op. */
  void addListener(Listener* l) {
    if (l && std::find(listeners_.begin(), listeners_.end(), l) == listeners_.end())
      listeners_.push_back(l);
  }
  /** Safe to call from inside a notification. */
  void removeListener(Listener* l) {
    listeners_.erase(std::remove(listeners_.begin(), listeners_.end(), l), listeners_.end());
  }

private:
  void changed() {
    const std::vector<Listener*> told = listeners_;
    for (Listener* l : told)
      if (std::find(listeners_.begin(), listeners_.end(), l) != listeners_.end())
        l->markersChanged(this);
  }

  std::vector<Marker> markers_;
  std::vector<Listener*> listeners_;
};

inline double RelativeCoordinate::resolve(const RelativeScope& scope, std::string* error) const {
  std::vector<std::string> evaluating;
  return resolveWithin(scope, evaluating, error);
}

inline double RelativeCoordinate::resolveWithin(const RelativeScope& scope,
                                                std::vector<std::string>& evaluating,
                                                std::string* error) const {
  if (!error_.empty()) {
    if (error) *error = error_;
    return 0.0;
  }
  auto edge = [](const Rect& r, const std::string& member, bool own, double* out) {
    // `own` is the parent seen from inside: its origin is the origin.
    const float x = own ? 0.0f : r.x, y = own ? 0.0f : r.y;
    if (member == "left" || member == "x") *out = x;
    else if (member == "right") *out = (double) x + r.w;
    else if (member == "top" || member == "y") *out = y;
    else if (member == "bottom") *out = (double) y + r.h;
    else if (member == "width") *out = r.w;
    else if (member == "height") *out = r.h;
    else if (member == "centreX" || member == "centerX") *out = (double) x + r.w * 0.5;
    else if (member == "centreY" || member == "centerY") *out = (double) y + r.h * 0.5;
    else return false;
    return true;
  };
  detail::RelativeExpression::Resolver resolver = [&](const std::string& object,
                                                      const std::string& member, double* value,
                                                      std::string* why) -> bool {
    if (object.empty()) {
      for (const MarkerList* list : scope.markers) {
        if (!list) continue;
        const MarkerList::Marker* m = list->getMarker(member);
        if (!m) continue;
        if (std::find(evaluating.begin(), evaluating.end(), member) != evaluating.end()) {
          std::string chain;
          for (const std::string& n : evaluating) chain += n + " -> ";
          *why = "circular marker reference: " + chain + member;
          return false;
        }
        if (evaluating.size() >= (size_t) detail::RelativeExpression::kMaxDepth) {
          *why = "markers nested too deeply";
          return false;
        }
        evaluating.push_back(member);
        *value = m->position.resolveWithin(scope, evaluating, why);
        evaluating.pop_back();
        return why->empty();
      }
      *why = "unknown marker '" + member + "'";
      return false;
    }
    if (object == "parent") {
      if (!scope.parent) {
        *why = "'parent' used where there is no parent";
        return false;
      }
      if (edge(scope.parent->bounds(), member, true, value)) return true;
    } else if (object == "this") {
      if (!scope.self) {
        *why = "'this' used where there is no component";
        return false;
      }
      if (edge(scope.self->bounds(), member, false, value)) return true;
    } else {
      const Component* other = scope.parent ? scope.parent->findChildWithID(object) : nullptr;
      if (!other) {
        *why = "no component with the ID '" + object + "'";
        return false;
      }
      if (edge(other->bounds(), member, false, value)) return true;
    }
    *why = "unknown edge '" + member + "' of '" + object + "'";
    return false;
  };
  std::string why;
  const double v = expr_.evaluate(resolver, &why);
  if (error) *error = why;
  return why.empty() ? v : 0.0;
}

/** Two coordinates. Written "x, y". */
class RelativePoint {
public:
  RelativeCoordinate x, /**< across */ y; ///< down

  /** The constant (0, 0). */
  RelativePoint() = default;
  /** A constant point. */
  RelativePoint(Point absolute) : x(absolute.x), y(absolute.y) {}
  /** From two coordinates. */
  RelativePoint(const RelativeCoordinate& px, const RelativeCoordinate& py) : x(px), y(py) {}
  /** Parses "x, y": two expressions separated by a comma outside any
   *  parentheses, or error() says why not. */
  explicit RelativePoint(const std::string& text) {
    const std::vector<std::string> parts = detail::splitRelativeList(text);
    if (parts.size() != 2) {
      error_ = "a point needs two coordinates, \"x, y\"";
      return;
    }
    x = RelativeCoordinate(parts[0]);
    y = RelativeCoordinate(parts[1]);
  }

  /** Whether the text was two coordinates and both parsed. */
  bool isValid() const { return error_.empty() && x.isValid() && y.isValid(); }
  /** The first thing wrong with the text; empty when valid. */
  std::string error() const {
    if (!error_.empty()) return error_;
    return !x.isValid() ? x.error() : y.error();
  }
  /** Whether either coordinate names something other than numbers. */
  bool isDynamic() const { return x.isDynamic() || y.isDynamic(); }
  /** "x, y", as the string constructor reads it. */
  std::string toString() const { return x.toString() + ", " + y.toString(); }

  /** Both coordinates in `scope`. A coordinate that fails is 0, and the first
   *  error goes in `error`. */
  Point resolve(const RelativeScope& scope, std::string* error = nullptr) const {
    std::string ex, ey;
    const double px = x.resolve(scope, &ex), py = y.resolve(scope, &ey);
    if (error) *error = !ex.empty() ? ex : ey;
    return Point((float) px, (float) py);
  }

private:
  std::string error_;
};

class RelativeCoordinatePositionerBase;

/** Four coordinates: the edges. Written "left, top, right, bottom". */
class RelativeRectangle {
public:
  RelativeCoordinate left, /**< the left edge */ right, /**< the right edge */ top, /**< the top edge */ bottom; ///< the bottom edge

  /** All four edges the constant 0. */
  RelativeRectangle() = default;
  /** A constant rectangle. */
  RelativeRectangle(const Rect& r) : left(r.x), right(r.right()), top(r.y), bottom(r.bottom()) {}
  /** From four edges -- note the order: left, right, top, bottom, unlike the
   *  string form's left, top, right, bottom. */
  RelativeRectangle(const RelativeCoordinate& l, const RelativeCoordinate& r,
                    const RelativeCoordinate& t, const RelativeCoordinate& b)
      : left(l), right(r), top(t), bottom(b) {}
  /** Parses "left, top, right, bottom": four expressions separated by commas
   *  outside parentheses, or error() says why not. */
  explicit RelativeRectangle(const std::string& text) {
    const std::vector<std::string> parts = detail::splitRelativeList(text);
    if (parts.size() != 4) {
      error_ = "a rectangle needs four coordinates, \"left, top, right, bottom\"";
      return;
    }
    left = RelativeCoordinate(parts[0]);
    top = RelativeCoordinate(parts[1]);
    right = RelativeCoordinate(parts[2]);
    bottom = RelativeCoordinate(parts[3]);
  }

  /** Whether the text was four coordinates and all parsed. */
  bool isValid() const {
    return error_.empty() && left.isValid() && right.isValid() && top.isValid() && bottom.isValid();
  }
  /** The first thing wrong with the text; empty when valid. */
  std::string error() const {
    if (!error_.empty()) return error_;
    for (const RelativeCoordinate* c : {&left, &top, &right, &bottom})
      if (!c->isValid()) return c->error();
    return {};
  }
  /** Whether any edge names something other than numbers. */
  bool isDynamic() const {
    return left.isDynamic() || right.isDynamic() || top.isDynamic() || bottom.isDynamic();
  }
  /** "left, top, right, bottom", as the string constructor reads it. */
  std::string toString() const {
    return left.toString() + ", " + top.toString() + ", " + right.toString() + ", " +
           bottom.toString();
  }

  /** The rectangle in `scope`: width is right minus left, height bottom minus
   *  top, and either may come out negative. An edge that fails is 0, and the
   *  first error goes in `error`. */
  Rect resolve(const RelativeScope& scope, std::string* error = nullptr) const {
    std::string e[4];
    const double l = left.resolve(scope, &e[0]), t = top.resolve(scope, &e[1]);
    const double r = right.resolve(scope, &e[2]), b = bottom.resolve(scope, &e[3]);
    if (error) {
      error->clear();
      for (const std::string& one : e)
        if (!one.empty()) {
          *error = one;
          break;
        }
    }
    return Rect((float) l, (float) t, (float) (r - l), (float) (b - t));
  }

  /**
   * Keep `component` where this rectangle says, from now on: evaluated now,
   * and again whenever the parent, a sibling it names, or a marker in
   * `markers` changes. Replaces any positioner the component had. The
   * positioner is owned by the SDK and goes when the component does.
   */
  RelativeCoordinatePositionerBase& applyToComponent(Component& component,
                                                     std::vector<const MarkerList*> markers = {}) const;

private:
  std::string error_;
};

/** Three points -- top-left, top-right, bottom-left -- and so a rectangle
 *  under any affine transform: what a sheared or rotated image is placed by. */
class RelativeParallelogram {
public:
  RelativePoint topLeft, /**< where the source's top-left lands */ topRight, /**< its top-right */ bottomLeft; ///< its bottom-left

  /** All three corners at (0, 0). */
  RelativeParallelogram() = default;
  /** A rectangle's three corners, as constants. */
  RelativeParallelogram(const Rect& r)
      : topLeft(Point(r.x, r.y)), topRight(Point(r.right(), r.y)), bottomLeft(Point(r.x, r.bottom())) {}
  /** From the top-left, top-right and bottom-left corners. */
  RelativeParallelogram(const RelativePoint& tl, const RelativePoint& tr, const RelativePoint& bl)
      : topLeft(tl), topRight(tr), bottomLeft(bl) {}

  /** Whether any corner names something other than numbers. */
  bool isDynamic() const {
    return topLeft.isDynamic() || topRight.isDynamic() || bottomLeft.isDynamic();
  }

  /** The three corners; the fourth is topRight + bottomLeft - topLeft. */
  void resolveThreePoints(Point out[3], const RelativeScope& scope, std::string* error = nullptr) const {
    std::string e[3];
    out[0] = topLeft.resolve(scope, &e[0]);
    out[1] = topRight.resolve(scope, &e[1]);
    out[2] = bottomLeft.resolve(scope, &e[2]);
    if (error) *error = !e[0].empty() ? e[0] : (!e[1].empty() ? e[1] : e[2]);
  }

  /** The transform that carries `source`'s corners onto these three points. */
  Transform transformFrom(const Rect& source, const RelativeScope& scope,
                          std::string* error = nullptr) const {
    Point p[3];
    resolveThreePoints(p, scope, error);
    if (!(source.w != 0.0f && source.h != 0.0f)) return Transform();
    const float a = (p[1].x - p[0].x) / source.w, b = (p[1].y - p[0].y) / source.w;
    const float c = (p[2].x - p[0].x) / source.h, d = (p[2].y - p[0].y) / source.h;
    return Transform(a, b, c, d, p[0].x - a * source.x - c * source.y,
                     p[0].y - b * source.x - d * source.y);
  }

  /** The axis-aligned box around all four corners in `scope`. Errors are not
   *  reported: a corner that fails is (0, 0). */
  Rect getBoundingBox(const RelativeScope& scope) const {
    Point p[3];
    resolveThreePoints(p, scope);
    const Point q(p[1].x + p[2].x - p[0].x, p[1].y + p[2].y - p[0].y);
    float x0 = p[0].x, y0 = p[0].y, x1 = p[0].x, y1 = p[0].y;
    for (const Point& v : {p[1], p[2], q}) {
      x0 = (std::min)(x0, v.x);
      y0 = (std::min)(y0, v.y);
      x1 = (std::max)(x1, v.x);
      y1 = (std::max)(y1, v.y);
    }
    return Rect(x0, y0, x1 - x0, y1 - y0);
  }
};

/** A Path whose points are RelativePoints. */
class RelativePointPath {
public:
  /** A path command, as Path has them. */
  enum class ElementType { StartNewSubPath, /**< a move */ LineTo, /**< a line */ QuadraticTo, /**< a quadratic curve */ CubicTo, /**< a cubic curve */ CloseSubPath /**< back to the start */ };

  /** One path command and its points, as Path takes them: one for a move or
   *  a line, two for a quadratic (control, end), three for a cubic, none for
   *  a close. The unused slots are left default. */
  struct Element {
    /** Which command. */
    ElementType type;
    /** Its points, in the order the command takes them. */
    RelativePoint points[3];
  };

  /** Begin a new contour at `p`. */
  void startNewSubPath(const RelativePoint& p) { add(ElementType::StartNewSubPath, {p}); }
  /** A straight line to `p`. */
  void lineTo(const RelativePoint& p) { add(ElementType::LineTo, {p}); }
  /** A quadratic curve to `end`, pulled towards `control`. */
  void quadraticTo(const RelativePoint& control, const RelativePoint& end) {
    add(ElementType::QuadraticTo, {control, end});
  }
  /** A cubic curve to `end`, with control points `c1` and `c2`. */
  void cubicTo(const RelativePoint& c1, const RelativePoint& c2, const RelativePoint& end) {
    add(ElementType::CubicTo, {c1, c2, end});
  }
  /** Close the contour back to its start. */
  void closeSubPath() { add(ElementType::CloseSubPath, {}); }

  /** How many commands there are. */
  int size() const { return (int) elements_.size(); }
  /** Command `i`, which must be in range: it is not checked. */
  const Element& element(int i) const { return elements_[(size_t) i]; }

  /** Whether any point names something other than numbers. */
  bool containsAnyDynamicPoints() const {
    for (const Element& e : elements_)
      for (const RelativePoint& p : e.points)
        if (p.isDynamic()) return true;
    return false;
  }

  /** Every point resolved, in order. The first error, if any, is reported;
   *  the path is built regardless, with that point at 0. */
  Path createPath(const RelativeScope& scope, std::string* error = nullptr) const {
    Path path;
    if (error) error->clear();
    auto at = [&](const RelativePoint& p) {
      std::string why;
      const Point q = p.resolve(scope, &why);
      if (error && error->empty() && !why.empty()) *error = why;
      return q;
    };
    for (const Element& e : elements_) {
      switch (e.type) {
        case ElementType::StartNewSubPath: {
          const Point p = at(e.points[0]);
          path.moveTo(p.x, p.y);
          break;
        }
        case ElementType::LineTo: {
          const Point p = at(e.points[0]);
          path.lineTo(p.x, p.y);
          break;
        }
        case ElementType::QuadraticTo: {
          const Point c = at(e.points[0]), p = at(e.points[1]);
          path.quadTo(c.x, c.y, p.x, p.y);
          break;
        }
        case ElementType::CubicTo: {
          const Point c1 = at(e.points[0]), c2 = at(e.points[1]), p = at(e.points[2]);
          path.cubicTo(c1.x, c1.y, c2.x, c2.y, p.x, p.y);
          break;
        }
        case ElementType::CloseSubPath: path.close(); break;
      }
    }
    return path;
  }

private:
  void add(ElementType type, std::initializer_list<RelativePoint> points) {
    Element e;
    e.type = type;
    int i = 0;
    for (const RelativePoint& p : points) e.points[i++] = p;
    elements_.push_back(std::move(e));
  }

  std::vector<Element> elements_;
};

/**
 * Keeps one component where its expressions put it.
 *
 * Listens to the component (for a new parent), its parent (moved, resized,
 * children changed), every sibling an expression names, and every marker
 * list in force -- and re-applies when any of them changes. The dependencies
 * are worked out again on every apply, so an expression naming a sibling that
 * does not exist yet starts working when it is added.
 */
class RelativeCoordinatePositionerBase : private ComponentListener, private MarkerList::Listener {
public:
  /** Stops listening to its marker lists. Run by removeFrom, by a later
   *  applyToComponent on the same component, or when the component goes. */
  ~RelativeCoordinatePositionerBase() override {
    for (const MarkerList* m : markers_)
      if (m) const_cast<MarkerList*>(m)->removeListener(this);
  }

  /** The component it positions. */
  Component& getComponent() const { return *component_; }

  /** Evaluate and move the component, with the cycle check described at the
   *  top of this file. */
  void apply() {
    std::vector<RelativeCoordinatePositionerBase*>& stack = positioning();
    auto self = std::find(stack.begin(), stack.end(), this);
    if (self != stack.end()) {
      std::string why;
      const Rect again = computeBounds(&why);
      const Rect now = component_->bounds();
      if (why.empty() && again.x == now.x && again.y == now.y && again.w == now.w && again.h == now.h)
        return; // converged: re-entered with nothing left to change
      for (auto it = self; it != stack.end(); ++it) {
        (*it)->cycle_ = true;
        (*it)->error_ = "circular dependency between positioned components";
      }
      return;
    }
    cycle_ = false;
    error_.clear();
    std::string why;
    const Rect bounds = computeBounds(&why);
    if (!why.empty()) {
      error_ = why;
      updateDependencies();
      return;
    }
    // Listening to what it depends on BEFORE moving: the move may set off
    // the very positioner that depends back on this one, and if this one is
    // not yet listening the loop goes round once, unseen, and stops with
    // the two components disagreeing about where they are.
    updateDependencies();
    stack.push_back(this);
    component_->setBounds(bounds);
    stack.erase(std::remove(stack.begin(), stack.end(), this), stack.end());
  }

  /** Whether the last apply stopped because positioned components kept moving
   *  one another. */
  bool hasCycle() const { return cycle_; }
  /** Why the last apply did not move the component, or empty. */
  const std::string& lastError() const { return error_; }
  /** How many times it has evaluated, for a test asking what set it off. */
  int applyCount() const { return applies_; }

  /** Stop positioning this component. Destroys the positioner. */
  static void removeFrom(Component& c) {
    auto& all = registry();
    for (size_t i = 0; i < all.size(); ++i)
      if (all[i]->component_ == &c) {
        all.erase(all.begin() + (std::ptrdiff_t) i);
        return;
      }
  }

  /** The positioner keeping `c` in place, or null. */
  static RelativeCoordinatePositionerBase* positionerFor(const Component& c) {
    for (auto& p : registry())
      if (p->component_ == &c) return p.get();
    return nullptr;
  }

protected:
  /** Listen to `c` and to every list in `markers`. A list destroyed first tells
   *  the positioner, which forgets it. */
  RelativeCoordinatePositionerBase(Component& c, std::vector<const MarkerList*> markers)
      : component_(&c), markers_(std::move(markers)) {
    component_->addComponentListener(this);
    for (const MarkerList* m : markers_)
      if (m) const_cast<MarkerList*>(m)->addListener(this);
  }

  /** The bounds the expressions give, in the parent's coordinates. */
  virtual Rect computeBounds(std::string* error) const = 0;
  /** Every symbol the expressions use, for working out what to listen to. */
  virtual std::vector<std::pair<std::string, std::string>> usedSymbols() const = 0;

  /** The component's parent, the component and the marker lists: what the
   *  expressions are resolved in. */
  RelativeScope scope() const {
    RelativeScope s;
    s.parent = component_->parent();
    s.self = component_;
    s.markers = markers_;
    return s;
  }

  /** Every positioner alive. They are owned here. */
  static std::vector<std::unique_ptr<RelativeCoordinatePositionerBase>>& registry() {
    static std::vector<std::unique_ptr<RelativeCoordinatePositionerBase>> all;
    return all;
  }

private:
  static std::vector<RelativeCoordinatePositionerBase*>& positioning() {
    static std::vector<RelativeCoordinatePositionerBase*> stack;
    return stack;
  }

  void updateDependencies() {
    ++applies_;
    std::vector<Component*> wanted;
    Component* parent = component_->parent();
    if (parent) wanted.push_back(parent);
    for (const auto& sym : usedSymbols()) {
      if (sym.first.empty() || sym.first == "parent" || sym.first == "this" || !parent) continue;
      if (Component* other = parent->findChildWithID(sym.first))
        if (std::find(wanted.begin(), wanted.end(), other) == wanted.end()) wanted.push_back(other);
    }
    // Its own component always: that is how it hears it has a new parent,
    // and how it goes when the component does.
    wanted.push_back(component_);
    // Read back from the components' own record of who is attached (see
    // ComponentListener::watchedComponents), never from a list of our own.
    const std::vector<Component*> attached = watchedComponents();
    for (Component* c : attached)
      if (std::find(wanted.begin(), wanted.end(), c) == wanted.end()) c->removeComponentListener(this);
    for (Component* c : wanted) c->addComponentListener(this);
  }

  // ── ComponentListener ────────────────────────────────────────────────────

  void componentMovedOrResized(Component& c, bool, bool) override {
    if (&c != component_) apply(); // our own move is the result, not a cause
  }
  void componentParentHierarchyChanged(Component& c) override {
    if (&c == component_) apply();
  }
  void componentChildrenChanged(Component& c) override {
    if (&c == component_->parent()) apply();
  }
  void componentVisibilityChanged(Component&) override {}
  void componentBeingDeleted(Component& c) override {
    if (&c == component_) removeFrom(c); // last statement: this destroys the positioner
  }

  // ── MarkerList::Listener ─────────────────────────────────────────────────

  void markersChanged(MarkerList*) override { apply(); }
  void markerListBeingDeleted(MarkerList* list) override {
    markers_.erase(std::remove(markers_.begin(), markers_.end(), list), markers_.end());
  }

  Component* component_;
  std::vector<const MarkerList*> markers_;
  std::string error_;
  bool cycle_ = false;
  int applies_ = 0;

  friend class RelativeRectangle;
};

namespace detail {

class RelativeRectanglePositioner : public RelativeCoordinatePositionerBase {
public:
  RelativeRectanglePositioner(Component& c, const RelativeRectangle& r,
                              std::vector<const MarkerList*> markers)
      : RelativeCoordinatePositionerBase(c, std::move(markers)), rect_(r) {}

  Rect computeBounds(std::string* error) const override {
    if (!rect_.isValid()) {
      if (error) *error = rect_.error();
      return {};
    }
    return rect_.resolve(scope(), error);
  }

  std::vector<std::pair<std::string, std::string>> usedSymbols() const override {
    std::vector<std::pair<std::string, std::string>> out;
    for (const RelativeCoordinate* c : {&rect_.left, &rect_.top, &rect_.right, &rect_.bottom}) {
      const auto s = c->symbols();
      out.insert(out.end(), s.begin(), s.end());
    }
    return out;
  }

private:
  RelativeRectangle rect_;
};

} // namespace detail

inline RelativeCoordinatePositionerBase& RelativeRectangle::applyToComponent(
    Component& component, std::vector<const MarkerList*> markers) const {
  RelativeCoordinatePositionerBase::removeFrom(component);
  auto& all = RelativeCoordinatePositionerBase::registry();
  all.push_back(std::make_unique<detail::RelativeRectanglePositioner>(component, *this,
                                                                      std::move(markers)));
  RelativeCoordinatePositionerBase& p = *all.back();
  p.apply();
  return p;
}

} // namespace gfx
} // namespace sonore
