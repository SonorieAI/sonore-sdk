// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: arithmetic parsed from text, with named symbols, evaluated at
// runtime.
//
// ── Why it is here after all ────────────────────────────────────────────────
//
// The earlier answer was that a generated plugin's arithmetic is code, which
// the compiler checks. True of the plugin's own DSP, and still the right
// place for it. It is not true of what a USER types: a value box that takes
// "440 * 2" or "-6 + 1.5", a modulation amount entered as a formula, a tuning
// written as ratios, a preset format that stores a curve as an expression.
// Those arrive at runtime as text, and the choice is between parsing them
// properly once, here, or badly in every plugin that wants it.
//
// ── The language ────────────────────────────────────────────────────────────
//
//     expression := term (('+' | '-') term)*
//     term       := unary (('*' | '/') unary)*
//     unary      := ('+' | '-')* primary
//     primary    := number | '(' expression ')'
//                 | symbol | function '(' [expression (',' expression)*] ')'
//
// Numbers are decimal with an optional fraction and exponent, read with '.'
// as the point whatever locale the host set. Symbols and functions are
// identifiers, dotted ones included ("lfo.rate"), resolved through a Scope at
// evaluation -- so one parsed expression can be evaluated against different
// values without being parsed again. A symbol's value is itself an
// Expression, so symbols may be defined in terms of each other.
//
// Arithmetic is IEEE double, left to right in the order written, as C++ does
// it: 1/0 is infinity and sqrt(-1) is NaN, not errors. Errors are for what
// cannot be evaluated at all -- an unknown symbol or function, the wrong
// number of arguments -- and are reported as messages, never thrown.
//
// ── Safe against what is typed ──────────────────────────────────────────────
//
// Parsing has limits on length, parenthesis depth and term count. Evaluation
// has a step budget and detects a symbol defined in terms of itself. And
// nothing that walks a tree recurses -- evaluating, printing, simplifying and
// destroying all use an explicit stack -- so even an expression built in code
// a million operators deep cannot exhaust a thread's stack. Only the parser
// recurses, and only as deep as the depth limit lets it.
//
// Not for the audio thread: evaluation allocates its working stacks. Parse
// and evaluate on the message thread and hand the number across.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "number_text.h"

namespace sonore {

/**
 * A parsed arithmetic expression. Immutable and cheap to copy: copies share
 * the parsed tree, so one expression may be evaluated from several threads at
 * once.
 *
 * Built in code, an expression can share one subexpression many times over
 * (`e = e + e`, repeated). evaluate() is protected from that by its step
 * budget; toString(), simplified() and the symbol queries walk every path and
 * are not -- don't print what you built that way. A parsed expression is
 * always a plain tree.
 */
class Expression {
public:
  /** What kind of node the top of the expression is, as the reference names
   *  them. Unary minus is an operator with one input. */
  enum class Type {
    constantType, ///< a number
    functionType, ///< a call: getSymbolOrFunction() names it
    operatorType, ///< + - * / with two inputs, or unary minus with one
    symbolType    ///< a name a Scope gives a value
  };

  /** What parsing and evaluation may cost before they give up. */
  struct Limits {
    size_t maxLength = 65536;            ///< characters of text
    int maxDepth = 100;                  ///< nested parentheses and calls; symbol chains
    size_t maxNodes = 65536;             ///< numbers, symbols, operators and calls
    size_t maxEvaluationSteps = 1000000; ///< nodes visited by one evaluate()
  };

  /**
   * Where symbols get their values and functions get evaluated.
   *
   * The base class knows no symbols and evaluates the standard functions
   * (see evaluateStandardFunction). Override getSymbolValue to supply values;
   * override evaluateFunction to add functions, and call the base for the
   * standard ones.
   */
  class Scope {
  public:
    /** Virtual: a Scope is subclassed. */
    virtual ~Scope() = default;

    /** Sets `value` and returns true if `symbol` is known here. */
    virtual bool getSymbolValue(const std::string& /*symbol*/, Expression& /*value*/) const {
      return false;
    }

    /** Sets `result` and returns true; or returns false with `error` set (an
     *  empty error is reported as "unknown function"). */
    virtual bool evaluateFunction(const std::string& functionName, const double* parameters,
                                  int numParameters, double& result, std::string& error) const {
      return evaluateStandardFunction(functionName, parameters, numParameters, result, error);
    }
  };

  /** A Scope holding named values: the usual way to give symbols values. */
  class VariableScope;

  /** The constant 0. */
  Expression() : node_(zeroNode()) {}
  /** The constant `constant`. */
  explicit Expression(double constant) : node_(makeConstant(constant)) {}

  /** Parses `stringToParse`; on failure the expression is 0 and `parseError`
   *  says why ("column 7: expected ')'"). On success `parseError` is "". */
  Expression(const std::string& stringToParse, std::string& parseError)
      : Expression(parse(stringToParse, &parseError)) {}

  /** Parses with the default limits. */
  static Expression parse(const std::string& text, std::string* errorOut = nullptr) {
    return parse(text, errorOut, Limits());
  }
  /** Parses within `limits`. On failure the expression is 0 and
   *  `*errorOut`, when given, says why; on success it is "". */
  static Expression parse(const std::string& text, std::string* errorOut, const Limits& limits) {
    Parser p(text, limits);
    std::shared_ptr<const Node> root;
    if (text.size() > limits.maxLength) {
      p.fail("the expression is longer than the limit allows");
    } else {
      p.skipSpace();
      if (p.pos >= text.size()) {
        p.fail("empty expression");
      } else {
        root = p.parseAdditive(0);
        if (root) {
          p.skipSpace();
          if (p.pos < text.size()) p.fail("unexpected " + p.describeHere());
        }
      }
    }
    if (errorOut) *errorOut = p.error;
    if (!p.error.empty() || !root) return Expression();
    return Expression(std::move(root));
  }

  // ── Building in code ─────────────────────────────────────────────────────

  /** The symbol `name`, its value looked up in a Scope at evaluation. The
   *  name is not checked: toString() writes it as given. */
  static Expression symbol(const std::string& name) {
    auto n = std::make_shared<Node>();
    n->op = Op::Symbol;
    n->name = name;
    return Expression(std::move(n));
  }
  /** A call of `name` with `parameters`, evaluated by a Scope. The name is
   *  not checked against any Scope until evaluation. */
  static Expression function(const std::string& name, const std::vector<Expression>& parameters) {
    auto n = std::make_shared<Node>();
    n->op = Op::Function;
    n->name = name;
    for (const Expression& p : parameters) n->inputs.push_back(p.root());
    return Expression(std::move(n));
  }
  ///@{
  /** A new expression applying the operator to this one and `other`, both
   *  shared rather than copied. Nothing is evaluated or simplified. */
  Expression operator+(const Expression& other) const { return binary(Op::Add, other); }
  Expression operator-(const Expression& other) const { return binary(Op::Subtract, other); }
  Expression operator*(const Expression& other) const { return binary(Op::Multiply, other); }
  Expression operator/(const Expression& other) const { return binary(Op::Divide, other); }
  ///@}
  /** This expression negated: unary minus, an operator with one input. */
  Expression operator-() const {
    auto n = std::make_shared<Node>();
    n->op = Op::Negate;
    n->inputs.push_back(root());
    return Expression(std::move(n));
  }

  // ── Evaluating ───────────────────────────────────────────────────────────

  /** Evaluated with no symbols and the standard functions; 0 on error. */
  double evaluate() const {
    std::string error;
    return evaluate(Scope(), error);
  }
  /** Evaluated against `scope`; 0 on error, with no way to tell an error
   *  from a value of 0 -- the overload below can. */
  double evaluate(const Scope& scope) const {
    std::string error;
    return evaluate(scope, error);
  }
  /** The value, or 0 with `evaluationError` set. */
  double evaluate(const Scope& scope, std::string& evaluationError) const {
    return evaluate(scope, evaluationError, Limits());
  }
  /** The same within `limits`: maxEvaluationSteps bounds the work and
   *  maxDepth how deeply symbols may be defined through each other. */
  double evaluate(const Scope& scope, std::string& evaluationError, const Limits& limits) const {
    evaluationError.clear();
    struct Frame {
      const Node* node;
      size_t next;                     // inputs already pushed
      size_t base;                     // where this node's input values start
      std::shared_ptr<const Node> keep; // a symbol's expression, held while it runs
    };
    std::vector<Frame> frames;
    std::vector<double> values;
    std::vector<const std::string*> expanding; // symbols being evaluated, outermost first
    size_t steps = 0;
    frames.push_back(Frame{root().get(), 0, 0, nullptr});
    while (!frames.empty()) {
      if (++steps > limits.maxEvaluationSteps)
        return failed(evaluationError, "evaluation took more steps than the limit allows");
      Frame& f = frames.back();
      const Node& n = *f.node;
      if (n.op == Op::Constant) {
        values.push_back(n.value);
        frames.pop_back();
        continue;
      }
      if (n.op == Op::Symbol) {
        if (f.next == 0) {
          f.next = 1;
          for (const std::string* s : expanding)
            if (*s == n.name)
              return failed(evaluationError, "symbol " + n.name + " is defined in terms of itself");
          if (expanding.size() >= (size_t) (limits.maxDepth > 0 ? limits.maxDepth : 0))
            return failed(evaluationError, "symbols refer to each other too deeply");
          Expression value;
          if (!scope.getSymbolValue(n.name, value))
            return failed(evaluationError, "unknown symbol: " + n.name);
          expanding.push_back(&n.name);
          std::shared_ptr<const Node> held = value.root();
          const Node* target = held.get();
          frames.push_back(Frame{target, 0, values.size(), std::move(held)});
        } else {
          expanding.pop_back();
          frames.pop_back();
        }
        continue;
      }
      if (f.next < n.inputs.size()) {
        const Node* child = n.inputs[f.next++].get();
        frames.push_back(Frame{child, 0, values.size(), nullptr});
        continue;
      }
      const double* in = values.data() + f.base;
      const size_t count = values.size() - f.base;
      double result = 0.0;
      switch (n.op) {
        case Op::Add: result = in[0] + in[1]; break;
        case Op::Subtract: result = in[0] - in[1]; break;
        case Op::Multiply: result = in[0] * in[1]; break;
        case Op::Divide: result = in[0] / in[1]; break;
        case Op::Negate: result = -in[0]; break;
        case Op::Function: {
          std::string functionError;
          if (!scope.evaluateFunction(n.name, count ? in : nullptr, (int) count, result, functionError))
            return failed(evaluationError,
                          functionError.empty() ? "unknown function: " + n.name : functionError);
          break;
        }
        default: break;
      }
      values.resize(f.base);
      values.push_back(result);
      frames.pop_back();
    }
    return values.empty() ? 0.0 : values.back();
  }

  /**
   * The standard functions: sin cos tan asin acos atan sqrt abs exp log
   * log10 floor ceil round (one argument), pow atan2 fmod (two), and min max
   * (one or more). What the base Scope evaluates; public so a Scope that adds
   * functions can fall back on it.
   */
  static bool evaluateStandardFunction(const std::string& name, const double* p, int count,
                                       double& result, std::string& error) {
    struct Unary {
      const char* name;
      double (*fn)(double);
    };
    static const Unary unary[] = {
        {"sin", [](double x) { return std::sin(x); }},     {"cos", [](double x) { return std::cos(x); }},
        {"tan", [](double x) { return std::tan(x); }},     {"asin", [](double x) { return std::asin(x); }},
        {"acos", [](double x) { return std::acos(x); }},   {"atan", [](double x) { return std::atan(x); }},
        {"sqrt", [](double x) { return std::sqrt(x); }},   {"abs", [](double x) { return std::fabs(x); }},
        {"exp", [](double x) { return std::exp(x); }},     {"log", [](double x) { return std::log(x); }},
        {"log10", [](double x) { return std::log10(x); }}, {"floor", [](double x) { return std::floor(x); }},
        {"ceil", [](double x) { return std::ceil(x); }},   {"round", [](double x) { return std::round(x); }},
    };
    for (const Unary& u : unary)
      if (name == u.name) {
        if (count != 1) return arityError(error, name, "1 argument", count);
        result = u.fn(p[0]);
        return true;
      }
    if (name == "pow" || name == "atan2" || name == "fmod") {
      if (count != 2) return arityError(error, name, "2 arguments", count);
      result = name == "pow" ? std::pow(p[0], p[1])
               : name == "atan2" ? std::atan2(p[0], p[1])
                                 : std::fmod(p[0], p[1]);
      return true;
    }
    if (name == "min" || name == "max") {
      if (count < 1) return arityError(error, name, "at least 1 argument", count);
      const bool lowest = name == "min";
      result = p[0];
      for (int i = 1; i < count; ++i)
        if (lowest ? p[i] < result : p[i] > result) result = p[i];
      return true;
    }
    error = "unknown function: " + name;
    return false;
  }

  // ── Looking inside ───────────────────────────────────────────────────────

  /** What kind of node the top of the expression is. */
  Type getType() const {
    switch (root()->op) {
      case Op::Constant: return Type::constantType;
      case Op::Symbol: return Type::symbolType;
      case Op::Function: return Type::functionType;
      default: return Type::operatorType;
    }
  }
  /** A symbol's or function's name, or an operator: "+", "-", "*", "/" (and
   *  "-" for negation, with one input). "" for a constant. */
  std::string getSymbolOrFunction() const {
    const Node& n = *root();
    switch (n.op) {
      case Op::Symbol:
      case Op::Function: return n.name;
      case Op::Add: return "+";
      case Op::Subtract:
      case Op::Negate: return "-";
      case Op::Multiply: return "*";
      case Op::Divide: return "/";
      default: return std::string();
    }
  }
  /** The top node's inputs: 2 for a binary operator, 1 for unary minus, the
   *  argument count for a function, 0 for a constant or a symbol. */
  int getNumInputs() const { return (int) root()->inputs.size(); }
  /** The index'th input of an operator or function; 0 when out of range. */
  Expression getInput(int index) const {
    const Node& n = *root();
    if (index < 0 || (size_t) index >= n.inputs.size()) return Expression();
    return Expression(n.inputs[(size_t) index]);
  }

  /** Does `name` appear as a symbol in this expression? (Symbols' own values
   *  are not followed: that needs a Scope.) */
  bool referencesSymbol(const std::string& name) const {
    for (const std::string& s : findReferencedSymbols())
      if (s == name) return true;
    return false;
  }
  /** Every symbol named in this expression, each once, in order of first
   *  appearance. */
  std::vector<std::string> findReferencedSymbols() const {
    std::vector<std::string> out;
    std::vector<const Node*> pending{root().get()};
    while (!pending.empty()) {
      const Node* n = pending.back();
      pending.pop_back();
      if (n->op == Op::Symbol) {
        bool seen = false;
        for (const std::string& s : out) seen = seen || s == n->name;
        if (!seen) out.push_back(n->name);
      }
      for (size_t i = n->inputs.size(); i-- > 0;) pending.push_back(n->inputs[i].get());
    }
    return out;
  }

  /** A copy with every use of symbol `oldName` named `newName` instead. */
  Expression withRenamedSymbol(const std::string& oldName, const std::string& newName) const {
    return Expression(foldTree<std::shared_ptr<const Node>>(
        root(), [&](const std::shared_ptr<const Node>& n, std::shared_ptr<const Node>* in, size_t count) {
          if (n->op == Op::Symbol && n->name == oldName) {
            auto renamed = std::make_shared<Node>(*n);
            renamed->name = newName;
            return std::shared_ptr<const Node>(std::move(renamed));
          }
          return rebuiltWith(n, in, count);
        }));
  }

  /** Every operator whose inputs are all constants replaced by its value --
   *  "x * (2 * 3)" becomes "x * 6", "-(1 + 2) + x" becomes "-3 + x".
   *  Functions are not folded: a Scope may define them differently. Nor is
   *  anything whose value would be infinite or NaN, so the result still
   *  prints as text that parses. */
  Expression simplified() const {
    return Expression(foldTree<std::shared_ptr<const Node>>(
        root(), [](const std::shared_ptr<const Node>& n, std::shared_ptr<const Node>* in, size_t count) {
          const bool arithmetic = n->op == Op::Add || n->op == Op::Subtract ||
                                  n->op == Op::Multiply || n->op == Op::Divide ||
                                  n->op == Op::Negate;
          bool allConstant = arithmetic && count > 0;
          for (size_t i = 0; allConstant && i < count; ++i) allConstant = in[i]->op == Op::Constant;
          if (allConstant) {
            double v = 0.0;
            switch (n->op) {
              case Op::Add: v = in[0]->value + in[1]->value; break;
              case Op::Subtract: v = in[0]->value - in[1]->value; break;
              case Op::Multiply: v = in[0]->value * in[1]->value; break;
              case Op::Divide: v = in[0]->value / in[1]->value; break;
              default: v = -in[0]->value; break;
            }
            if (std::isfinite(v)) return makeConstant(v);
          }
          return rebuiltWith(n, in, count);
        }));
  }

  /** The expression as text that parses back to the same tree: spaces
   *  around binary operators, parentheses only where they are needed, and
   *  numbers in the fewest digits that read back exactly. */
  std::string toString() const {
    struct Printed {
      std::string text;
      int precedence; // 1 + -, 2 * /, 3 unary minus, 4 an atom
    };
    Printed p = foldTree<Printed>(
        root(), [](const std::shared_ptr<const Node>& n, Printed* in, size_t count) {
          switch (n->op) {
            case Op::Constant: {
              std::string t = formatNumber(n->value);
              return Printed{t, t[0] == '-' ? 3 : 4};
            }
            case Op::Symbol: return Printed{n->name, 4};
            case Op::Function: {
              std::string t = n->name + "(";
              for (size_t i = 0; i < count; ++i) {
                if (i) t += ", ";
                t += in[i].text;
              }
              return Printed{t + ")", 4};
            }
            case Op::Negate:
              return Printed{"-" + (in[0].precedence < 3 ? "(" + in[0].text + ")" : in[0].text), 3};
            default: {
              const int prec = (n->op == Op::Add || n->op == Op::Subtract) ? 1 : 2;
              const char* sym = n->op == Op::Add ? " + "
                                : n->op == Op::Subtract ? " - "
                                : n->op == Op::Multiply ? " * "
                                                        : " / ";
              // Left-associative: the right side keeps its parentheses at
              // equal precedence, or "a - (b - c)" would read back as
              // "(a - b) - c" -- and even "a + (b + c)" rounds differently.
              const std::string l = in[0].precedence < prec ? "(" + in[0].text + ")" : in[0].text;
              const std::string r = in[1].precedence <= prec ? "(" + in[1].text + ")" : in[1].text;
              return Printed{l + sym + r, prec};
            }
          }
        });
    return p.text;
  }

private:
  enum class Op : unsigned char { Constant, Symbol, Function, Add, Subtract, Multiply, Divide, Negate };

  /** One node of a parsed tree: a constant, a symbol, a function call or an
   *  operator, with its inputs. Never changed once built and held through
   *  `shared_ptr<const Node>`, so copies of an Expression, and the trees
   *  derived from one, share every subtree they have in common. Destroyed
   *  with a work list rather than recursion, like every other walk here. */
  struct Node {
    Op op = Op::Constant;                            ///< what the node is
    double value = 0.0;                              ///< a constant's value
    std::string name;                                ///< a symbol's or function's name
    std::vector<std::shared_ptr<const Node>> inputs; ///< operands or arguments, shared

    /** The constant 0. */
    Node() = default;
    /** Shares the inputs, copies the rest. */
    Node(const Node&) = default;

    // Taken apart with a work list: a node that is the only owner of its
    // inputs moves them out before it dies, so no destructor ever recurses.
    /** Frees the inputs only this node holds, without recursion. */
    ~Node() {
      if (inputs.empty()) return;
      std::vector<std::shared_ptr<const Node>> pending;
      for (auto& in : inputs)
        if (in && in.use_count() == 1) pending.push_back(std::move(in));
      inputs.clear();
      while (!pending.empty()) {
        std::shared_ptr<const Node> n = std::move(pending.back());
        pending.pop_back();
        if (n.use_count() == 1) {
          // Sole owner of a node that was created non-const: taking its
          // inputs is safe, and it is about to be destroyed anyway.
          auto& kids = const_cast<Node&>(*n).inputs;
          for (auto& k : kids)
            if (k && k.use_count() == 1) pending.push_back(std::move(k));
          kids.clear();
        }
      }
    }
  };

  explicit Expression(std::shared_ptr<const Node> node) : node_(std::move(node)) {}

  static const std::shared_ptr<const Node>& zeroNode() {
    static const std::shared_ptr<const Node> zero = std::make_shared<Node>();
    return zero;
  }
  // A moved-from Expression has no node; it reads as 0 rather than crashing.
  const std::shared_ptr<const Node>& root() const { return node_ ? node_ : zeroNode(); }

  static std::shared_ptr<const Node> makeConstant(double v) {
    auto n = std::make_shared<Node>();
    n->value = v;
    return n;
  }

  Expression binary(Op op, const Expression& other) const {
    auto n = std::make_shared<Node>();
    n->op = op;
    n->inputs.push_back(root());
    n->inputs.push_back(other.root());
    return Expression(std::move(n));
  }

  static double failed(std::string& error, const std::string& message) {
    error = message;
    return 0.0;
  }

  static bool arityError(std::string& error, const std::string& name, const char* wanted, int given) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%d", given);
    error = name + " takes " + wanted + ", given " + buffer;
    return false;
  }

  // `n` itself if its new inputs are the ones it had, else a copy of it with
  // them -- so an unchanged subtree stays shared rather than copied.
  static std::shared_ptr<const Node> rebuiltWith(const std::shared_ptr<const Node>& n,
                                                 std::shared_ptr<const Node>* in, size_t count) {
    bool same = true;
    for (size_t i = 0; same && i < count; ++i) same = in[i] == n->inputs[i];
    if (same) return n;
    auto copy = std::make_shared<Node>();
    copy->op = n->op;
    copy->value = n->value;
    copy->name = n->name;
    for (size_t i = 0; i < count; ++i) copy->inputs.push_back(std::move(in[i]));
    return copy;
  }

  // Post-order over the tree with an explicit stack: `combine` gets each node
  // and its inputs' results, and its result goes to the parent.
  template <typename Result, typename Combine>
  static Result foldTree(const std::shared_ptr<const Node>& top, Combine combine) {
    struct Frame {
      const std::shared_ptr<const Node>* node;
      size_t next;
      size_t base;
    };
    std::vector<Frame> frames{Frame{&top, 0, 0}};
    std::vector<Result> results;
    while (!frames.empty()) {
      Frame& f = frames.back();
      const Node& n = **f.node;
      if (f.next < n.inputs.size()) {
        const std::shared_ptr<const Node>* child = &n.inputs[f.next++];
        frames.push_back(Frame{child, 0, results.size()});
        continue;
      }
      Result r = combine(*f.node, results.data() + f.base, results.size() - f.base);
      results.erase(results.begin() + (std::ptrdiff_t) f.base, results.end());
      results.push_back(std::move(r));
      frames.pop_back();
    }
    return std::move(results.back());
  }

  static std::string formatNumber(double value) {
    if (value != value) return "nan";
    if (value == HUGE_VAL) return "inf";
    if (value == -HUGE_VAL) return "-inf";
    char buffer[40];
    for (int digits = 15; digits <= 17; ++digits) {
      char spec[8];
      std::snprintf(spec, sizeof(spec), "%%.%dg", digits);
      numtext::format(buffer, sizeof(buffer), value, spec);
      double back = 0.0;
      const size_t n = std::strlen(buffer);
      if (numtext::parse(buffer, n, &back) == n && back == value) break;
    }
    return buffer;
  }

  // ── The parser: recursive descent, one level per precedence ────────────

  /** One parse() call's state: the text, the limits, the position, the nodes
   *  made so far and the first error, which keeps its column. Recurses only
   *  into parentheses and call arguments, as deep as Limits::maxDepth. Holds
   *  references to the text and the limits, so it lives inside parse() and
   *  nowhere else. */
  struct Parser {
    Parser(const std::string& t, const Limits& l) : text(t), limits(l) {}

    const std::string& text;
    const Limits& limits;
    size_t pos = 0;
    size_t nodes = 0;
    std::string error;

    std::shared_ptr<const Node> fail(const std::string& message) {
      if (error.empty()) {
        char where[40];
        std::snprintf(where, sizeof(where), "column %zu: ", pos + 1);
        error = where + message;
      }
      return nullptr;
    }

    std::string describeHere() const {
      if (pos >= text.size()) return "end of expression";
      const unsigned char c = (unsigned char) text[pos];
      char buffer[24];
      if (c > 0x20 && c < 0x7f) std::snprintf(buffer, sizeof(buffer), "'%c'", (char) c);
      else std::snprintf(buffer, sizeof(buffer), "byte 0x%02X", (unsigned) c);
      return buffer;
    }

    void skipSpace() {
      while (pos < text.size() &&
             (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' || text[pos] == '\r'))
        ++pos;
    }

    std::shared_ptr<Node> newNode(Op op) {
      if (++nodes > limits.maxNodes) {
        fail("the expression has more terms than the limit allows");
        return nullptr;
      }
      auto n = std::make_shared<Node>();
      n->op = op;
      return n;
    }

    static bool isIdentStart(char c) {
      return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    }
    static bool isIdentChar(char c) { return isIdentStart(c) || (c >= '0' && c <= '9'); }
    static bool isDigit(char c) { return c >= '0' && c <= '9'; }

    std::shared_ptr<const Node> parseAdditive(int depth) {
      std::shared_ptr<const Node> lhs = parseMultiplicative(depth);
      for (;;) {
        if (!lhs) return nullptr;
        skipSpace();
        if (pos >= text.size() || (text[pos] != '+' && text[pos] != '-')) return lhs;
        const Op op = text[pos] == '+' ? Op::Add : Op::Subtract;
        ++pos;
        std::shared_ptr<const Node> rhs = parseMultiplicative(depth);
        if (!rhs) return nullptr;
        std::shared_ptr<Node> n = newNode(op);
        if (!n) return nullptr;
        n->inputs.push_back(std::move(lhs));
        n->inputs.push_back(std::move(rhs));
        lhs = std::move(n);
      }
    }

    std::shared_ptr<const Node> parseMultiplicative(int depth) {
      std::shared_ptr<const Node> lhs = parseUnary(depth);
      for (;;) {
        if (!lhs) return nullptr;
        skipSpace();
        if (pos >= text.size() || (text[pos] != '*' && text[pos] != '/')) return lhs;
        const Op op = text[pos] == '*' ? Op::Multiply : Op::Divide;
        ++pos;
        std::shared_ptr<const Node> rhs = parseUnary(depth);
        if (!rhs) return nullptr;
        std::shared_ptr<Node> n = newNode(op);
        if (!n) return nullptr;
        n->inputs.push_back(std::move(lhs));
        n->inputs.push_back(std::move(rhs));
        lhs = std::move(n);
      }
    }

    // Signs are counted in a loop rather than recursed on, so "------1" costs
    // nodes, not stack.
    std::shared_ptr<const Node> parseUnary(int depth) {
      size_t negations = 0;
      for (;;) {
        skipSpace();
        if (pos < text.size() && text[pos] == '-') {
          ++negations;
          ++pos;
        } else if (pos < text.size() && text[pos] == '+') {
          ++pos;
        } else {
          break;
        }
      }
      std::shared_ptr<const Node> operand = parsePrimary(depth);
      for (size_t i = 0; operand && i < negations; ++i) {
        std::shared_ptr<Node> n = newNode(Op::Negate);
        if (!n) return nullptr;
        n->inputs.push_back(std::move(operand));
        operand = std::move(n);
      }
      return operand;
    }

    std::shared_ptr<const Node> parsePrimary(int depth) {
      skipSpace();
      if (pos >= text.size()) return fail("unexpected end of expression");
      const char c = text[pos];
      if (c == '(') {
        if (depth >= limits.maxDepth) return fail("parentheses nested more deeply than the limit allows");
        ++pos;
        std::shared_ptr<const Node> inner = parseAdditive(depth + 1);
        if (!inner) return nullptr;
        skipSpace();
        if (pos >= text.size() || text[pos] != ')') return fail("expected ')'");
        ++pos;
        return inner;
      }
      if (isDigit(c) || c == '.') return parseNumber();
      if (isIdentStart(c)) {
        const size_t from = pos;
        ++pos;
        for (;;) {
          while (pos < text.size() && isIdentChar(text[pos])) ++pos;
          if (pos + 1 < text.size() && text[pos] == '.' && isIdentStart(text[pos + 1])) {
            pos += 2;
            continue;
          }
          break;
        }
        const std::string name = text.substr(from, pos - from);
        skipSpace();
        if (pos >= text.size() || text[pos] != '(') {
          std::shared_ptr<Node> n = newNode(Op::Symbol);
          if (!n) return nullptr;
          n->name = name;
          return n;
        }
        if (depth >= limits.maxDepth) return fail("calls nested more deeply than the limit allows");
        ++pos;
        std::shared_ptr<Node> call = newNode(Op::Function);
        if (!call) return nullptr;
        call->name = name;
        skipSpace();
        if (pos < text.size() && text[pos] == ')') {
          ++pos;
          return call;
        }
        for (;;) {
          std::shared_ptr<const Node> argument = parseAdditive(depth + 1);
          if (!argument) return nullptr;
          call->inputs.push_back(std::move(argument));
          skipSpace();
          if (pos < text.size() && text[pos] == ',') {
            ++pos;
            continue;
          }
          if (pos < text.size() && text[pos] == ')') {
            ++pos;
            return call;
          }
          return fail("expected ',' or ')' in the arguments of " + name);
        }
      }
      return fail("unexpected " + describeHere());
    }

    std::shared_ptr<const Node> parseNumber() {
      const size_t from = pos;
      size_t digits = 0;
      while (pos < text.size() && isDigit(text[pos])) {
        ++pos;
        ++digits;
      }
      if (pos < text.size() && text[pos] == '.') {
        ++pos;
        while (pos < text.size() && isDigit(text[pos])) {
          ++pos;
          ++digits;
        }
      }
      if (digits == 0) {
        pos = from;
        return fail("malformed number");
      }
      if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
        size_t at = pos + 1;
        if (at < text.size() && (text[at] == '+' || text[at] == '-')) ++at;
        if (at >= text.size() || !isDigit(text[at])) {
          pos = from;
          return fail("malformed number: an exponent needs digits");
        }
        while (at < text.size() && isDigit(text[at])) ++at;
        pos = at;
      }
      double value = 0.0;
      if (numtext::parse(text.data() + from, pos - from, &value) != pos - from) {
        pos = from;
        return fail("malformed number");
      }
      if (!std::isfinite(value)) {
        pos = from;
        return fail("number out of range");
      }
      std::shared_ptr<Node> n = newNode(Op::Constant);
      if (!n) return nullptr;
      n->value = value;
      return n;
    }
  };

  std::shared_ptr<const Node> node_;
};

/** Symbols with values: numbers or other expressions.
 *
 *     Expression::VariableScope scope;
 *     scope.set("bpm", 120.0);
 *     scope.set("beat", Expression::parse("60 / bpm"));
 *     Expression::parse("beat * 4").evaluate(scope);   // 2.0
 */
class Expression::VariableScope : public Expression::Scope {
public:
  /** Gives `name` a number, replacing what it had. */
  void set(const std::string& name, double value) { values_[name] = Expression(value); }
  /** Gives `name` an expression, evaluated against this same scope each time
   *  the symbol is used; replaces what it had. */
  void set(const std::string& name, const Expression& value) { values_[name] = value; }
  /** Forgets `name`; an expression using it then fails as unknown. */
  void remove(const std::string& name) { values_.erase(name); }
  /** Forgets every name. */
  void clear() { values_.clear(); }
  /** Whether `name` has a value here. */
  bool has(const std::string& name) const { return values_.find(name) != values_.end(); }

  bool getSymbolValue(const std::string& symbol, Expression& value) const override {
    const auto it = values_.find(symbol);
    if (it == values_.end()) return false;
    value = it->second;
    return true;
  }

private:
  std::map<std::string, Expression> values_;
};

} // namespace sonore
