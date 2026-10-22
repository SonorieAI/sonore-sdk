// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: a small JavaScript interpreter, for scripting and for
// expressions in settings.
//
// JavascriptEngine runs a subset of JavaScript by walking a syntax tree: no
// JIT, no bytecode, no browser. What a plugin wants from it is small -- a
// user's macro, a modulation formula typed into a field, a preset that
// computes a curve, a config value written as `48000 / 4` -- and what a plugin
// cannot afford is for any of that to take the host down with it.
//
// ── The subset ──────────────────────────────────────────────────────────────
//
// var / let / const (block scoping, the temporal dead zone, a fresh binding
// per loop iteration when the body makes a closure), function declarations
// and expressions, arrow functions, closures, default and rest parameters,
// spread in calls and literals, if / for / for-in / for-of / while / do /
// switch, break and continue, try / catch / finally and throw, new with
// constructor functions and prototypes, instanceof, in, delete, typeof, void,
// template literals, optional chaining, ?? and the logical assignments, and
// the whole operator table with ECMAScript's coercions.
//
// Built in: Object, Array, String, Number, Boolean, Math, JSON, the Error
// family, parseInt / parseFloat / isNaN / isFinite, eval (at global scope),
// console. Plus the reference's own additions: Array.prototype.contains and
// remove, Math.sqr / range / randInt / toDegrees / toRadians, and trace().
//
// Not here, deliberately: regular expressions, classes, getters and setters,
// destructuring, generators, async, Symbol, Map and Set, Date, labels, and
// sparse arrays. Each is a meaningful amount of engine for a feature a
// plugin's script does not need, and each would be one more place for a
// hostile script to find an edge.
//
// ── Semantics: ECMAScript's, checked against a real engine ─────────────────
//
// Where the subset says something, it says what ECMAScript says. Strings are
// sequences of UTF-16 code units -- "é".length is 1 and an emoji's is 2 --
// stored as UTF-8 (WTF-8 for a lone surrogate, so one can be split and
// rejoined). Numbers are doubles and print as JavaScript prints them, the
// shortest digits that read back exactly. Property order is ECMAScript's:
// integer keys ascending, then the rest in insertion order. The tests run
// thousands of generated programs here and in node and compare the output.
//
// ── Hostile scripts ─────────────────────────────────────────────────────────
//
// A script may come from a preset somebody downloaded, so every way of making
// the interpreter consume the machine has a limit that ends in an error rather
// than a hang or a crash:
//   * time: maximumExecutionTime, checked on every loop iteration and call --
//     "Execution timed-out" once exceeded; and stop(), from any thread,
//     "Interrupted";
//   * stack: maximumRecursionDepth for script calls, and a measured budget of
//     native stack under every recursive path -- the parser, the evaluator,
//     JSON, string conversion -- never more than the calling thread actually
//     has left, so nesting a thousand brackets is an error, not a stack
//     overflow, on a host thread with 512 KB as on one with 8 MB;
//   * memory: everything the engine allocates is charged to a meter and
//     refunded when it is freed; past maximumMemoryBytes the engine collects
//     its garbage cycles and, if that is not enough, stops the script. Strings
//     and arrays also have a maximum length, checked BEFORE allocating.
// Those three end the script outright: a try/catch inside the script cannot
// catch them, because a limit a script can catch is a limit it can retry.
//
// Nothing escapes to the caller as a C++ exception. Every error -- syntax,
// an uncaught throw, a limit -- comes back in a Result, with a line number.
//
// ── Memory without leaks ────────────────────────────────────────────────────
//
// Values are reference counted (std::shared_ptr), so a JsVar stays valid in
// C++ after the engine that made it is gone. Reference counting alone leaks
// cycles, and closures make cycles constantly -- a function stored in the
// scope it captured is one. So the engine keeps a weak list of what it
// allocated and, now and then, finds the objects whose every reference comes
// from another object on the list and from nothing else (a trial deletion:
// count the references held inside the list, subtract from the use count,
// anything left over is held from outside). What no outside reference can
// reach is garbage, and its references are dropped. A collector that only
// ever empties what is provably unreachable cannot free anything in use.
//
// Destruction is iterative for the same reason the parser has a stack budget:
// an array nested a million deep is a few lines of script, and freeing it by
// recursion would overflow the stack in a destructor.
//
// ── Threads ─────────────────────────────────────────────────────────────────
//
// One engine runs on one thread at a time. stop() is the exception, and is
// safe from any thread. Script values must not be shared across threads while
// a script runs.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

// For the thread's stack bounds -- see stackRoomBelow().
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__) || defined(__linux__)
#include <pthread.h>
#endif

// The interpreter's recursive functions are kept out of each other's frames:
// see the note above Interp::exec.
#if defined(_MSC_VER)
#define SONORE_JS_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define SONORE_JS_NOINLINE __attribute__((noinline))
#else
#define SONORE_JS_NOINLINE
#endif

#include "json.h"
#include "number_text.h"

namespace sonore {

class DynamicObject;
class JavascriptEngine;
class JsVar;

namespace detail {
namespace js {

class Interp;
class ArrayObject;
class FunctionObject;
class JsonWriter;
class Scope;
struct Access;

// The brace characters, named rather than quoted: scripts/unclaimed.mjs and
// namespace-rules.mjs count braces without reading literals, and one quoted
// brace shifts their idea of which namespace the code below it is in.
constexpr char kLBrace = 0x7B;
constexpr char kRBrace = 0x7D;

/** What an engine has allocated and not yet freed, in bytes (an estimate,
 *  but a conservative one). Shared, because a string can outlive its engine
 *  and must still be able to refund what it was charged. */
struct Meter {
  std::atomic<long long> bytes{0};
};

/** An immutable string: UTF-8, or WTF-8 where a lone surrogate must survive. */
struct StringRep {
  std::string text;
  size_t units = 0; // length in UTF-16 code units, which is what .length says
  bool ascii = true;
  std::shared_ptr<Meter> meter;
  long long charged = 0;
  StringRep() = default;
  StringRep(const StringRep&) = delete;
  StringRep& operator=(const StringRep&) = delete;
  ~StringRep() {
    if (meter) meter->bytes -= charged;
  }
};

/**
 * Something that can hold references to other heap objects: an object, an
 * array, a function, or a scope. The collector asks each for the references
 * it holds (gcVisit, once per reference) and, for garbage, has it give them
 * up (gcDrop).
 */
class GcNode {
public:
  GcNode() = default;
  GcNode(const GcNode&) = delete;
  GcNode& operator=(const GcNode&) = delete;
  virtual ~GcNode() = default;
  virtual void gcVisit(std::vector<const GcNode*>& out) const = 0;
  virtual void gcDrop(std::vector<std::shared_ptr<GcNode>>& out) = 0;
  bool gcTracked = false;
};

/** Frees `stack` and everything only it keeps alive, without recursion: a
 *  node about to die hands its children to the stack first, so its own
 *  destructor has nothing left to recurse into. */
inline void releaseIteratively(std::vector<std::shared_ptr<GcNode>>& stack) {
  while (!stack.empty()) {
    std::shared_ptr<GcNode> node = std::move(stack.back());
    stack.pop_back();
    if (node && node.use_count() == 1) node->gcDrop(stack);
  }
}

} // namespace js
} // namespace detail

/**
 * A JavaScript value: undefined, null, a boolean, a number, a string, or a
 * reference to an array, an object or a function.
 *
 * One numeric kind, because ECMAScript has one: 1 and 1.0 are the same value
 * and compare equal. isInt() is a question about the value (integral, and in
 * int range), not a second storage type -- the reference keeps int, int64 and
 * double apart, and then has to decide what `int == double` means.
 *
 * Arrays, objects and functions are references, as in JavaScript: copying a
 * JsVar copies the reference, and `clone()` is the deep copy.
 */
class JsVar {
public:
  /** Which kind of value a JsVar holds. */
  enum class Kind : uint8_t {
    Undefined, ///< `undefined`: a default JsVar, a missing property or argument
    Null,      ///< `null`
    Boolean,   ///< `true` or `false`
    Number,    ///< a double: JavaScript's one number type
    String,    ///< UTF-8 text
    Array,     ///< a reference to an array
    Object,    ///< a reference to a plain object
    Function   ///< a reference to a function, native or script
  };

  /** What a native function receives. `arguments` holds `numArguments`
   *  values; operator[] gives undefined past the end, because JavaScript lets
   *  a caller pass fewer arguments than a function names. */
  struct NativeFunctionArgs {
    /** Made by the engine for each call; the references are valid only
     *  during it. */
    NativeFunctionArgs(const JsVar& self, const JsVar* args, int count) noexcept
        : thisObject(self), arguments(args), numArguments(count) {}
    const JsVar& thisObject; ///< `this` for the call
    const JsVar* arguments;  ///< the arguments passed, `numArguments` of them
    int numArguments;        ///< how many the caller passed, which may be fewer than named
    /** Argument `index` (0-based), or undefined past the end. */
    const JsVar& operator[](int index) const noexcept;
  };

  /** A function implemented in C++. It may throw a std::exception: the
   *  script sees an Error with the exception's message, which it can catch. */
  using NativeFunction = std::function<JsVar(const NativeFunctionArgs&)>;

  /** undefined. */
  JsVar() noexcept {}
  /** null. */
  JsVar(std::nullptr_t) noexcept : kind_(Kind::Null) {}
  /** A boolean. */
  JsVar(bool value) noexcept : kind_(Kind::Boolean), number_(value ? 1.0 : 0.0) {}
  /** A number from any arithmetic type, converted to double: an integer
   *  beyond 2^53 loses its low bits, as it would in JavaScript. */
  template <typename T, typename std::enable_if<std::is_arithmetic<T>::value &&
                                                    !std::is_same<T, bool>::value,
                                                int>::type = 0>
  JsVar(T value) noexcept : kind_(Kind::Number), number_(static_cast<double>(value)) {}
  /** A string, copied. Malformed UTF-8 is replaced with U+FFFD; a null
   *  pointer gives "". */
  JsVar(const char* text);
  /** A string, copied; malformed UTF-8 is replaced with U+FFFD. */
  JsVar(const std::string& text);
  /** An object, array or function -- whichever `object` is. Null gives null. */
  JsVar(std::shared_ptr<DynamicObject> object) noexcept;
  /** A native function, from anything callable with NativeFunctionArgs. */
  template <typename F,
            typename std::enable_if<
                !std::is_same<typename std::decay<F>::type, JsVar>::value &&
                    !std::is_arithmetic<typename std::decay<F>::type>::value &&
                    !std::is_convertible<F, const char*>::value &&
                    !std::is_convertible<F, std::shared_ptr<DynamicObject>>::value &&
                    std::is_convertible<decltype(std::declval<F&>()(
                                            std::declval<const NativeFunctionArgs&>())),
                                        JsVar>::value,
                int>::type = 0>
  JsVar(F&& function) : JsVar(NativeFunction(std::forward<F>(function)), 0) {}

  /** undefined, by name. */
  static JsVar undefined() noexcept { return JsVar(); }
  /** null, by name. */
  static JsVar null() noexcept { return JsVar(nullptr); }
  /** A new array holding `elements`. */
  static JsVar array(std::vector<JsVar> elements = {});
  /** A new, empty plain object. */
  static JsVar object();
  /** A new native function; see NativeFunction. */
  static JsVar function(NativeFunction function) { return JsVar(std::move(function), 0); }

  /** Which kind of value this is. */
  Kind kind() const noexcept { return kind_; }
  /** True for undefined. */
  bool isUndefined() const noexcept { return kind_ == Kind::Undefined; }
  /** True for null, not for undefined: isVoid() asks for either. */
  bool isNull() const noexcept { return kind_ == Kind::Null; }
  /** Undefined or null: the two values that have no properties at all. */
  bool isVoid() const noexcept { return kind_ == Kind::Undefined || kind_ == Kind::Null; }
  /** True for a boolean. */
  bool isBool() const noexcept { return kind_ == Kind::Boolean; }
  /** True for any number, NaN and the infinities included. */
  bool isNumber() const noexcept { return kind_ == Kind::Number; }
  /** A number that is integral and within int's range. */
  bool isInt() const noexcept {
    return kind_ == Kind::Number && number_ >= -2147483648.0 && number_ <= 2147483647.0 &&
           number_ == std::floor(number_);
  }
  /** A number that is not isInt(): fractional, out of int's range, NaN or
   *  infinite. */
  bool isDouble() const noexcept { return kind_ == Kind::Number && !isInt(); }
  /** True for a string. */
  bool isString() const noexcept { return kind_ == Kind::String; }
  /** True for an array. */
  bool isArray() const noexcept { return kind_ == Kind::Array; }
  /** A plain object -- not an array, not a function, though both are objects
   *  to JavaScript's typeof. */
  bool isObject() const noexcept { return kind_ == Kind::Object; }
  /** True for a function, native or script. */
  bool isFunction() const noexcept { return kind_ == Kind::Function; }
  /** A function implemented in C++, rather than in script. */
  bool isMethod() const noexcept;

  /** JavaScript's truthiness: false, 0, NaN, "", null and undefined are false. */
  bool toBool() const noexcept;
  /** JavaScript's ToNumber, except that an object's own valueOf is not called
   *  (there is no engine to call it with): objects give NaN, arrays what
   *  their string form gives. */
  double toDouble() const;
  /** toDouble(), truncated toward zero and saturated to int's range; NaN is 0. */
  int toInt() const;
  /** toDouble(), truncated toward zero and saturated at +-9e18; NaN is 0. */
  int64_t toInt64() const;
  /** JavaScript's ToString: numbers as JavaScript prints them, arrays joined
   *  with commas, objects as "[object Object]". A string comes back whole;
   *  an array's text is cut at 8 MB and ends in "..." -- an array shared
   *  many times over would otherwise be written out every time it is met. */
  std::string toString() const;
  /** What typeof says: "undefined", "object" (null too), "boolean", "number",
   *  "string" or "function". */
  const char* typeName() const noexcept;

  // ── Arrays ───────────────────────────────────────────────────────────────

  /** An array's length; 0 for anything else. */
  int size() const noexcept;
  /** An array element, or undefined. */
  JsVar operator[](int index) const;
  /** The array's elements, or null if this is not an array. Writing through
   *  it is fine; the engine re-measures an array when a script next grows it. */
  std::vector<JsVar>* getArray() const noexcept;
  /** Appends to an array; false if this is not one. */
  bool append(const JsVar& value);

  // ── Objects ──────────────────────────────────────────────────────────────

  /** The object behind an object, array or function; null otherwise. */
  DynamicObject* getDynamicObject() const noexcept;
  /** The same, sharing ownership: the object stays alive while the pointer
   *  does. */
  std::shared_ptr<DynamicObject> getObject() const noexcept;
  /** An own property of an object, array or function, or undefined. */
  JsVar operator[](const char* name) const;
  /** The same, by std::string. */
  JsVar operator[](const std::string& name) const;
  /** Whether an object, array or function has this own property; for an
   *  array, "length" and an index below the length count too. */
  bool hasProperty(const std::string& name) const;
  /** An own property, or `fallback` when there is none (or this is not an
   *  object); an array answers "length" and its indices. */
  JsVar getProperty(const std::string& name, const JsVar& fallback = JsVar()) const;

  /** Strict equality, ===: same kind and value, or the same object. NaN is
   *  not equal to itself, and 0 equals -0. */
  bool operator==(const JsVar& other) const noexcept;
  /** Strict inequality, !==. */
  bool operator!=(const JsVar& other) const noexcept { return !(*this == other); }
  /** Loose equality, ==, for primitives; an object is converted as toString()
   *  converts it, without calling script. */
  bool looselyEquals(const JsVar& other) const;

  /** A deep copy of arrays and objects (shared structure and cycles are kept
   *  as they were); functions are shared, not copied. */
  JsVar clone() const;

  // ── JSON ─────────────────────────────────────────────────────────────────

  /** The value as a JsonValue: undefined and functions become null, as do
   *  non-finite numbers, and a cycle is cut with null -- as is everything
   *  past about 8 MB of content. A JsonValue keeps its keys sorted, so the
   *  property order is not carried over. */
  JsonValue toJson() const;
  /** A new value from a JsonValue: objects and arrays become new ones.
   *  Properties come in the JsonValue's sorted key order. Nesting past 256
   *  levels becomes null. */
  static JsVar fromJson(const JsonValue& json);
  /** JSON text in property order, as JSON.stringify writes it. A value that
   *  JSON cannot hold (undefined, a function), or whose text would pass
   *  8 MB, gives an empty string. */
  std::string toJsonString(int indent = 0) const;
  /** Parses JSON text, keeping the property order; undefined with `error`
   *  set when it is not valid JSON. */
  static JsVar parseJson(const std::string& text, std::string* error = nullptr);

private:
  friend struct detail::js::Access;
  JsVar(NativeFunction function, int tag);

  Kind kind_ = Kind::Undefined;
  double number_ = 0.0;
  std::shared_ptr<void> ref_;
};

/**
 * An object: named properties in order, an optional prototype, and methods.
 *
 * What a script sees as an object, and what a plugin builds to give a script
 * something to call -- registerNativeObject() puts one in the global scope,
 * setMethod() gives it functions implemented in C++.
 *
 * The public interface is about OWN properties; a script's lookups also walk
 * the prototype chain, which the engine does. Property names are UTF-8.
 */
class DynamicObject : public detail::js::GcNode {
public:
  /** How objects are held: shared, as JsVar holds them. */
  using Ptr = std::shared_ptr<DynamicObject>;

  /** An object with no properties. Make it with std::make_shared: a JsVar
   *  can hold it only through a shared_ptr. */
  DynamicObject() = default;
  /** Releases the properties without recursing, however deep the values
   *  they hold. */
  ~DynamicObject() override;

  /** Whether this object has an own property of that name (methods
   *  included); the prototype is not asked. */
  bool hasProperty(const std::string& name) const;
  /** The property's value, or undefined. */
  JsVar getProperty(const std::string& name) const;
  /** Sets an own property, replacing its value if it exists (and keeping
   *  its place in the order); a new one is enumerable and goes last. */
  void setProperty(const std::string& name, const JsVar& value);
  /** True if there was such a property. */
  bool removeProperty(const std::string& name);
  /** True if the property holds a function (native or script). */
  bool hasMethod(const std::string& name) const;
  /** A native method, not enumerated by for-in -- as a class's methods are not. */
  void setMethod(const std::string& name, JsVar::NativeFunction function);
  /** Calls a NATIVE method with `thisObject` as `this`. A script function
   *  needs an engine to run in: JavascriptEngine::callFunctionObject. */
  JsVar invokeMethod(const JsVar& thisObject, const std::string& name,
                     const std::vector<JsVar>& args) const;
  /** Removes every own property, methods included. The prototype stays. */
  void clear();
  /** How many own properties, methods included. */
  size_t getNumProperties() const noexcept { return props_.size(); }
  /** Own property names in JavaScript's order: integer keys ascending, then
   *  the others as they were added. */
  std::vector<std::string> getPropertyNames() const;
  /** A deep copy: properties are cloned as JsVar::clone() clones them. */
  Ptr clone() const;

  /** For the engine's cycle collector, not for plugin code: lists what this
   *  object keeps alive. */
  void gcVisit(std::vector<const detail::js::GcNode*>& out) const override;
  /** For the engine's cycle collector, not for plugin code: hands over what
   *  this object holds so it can be freed without recursion. */
  void gcDrop(std::vector<std::shared_ptr<detail::js::GcNode>>& out) override;

protected:
  /** Extra bytes a subclass holds, for the memory meter. */
  virtual long long extraBytes() const { return 0; }
  /** Re-measures this object against its engine's meter, if it has one. */
  void recharge();

private:
  friend class JsVar;
  friend struct detail::js::Access;
  friend class detail::js::Interp;
  friend class detail::js::ArrayObject;
  friend class detail::js::FunctionObject;
  friend class detail::js::JsonWriter;

  enum : uint8_t { kEnumerable = 1, kReadOnly = 2 };
  /** One own property: its key, its value, and whether for-in sees it and
   *  a script may assign it (kEnumerable, kReadOnly). Held in the order
   *  added; getPropertyNames() puts integer keys first, as JavaScript does. */
  struct Property {
    std::string key;             ///< the name, UTF-8
    JsVar value;                 ///< the value
    uint8_t flags = kEnumerable; ///< kEnumerable and kReadOnly, ORed
  };

  int findIndex(const std::string& name) const;
  Property* find(const std::string& name);
  const Property* find(const std::string& name) const;
  /** Defines or overwrites a property, flags included. */
  void put(const std::string& name, const JsVar& value, uint8_t flags);
  void rebuildIndex();
  std::vector<std::string> keysInOrder(bool enumerableOnly) const;

  std::vector<Property> props_;
  // Built once an object has more than a handful of properties; below that a
  // linear scan is faster than hashing.
  std::unique_ptr<std::unordered_map<std::string, uint32_t>> index_;
  std::shared_ptr<DynamicObject> proto_;
  bool explicitProto_ = false; // false: the default for the kind (Object, Array, Function)
  JsVar::Kind cellKind_ = JsVar::Kind::Object;
  std::shared_ptr<detail::js::Meter> meter_;
  long long charged_ = 0;
  long long keyBytes_ = 0;
};

namespace detail {
namespace js {

// ── Access to JsVar's insides, for the engine ──────────────────────────────

struct Access {
  static JsVar::Kind kind(const JsVar& v) noexcept { return v.kind_; }
  static double num(const JsVar& v) noexcept { return v.number_; }
  static const StringRep* str(const JsVar& v) noexcept {
    return v.kind_ == JsVar::Kind::String ? static_cast<const StringRep*>(v.ref_.get()) : nullptr;
  }
  static DynamicObject* obj(const JsVar& v) noexcept {
    return (v.kind_ >= JsVar::Kind::Array) ? static_cast<DynamicObject*>(v.ref_.get()) : nullptr;
  }
  static std::shared_ptr<DynamicObject> objPtr(const JsVar& v) noexcept {
    if (v.kind_ < JsVar::Kind::Array) return nullptr;
    return std::static_pointer_cast<DynamicObject>(v.ref_);
  }
  static JsVar fromRep(std::shared_ptr<StringRep> rep) noexcept {
    JsVar v;
    v.kind_ = JsVar::Kind::String;
    v.ref_ = std::shared_ptr<void>(std::move(rep));
    return v;
  }
  static JsVar fromObject(const std::shared_ptr<DynamicObject>& o) noexcept {
    JsVar v;
    if (!o) {
      v.kind_ = JsVar::Kind::Null;
      return v;
    }
    v.kind_ = o->cellKind_;
    v.ref_ = std::static_pointer_cast<void>(o);
    return v;
  }
  static void dropRef(JsVar& v, std::vector<std::shared_ptr<GcNode>>& out) {
    if (v.kind_ >= JsVar::Kind::Array && v.ref_) {
      std::shared_ptr<DynamicObject> o = std::static_pointer_cast<DynamicObject>(v.ref_);
      v.ref_.reset();
      out.push_back(std::move(o));
    } else {
      v.ref_.reset();
    }
    v.kind_ = JsVar::Kind::Undefined;
  }
  static void visit(const JsVar& v, std::vector<const GcNode*>& out) {
    if (v.kind_ >= JsVar::Kind::Array && v.ref_)
      out.push_back(static_cast<const DynamicObject*>(v.ref_.get()));
  }
  static JsVar cloneValue(const JsVar& root, const DynamicObject* seedSource = nullptr,
                          const std::shared_ptr<DynamicObject>& seedCopy = nullptr);
  static JsonValue toJsonValue(const JsVar& v, std::vector<const DynamicObject*>& stack, size_t& budget);
};

// ── UTF-8 / UTF-16 ──────────────────────────────────────────────────────────
//
// JavaScript strings are UTF-16 code units. They are stored here as UTF-8, so
// that a plugin reads them as the strings it already has; where a lone
// surrogate has to survive (a script split a pair with substring), it is
// stored as its three-byte form, which is WTF-8. Every string the engine
// makes is CANONICAL in that sense -- a pair is always four bytes -- so equal
// strings are equal bytes, and comparing two of them never needs decoding.

inline void appendCodePoint(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out += (char) cp;
  } else if (cp < 0x800) {
    out += (char) (0xC0 | (cp >> 6));
    out += (char) (0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += (char) (0xE0 | (cp >> 12));
    out += (char) (0x80 | ((cp >> 6) & 0x3F));
    out += (char) (0x80 | (cp & 0x3F));
  } else {
    out += (char) (0xF0 | (cp >> 18));
    out += (char) (0x80 | ((cp >> 12) & 0x3F));
    out += (char) (0x80 | ((cp >> 6) & 0x3F));
    out += (char) (0x80 | (cp & 0x3F));
  }
}

/** Decodes one code point at `i` (advancing it). Malformed bytes decode as
 *  U+FFFD, one byte at a time; a three-byte surrogate decodes as itself. */
inline uint32_t decodeCodePoint(const unsigned char* s, size_t n, size_t& i) {
  const unsigned char c = s[i];
  if (c < 0x80) {
    ++i;
    return c;
  }
  auto cont = [&](size_t k) { return i + k < n && (s[i + k] & 0xC0) == 0x80; };
  if (c >= 0xC2 && c <= 0xDF && cont(1)) {
    const uint32_t cp = ((uint32_t) (c & 0x1F) << 6) | (s[i + 1] & 0x3F);
    i += 2;
    return cp;
  }
  if (c >= 0xE0 && c <= 0xEF && cont(1) && cont(2)) {
    const uint32_t cp = ((uint32_t) (c & 0x0F) << 12) | ((uint32_t) (s[i + 1] & 0x3F) << 6) |
                        (s[i + 2] & 0x3F);
    if (cp >= 0x800) {
      i += 3;
      return cp;
    }
  }
  if (c >= 0xF0 && c <= 0xF4 && cont(1) && cont(2) && cont(3)) {
    const uint32_t cp = ((uint32_t) (c & 0x07) << 18) | ((uint32_t) (s[i + 1] & 0x3F) << 12) |
                        ((uint32_t) (s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F);
    if (cp >= 0x10000 && cp <= 0x10FFFF) {
      i += 4;
      return cp;
    }
  }
  ++i;
  return 0xFFFD;
}

inline void toUtf16(const std::string& text, std::u16string& out) {
  out.clear();
  out.reserve(text.size());
  const unsigned char* s = (const unsigned char*) text.data();
  const size_t n = text.size();
  size_t i = 0;
  while (i < n) {
    const uint32_t cp = decodeCodePoint(s, n, i);
    if (cp >= 0x10000) {
      out += (char16_t) (0xD800 + ((cp - 0x10000) >> 10));
      out += (char16_t) (0xDC00 + ((cp - 0x10000) & 0x3FF));
    } else {
      out += (char16_t) cp;
    }
  }
}

inline std::string fromUtf16(const char16_t* units, size_t n) {
  std::string out;
  out.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    uint32_t u = units[i];
    if (u >= 0xD800 && u <= 0xDBFF && i + 1 < n && units[i + 1] >= 0xDC00 &&
        units[i + 1] <= 0xDFFF) {
      u = 0x10000 + ((u - 0xD800) << 10) + (units[i + 1] - 0xDC00u);
      ++i;
    }
    appendCodePoint(out, u);
  }
  return out;
}

inline std::string fromUtf16(const std::u16string& s) { return fromUtf16(s.data(), s.size()); }

/** Length in UTF-16 units of canonical text: every lead byte is a unit, and a
 *  four-byte sequence is two. */
inline size_t utf16Units(const std::string& text, bool* asciiOut) {
  size_t units = 0;
  bool ascii = true;
  for (unsigned char c : text) {
    if (c >= 0x80) ascii = false;
    if ((c & 0xC0) != 0x80) ++units;
    if (c >= 0xF0) ++units;
  }
  if (asciiOut) *asciiOut = ascii;
  return units;
}

/** Text in canonical form: valid UTF-8, with surrogates allowed alone but
 *  never as a pair of three-byte halves, and malformed bytes as U+FFFD. */
inline std::string canonicalText(const std::string& text) {
  bool ascii = true;
  for (unsigned char c : text)
    if (c >= 0x80) {
      ascii = false;
      break;
    }
  if (ascii) return text;
  std::u16string units;
  toUtf16(text, units);
  return fromUtf16(units);
}

/** A string from text known to be canonical. */
inline std::shared_ptr<StringRep> makeRep(std::string text, const std::shared_ptr<Meter>& meter) {
  auto rep = std::make_shared<StringRep>();
  rep->text = std::move(text);
  rep->units = utf16Units(rep->text, &rep->ascii);
  if (meter) {
    rep->meter = meter;
    rep->charged = (long long) (sizeof(StringRep) + rep->text.capacity() + 48);
    meter->bytes += rep->charged;
  }
  return rep;
}

/** Joins two canonical strings into canonical text: a high surrogate at the
 *  end of one and a low one at the start of the other become one pair. */
inline std::string concatText(const std::string& a, const std::string& b) {
  const size_t na = a.size();
  if (na >= 3 && b.size() >= 3 && (unsigned char) a[na - 3] == 0xED &&
      ((unsigned char) a[na - 2] & 0xF0) == 0xA0 && (unsigned char) b[0] == 0xED &&
      ((unsigned char) b[1] & 0xF0) == 0xB0) {
    const uint32_t hi = 0xD000u | (((unsigned char) a[na - 2] & 0x3Fu) << 6) |
                        ((unsigned char) a[na - 1] & 0x3Fu);
    const uint32_t lo = 0xD000u | (((unsigned char) b[1] & 0x3Fu) << 6) |
                        ((unsigned char) b[2] & 0x3Fu);
    std::string out;
    out.reserve(na + b.size() - 2);
    out.append(a, 0, na - 3);
    appendCodePoint(out, 0x10000u + ((hi - 0xD800u) << 10) + (lo - 0xDC00u));
    out.append(b, 3, std::string::npos);
    return out;
  }
  std::string out;
  out.reserve(na + b.size());
  out += a;
  out += b;
  return out;
}

/** JavaScript's white space and line terminators, for trim() and ToNumber. */
inline bool isJsSpace(uint32_t cp) {
  switch (cp) {
    case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x20: case 0xA0:
    case 0x1680: case 0x2028: case 0x2029: case 0x202F: case 0x205F: case 0x3000:
    case 0xFEFF:
      return true;
    default:
      return cp >= 0x2000 && cp <= 0x200A;
  }
}

/** Byte offsets of `text` with JavaScript white space removed from either end. */
inline void trimBounds(const std::string& text, bool start, bool end, size_t& from, size_t& to) {
  const unsigned char* s = (const unsigned char*) text.data();
  const size_t n = text.size();
  from = 0;
  to = n;
  if (start) {
    while (from < n) {
      size_t i = from;
      const uint32_t cp = decodeCodePoint(s, n, i);
      if (!isJsSpace(cp)) break;
      from = i;
    }
  }
  if (end) {
    while (to > from) {
      size_t lead = to - 1;
      while (lead > from && (s[lead] & 0xC0) == 0x80) --lead;
      size_t i = lead;
      const uint32_t cp = decodeCodePoint(s, n, i);
      if (i != to || !isJsSpace(cp)) break;
      to = lead;
    }
  }
}

// ── Numbers ─────────────────────────────────────────────────────────────────

inline bool isIndexText(const std::string& s, uint32_t* out) {
  // A canonical array index: "0", or digits without a leading zero, below 2^32-1.
  if (s.empty() || s.size() > 10) return false;
  if (s.size() > 1 && s[0] == '0') return false;
  uint64_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + (uint64_t) (c - '0');
  }
  if (v >= 4294967295ull) return false;
  if (out) *out = (uint32_t) v;
  return true;
}

inline double toInteger(double v) {
  if (v != v) return 0.0;
  if (std::isinf(v)) return v;
  return std::trunc(v);
}

inline uint32_t toUint32(double v) {
  if (!std::isfinite(v)) return 0;
  double t = std::fmod(std::trunc(v), 4294967296.0);
  if (t < 0) t += 4294967296.0;
  return (uint32_t) t;
}

inline int32_t toInt32(double v) { return (int32_t) toUint32(v); }

/** Saturating double to int64, for indices and lengths: NaN is 0. */
inline long long clampToLong(double v) {
  if (v != v) return 0;
  if (v >= 9.0e18) return 9000000000000000000ll;
  if (v <= -9.0e18) return -9000000000000000000ll;
  return (long long) v;
}

/** The exact decimal expansion of |v| (finite, non-zero): digits without
 *  leading or trailing zeros, and the exponent of the first digit. printf is
 *  exact at any precision on every C library this SDK supports, and a double
 *  has at most 767 significant decimal digits. */
inline void exactDigits(double v, std::string& digits, int& exponent) {
  std::vector<char> buffer(1200);
  numtext::format(buffer.data(), buffer.size(), std::fabs(v), "%.1100e");
  digits.clear();
  const char* p = buffer.data();
  while (*p && *p != 'e') {
    if (*p >= '0' && *p <= '9') digits += *p;
    ++p;
  }
  exponent = *p == 'e' ? std::atoi(p + 1) : 0;
  while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
}

/** The shortest digits that read back as `v` (finite, non-zero), and the
 *  exponent of the first: the k and n of ECMAScript's Number::toString. */
inline bool readsBackAs(const std::string& digits, int exponent, double a) {
  std::string text = digits.substr(0, 1) + "." + digits.substr(1) + "e" + std::to_string(exponent);
  double back = 0.0;
  numtext::parse(text.data(), text.size(), &back);
  return back == a;
}

inline void shortestDigits(double v, std::string& digits, int& exponent) {
  char buffer[64];
  char format[16];
  const double a = std::fabs(v);
  for (int precision = 0; precision <= 16; ++precision) {
    std::snprintf(format, sizeof(format), "%%.%de", precision);
    numtext::format(buffer, sizeof(buffer), a, format);
    std::string d;
    const char* p = buffer;
    while (*p && *p != 'e') {
      if (*p >= '0' && *p <= '9') d += *p;
      ++p;
    }
    int e = *p == 'e' ? std::atoi(p + 1) : 0;
    bool found = readsBackAs(d, e, a);
    if (!found) {
      // printf's digits are the CLOSEST at this length, and still may not read
      // back: at a power of two the gap below a double is half the gap above,
      // and a tie rounds to even -- 2^-24 prints as ...062e-8 at sixteen
      // digits, which is outside its interval, while ...063e-8 is inside. So
      // the neighbours one unit either side are tried too.
      std::string up = d;
      int i = (int) up.size() - 1;
      while (i >= 0 && up[(size_t) i] == '9') up[(size_t) i--] = '0';
      int eu = e;
      if (i < 0) {
        up.insert(up.begin(), '1');
        up.pop_back();
        ++eu;
      } else {
        ++up[(size_t) i];
      }
      std::string down = d;
      int j = (int) down.size() - 1;
      while (j >= 0 && down[(size_t) j] == '0') down[(size_t) j--] = '9';
      const bool downOk = j > 0 || (j == 0 && down[0] > '1');
      if (downOk) --down[(size_t) j];
      if (readsBackAs(up, eu, a)) {
        d = up;
        e = eu;
        found = true;
      } else if (downOk && readsBackAs(down, e, a)) {
        d = down;
        found = true;
      }
    }
    if (found) {
      while (d.size() > 1 && d.back() == '0') d.pop_back();
      digits = d;
      exponent = e;
      return;
    }
  }
  numtext::format(buffer, sizeof(buffer), a, "%.16e"); // seventeen digits always read back
  digits.clear();
  const char* p = buffer;
  while (*p && *p != 'e') {
    if (*p >= '0' && *p <= '9') digits += *p;
    ++p;
  }
  exponent = *p == 'e' ? std::atoi(p + 1) : 0;
  while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
}

/** Number::toString(10), ECMAScript's algorithm exactly. */
inline std::string numberToString(double v) {
  if (v != v) return "NaN";
  if (v == 0) return "0";
  if (std::isinf(v)) return v < 0 ? "-Infinity" : "Infinity";
  std::string out;
  if (v < 0) out += '-';
  const double a = std::fabs(v);
  if (a < 9007199254740992.0 && a == std::floor(a)) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%lld", (long long) a);
    return out + buffer;
  }
  std::string d;
  int e = 0;
  shortestDigits(a, d, e);
  const int k = (int) d.size();
  const int n = e + 1;
  if (k <= n && n <= 21) {
    out += d;
    out.append((size_t) (n - k), '0');
  } else if (0 < n && n <= 21) {
    out.append(d, 0, (size_t) n);
    out += '.';
    out.append(d, (size_t) n, std::string::npos);
  } else if (-6 < n && n <= 0) {
    out += "0.";
    out.append((size_t) -n, '0');
    out += d;
  } else {
    out += d[0];
    if (k > 1) {
      out += '.';
      out.append(d, 1, std::string::npos);
    }
    out += 'e';
    out += (n - 1 >= 0) ? '+' : '-';
    out += std::to_string(n - 1 >= 0 ? n - 1 : 1 - n);
  }
  return out;
}

/** Rounds the decimal `digits` (value 0.d1d2... scaled) to `keep` digits,
 *  half away from zero -- which is what toFixed and toPrecision ask for, on
 *  the EXACT value. Returns true if the rounding carried into a new digit. */
inline bool roundDigits(std::string& digits, int keep) {
  if (keep < 0) {
    digits.clear();
    return false;
  }
  if ((int) digits.size() <= keep) {
    digits.append((size_t) keep - digits.size(), '0');
    return false;
  }
  const bool up = digits[(size_t) keep] >= '5';
  digits.resize((size_t) keep);
  if (!up) return false;
  for (int i = keep - 1; i >= 0; --i) {
    if (digits[(size_t) i] == '9') {
      digits[(size_t) i] = '0';
    } else {
      ++digits[(size_t) i];
      return false;
    }
  }
  digits.insert(digits.begin(), '1');
  return true;
}

inline std::string numberToFixed(double v, int fraction) {
  if (v != v) return "NaN";
  if (std::fabs(v) >= 1e21 || std::isinf(v)) return numberToString(v);
  // (-0).toFixed(2) is "0.00", but (-0.0001).toFixed(2) is "-0.00": the sign
  // is the value's, decided before rounding.
  std::string out = v < 0 ? "-" : "";
  // N = round(|v| * 10^fraction), half up, on the EXACT value -- 2.5.toFixed(0)
  // is "3", and 1.005.toFixed(2) is "1.00" because 1.005 is really 1.00499...
  std::string r;
  if (v != 0) {
    std::string d;
    int e = 0;
    exactDigits(v, d, e);
    const int keep = e + 1 + fraction; // the digits at or above 10^-fraction
    if (keep >= 0) {
      r = d;
      roundDigits(r, keep);
    }
  }
  if (r.empty()) r = "0";
  if ((int) r.size() < fraction + 1) r.insert(0, (size_t) (fraction + 1) - r.size(), '0');
  out.append(r, 0, r.size() - (size_t) fraction);
  if (fraction > 0) {
    out += '.';
    out.append(r, r.size() - (size_t) fraction, std::string::npos);
  }
  return out;
}

inline std::string numberToExponential(double v, int fraction, bool auto_) {
  if (v != v) return "NaN";
  if (std::isinf(v)) return v < 0 ? "-Infinity" : "Infinity";
  std::string out;
  if (v < 0) out += '-';
  std::string d;
  int e = 0;
  if (v == 0) {
    d = std::string((size_t) (auto_ ? 1 : fraction + 1), '0');
    e = 0;
  } else if (auto_) {
    shortestDigits(v, d, e);
  } else {
    exactDigits(v, d, e);
    if (roundDigits(d, fraction + 1)) {
      d.pop_back();
      ++e;
    }
  }
  out += d[0];
  if (d.size() > 1) {
    out += '.';
    out.append(d, 1, std::string::npos);
  }
  out += 'e';
  out += e >= 0 ? '+' : '-';
  out += std::to_string(e >= 0 ? e : -e);
  return out;
}

inline std::string numberToPrecision(double v, int precision) {
  if (v != v) return "NaN";
  if (std::isinf(v)) return v < 0 ? "-Infinity" : "Infinity";
  std::string out;
  if (v < 0) out += '-';
  std::string d;
  int e = 0;
  if (v == 0) {
    d = std::string((size_t) precision, '0');
  } else {
    exactDigits(v, d, e);
    if (roundDigits(d, precision)) {
      d.pop_back();
      ++e;
    }
  }
  if (e < -6 || e >= precision) {
    out += d[0];
    if (precision > 1) {
      out += '.';
      out.append(d, 1, std::string::npos);
    }
    out += 'e';
    out += e >= 0 ? '+' : '-';
    out += std::to_string(e >= 0 ? e : -e);
    return out;
  }
  if (e >= 0) {
    out.append(d, 0, (size_t) e + 1);
    if (precision > e + 1) {
      out += '.';
      out.append(d, (size_t) e + 1, std::string::npos);
    }
  } else {
    out += "0.";
    out.append((size_t) (-(e + 1)), '0');
    out += d;
  }
  return out;
}

/** Number::toString(radix). Integers are exact; a fraction is written to the
 *  precision the double carries, which may differ from other engines in the
 *  last digit. */
inline std::string numberToRadix(double v, int radix) {
  if (radix == 10 || v != v || std::isinf(v) || v == 0) return numberToString(v);
  static const char* kDigits = "0123456789abcdefghijklmnopqrstuvwxyz";
  std::string out;
  const bool negative = v < 0;
  double a = std::fabs(v);
  double ip = std::floor(a);
  double fp = a - ip;
  std::string ints;
  if (ip == 0) ints = "0";
  int guard = 0;
  while (ip > 0 && guard++ < 1100) {
    const double r = std::fmod(ip, (double) radix);
    ints += kDigits[(int) r];
    ip = std::floor((ip - r) / radix);
  }
  std::reverse(ints.begin(), ints.end());
  out = negative ? "-" + ints : ints;
  if (fp > 0) {
    // As many digits as the double's own resolution justifies: stop when the
    // remaining fraction is below half an ulp of the value.
    double ulp = std::nextafter(a, std::numeric_limits<double>::infinity()) - a;
    double delta = (std::max)(ulp * 0.5, std::numeric_limits<double>::denorm_min());
    out += '.';
    int count = 0;
    while (fp >= delta && count < 1100) {
      fp *= radix;
      delta *= radix;
      int digit = (int) fp;
      if (digit >= radix) digit = radix - 1;
      fp -= digit;
      out += kDigits[digit];
      ++count;
    }
    if (out.back() == '.') out.pop_back();
  }
  return out;
}

/** The decimal literal grammar, checked before strtod sees the text: strtod
 *  also accepts "inf", "nan", hex floats and a bare "1e", none of which
 *  JavaScript reads as a number. Returns the length matched (0 for none);
 *  `whole` asks for the entire text. */
inline size_t matchDecimal(const char* s, size_t n) {
  size_t i = 0;
  if (i < n && (s[i] == '+' || s[i] == '-')) ++i;
  size_t digits = 0;
  while (i < n && s[i] >= '0' && s[i] <= '9') {
    ++i;
    ++digits;
  }
  if (i < n && s[i] == '.') {
    size_t j = i + 1;
    size_t frac = 0;
    while (j < n && s[j] >= '0' && s[j] <= '9') {
      ++j;
      ++frac;
    }
    if (digits + frac > 0) {
      i = j;
      digits += frac;
    }
  }
  if (digits == 0) return 0;
  if (i < n && (s[i] == 'e' || s[i] == 'E')) {
    size_t j = i + 1;
    if (j < n && (s[j] == '+' || s[j] == '-')) ++j;
    size_t exp = 0;
    while (j < n && s[j] >= '0' && s[j] <= '9') {
      ++j;
      ++exp;
    }
    if (exp > 0) i = j;
  }
  return i;
}

inline double parseDecimal(const char* s, size_t n) {
  double v = 0.0;
  // numtext copies only what a number can contain, and parses with '.' as
  // the point whatever the host's locale says.
  if (numtext::parse(s, n, &v) == 0) return std::numeric_limits<double>::quiet_NaN();
  return v;
}

inline int digitValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'z') return c - 'a' + 10;
  if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
  return 99;
}

/** Digits in `radix` as a double. For a power-of-two radix the result is
 *  correctly rounded, as ECMAScript requires of parseInt and of 0x / 0o / 0b
 *  literals: the first 62 or so bits are kept exactly, the rest folded into a
 *  sticky bit, and the rounding to 53 bits done once. Adding digit by digit
 *  in a double would round at every step past 2^53. Other radixes may be
 *  approximated (the language allows it); they are accumulated exactly in 64
 *  bits and folded in a chunk at a time, which rounds once per chunk. */
inline double digitsToDouble(const char* s, size_t n, int radix) {
  int bitsPerDigit = 0;
  for (int r = radix; r > 1 && !(r & 1); r >>= 1) ++bitsPerDigit;
  if ((1 << bitsPerDigit) == radix) {
    uint64_t m = 0;
    int exponent = 0;
    bool sticky = false;
    for (size_t i = 0; i < n; ++i) {
      const uint64_t d = (uint64_t) digitValue(s[i]);
      if ((m >> (62 - bitsPerDigit)) == 0) {
        m = (m << bitsPerDigit) | d;
      } else {
        exponent += bitsPerDigit;
        if (d) sticky = true;
      }
    }
    int length = 0;
    for (uint64_t t = m; t; t >>= 1) ++length;
    if (length > 53) {
      const int shift = length - 53;
      const uint64_t rest = m & ((1ull << shift) - 1);
      const uint64_t half = 1ull << (shift - 1);
      m >>= shift;
      exponent += shift;
      if (rest > half || (rest == half && (sticky || (m & 1)))) ++m;
    }
    return std::ldexp((double) m, exponent);
  }
  double result = 0.0;
  size_t i = 0;
  while (i < n) {
    uint64_t part = 0, multiplier = 1;
    while (i < n && multiplier <= (std::numeric_limits<uint64_t>::max)() / 64) {
      part = part * (uint64_t) radix + (uint64_t) digitValue(s[i++]);
      multiplier *= (uint64_t) radix;
    }
    result = result * (double) multiplier + (double) part;
  }
  return result;
}

/** ECMAScript's ToNumber applied to a string. */
inline double stringToNumber(const std::string& text) {
  size_t from = 0, to = 0;
  trimBounds(text, true, true, from, to);
  const char* s = text.data() + from;
  const size_t n = to - from;
  if (n == 0) return 0.0;
  if (n > 2 && s[0] == '0') {
    int radix = 0;
    if (s[1] == 'x' || s[1] == 'X') radix = 16;
    if (s[1] == 'o' || s[1] == 'O') radix = 8;
    if (s[1] == 'b' || s[1] == 'B') radix = 2;
    if (radix) {
      for (size_t i = 2; i < n; ++i)
        if (digitValue(s[i]) >= radix) return std::numeric_limits<double>::quiet_NaN();
      return digitsToDouble(s + 2, n - 2, radix);
    }
  }
  size_t i = 0;
  bool negative = false;
  if (s[0] == '+' || s[0] == '-') {
    negative = s[0] == '-';
    i = 1;
  }
  if (n - i == 8 && std::memcmp(s + i, "Infinity", 8) == 0)
    return negative ? -std::numeric_limits<double>::infinity()
                    : std::numeric_limits<double>::infinity();
  if (matchDecimal(s, n) != n) return std::numeric_limits<double>::quiet_NaN();
  return parseDecimal(s, n);
}

inline double parseFloatPrefix(const std::string& text) {
  size_t from = 0, to = 0;
  trimBounds(text, true, false, from, to);
  const char* s = text.data() + from;
  const size_t n = to - from;
  size_t i = 0;
  bool negative = false;
  if (i < n && (s[i] == '+' || s[i] == '-')) {
    negative = s[i] == '-';
    ++i;
  }
  if (n - i >= 8 && std::memcmp(s + i, "Infinity", 8) == 0)
    return negative ? -std::numeric_limits<double>::infinity()
                    : std::numeric_limits<double>::infinity();
  const size_t m = matchDecimal(s, n);
  if (m == 0) return std::numeric_limits<double>::quiet_NaN();
  return parseDecimal(s, m);
}

inline double parseIntText(const std::string& text, double radixArg) {
  size_t from = 0, to = 0;
  trimBounds(text, true, false, from, to);
  const char* s = text.data() + from;
  const size_t n = to - from;
  size_t i = 0;
  bool negative = false;
  if (i < n && (s[i] == '+' || s[i] == '-')) {
    negative = s[i] == '-';
    ++i;
  }
  int radix = toInt32(radixArg);
  bool stripPrefix = true;
  if (radix != 0) {
    if (radix < 2 || radix > 36) return std::numeric_limits<double>::quiet_NaN();
    if (radix != 16) stripPrefix = false;
  } else {
    radix = 10;
  }
  if (stripPrefix && i + 1 < n && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
    i += 2;
    radix = 16;
  }
  const size_t start = i;
  while (i < n && digitValue(s[i]) < radix) ++i;
  if (i == start) return std::numeric_limits<double>::quiet_NaN();
  double v = 0.0;
  if (radix == 10) {
    v = parseDecimal(s + start, i - start); // correctly rounded, however long
  } else {
    v = digitsToDouble(s + start, i - start, radix);
  }
  return negative ? -v : v;
}

/** Where the native stack is now. Not the address of a local: under a
 *  sanitizer's fake stack that is a heap address, and the budget would be
 *  measured against nothing. */
inline uintptr_t stackPosition() noexcept {
#if defined(_MSC_VER)
  return (uintptr_t) _AddressOfReturnAddress();
#elif defined(__GNUC__) || defined(__clang__)
  return (uintptr_t) __builtin_frame_address(0);
#else
  volatile char marker = 0;
  return (uintptr_t) &marker;
#endif
}

/** The lowest address of this thread's stack, or 0 if the platform cannot
 *  say. Asked once per thread: a thread's stack does not move. */
inline uintptr_t stackLowLimit() noexcept {
  static thread_local uintptr_t low = 1; // 1: not asked yet
  if (low != 1) return low;
  low = 0;
#if defined(_WIN32)
  // The stack is one reservation, and VirtualQuery on any address in it
  // names where the reservation starts. (GetCurrentThreadStackLimits says
  // the same, but only from Windows 8 and only with _WIN32_WINNT set for it.)
  MEMORY_BASIC_INFORMATION info;
  const uintptr_t here = stackPosition();
  if (VirtualQuery((const void*) here, &info, sizeof(info)) != 0)
    low = (uintptr_t) info.AllocationBase;
#elif defined(__APPLE__)
  pthread_t self = pthread_self();
  const uintptr_t top = (uintptr_t) pthread_get_stackaddr_np(self);
  const size_t size = pthread_get_stacksize_np(self);
  if (top > size) low = top - size;
#elif defined(__linux__)
  pthread_attr_t attr;
  if (pthread_getattr_np(pthread_self(), &attr) == 0) {
    void* addr = nullptr;
    size_t size = 0;
    if (pthread_attr_getstack(&attr, &addr, &size) == 0) low = (uintptr_t) addr;
    pthread_attr_destroy(&attr);
  }
#endif
  return low;
}

/** The stack budget for an entry at `base`: what the engine was told it may
 *  use, but never more than the thread has left below `base` less a margin
 *  for the host and for the frames between two checks. A plugin's script can
 *  run on a host thread with 512 KB of stack or on a main thread with 8 MB;
 *  a fixed budget is either too small for one or an overflow on the other.
 *  Where the platform cannot say, a conservative 256 KB. */
inline size_t stackBudget(uintptr_t base, size_t wanted) noexcept {
  const size_t margin = 64 * 1024;
  const uintptr_t low = stackLowLimit();
  if (low == 0 || base <= low) return (std::min)(wanted, (size_t) 256 * 1024);
  const size_t room = (size_t) (base - low);
  return (std::min)(wanted, room > 2 * margin ? room - margin : room / 2);
}

} // namespace js
} // namespace detail

namespace detail {
namespace js {

struct FunctionCode;
struct Program;

// ── The heap: arrays, functions, scopes ─────────────────────────────────────

class ArrayObject : public DynamicObject {
public:
  ArrayObject() { cellKind_ = JsVar::Kind::Array; }
  ~ArrayObject() override {
    std::vector<std::shared_ptr<GcNode>> stack;
    for (JsVar& v : elements) Access::dropRef(v, stack);
    elements.clear();
    releaseIteratively(stack);
  }
  void gcVisit(std::vector<const GcNode*>& out) const override {
    DynamicObject::gcVisit(out);
    for (const JsVar& v : elements) Access::visit(v, out);
  }
  void gcDrop(std::vector<std::shared_ptr<GcNode>>& out) override {
    for (JsVar& v : elements) Access::dropRef(v, out);
    elements.clear();
    DynamicObject::gcDrop(out);
  }
  void touch() { recharge(); }

  std::vector<JsVar> elements;

protected:
  long long extraBytes() const override {
    return (long long) (elements.capacity() * sizeof(JsVar)) + 16;
  }
};

class FunctionObject : public DynamicObject {
public:
  enum class Type : uint8_t { Script, Native, Bound };
  FunctionObject() { cellKind_ = JsVar::Kind::Function; }
  ~FunctionObject() override {
    std::vector<std::shared_ptr<GcNode>> stack;
    dropOwn(stack);
    releaseIteratively(stack);
  }
  void gcVisit(std::vector<const GcNode*>& out) const override;
  void gcDrop(std::vector<std::shared_ptr<GcNode>>& out) override {
    dropOwn(out);
    DynamicObject::gcDrop(out);
  }

  Type type = Type::Native;
  JsVar::NativeFunction native;
  const FunctionCode* code = nullptr;
  std::shared_ptr<const Program> program; // keeps `code` alive
  std::shared_ptr<Scope> scope;
  JsVar boundTarget, boundThis;
  std::vector<JsVar> boundArgs;
  std::string name;
  int arity = 0;

protected:
  long long extraBytes() const override {
    return (long long) (sizeof(FunctionObject) - sizeof(DynamicObject) + name.capacity() +
                        boundArgs.capacity() * sizeof(JsVar));
  }

private:
  void dropOwn(std::vector<std::shared_ptr<GcNode>>& out);
};

struct Binding {
  enum : uint8_t { kConst = 1, kUninitialised = 2 };
  std::string name;
  JsVar value;
  uint8_t flags = 0;
};

/** A lexical environment: one function call's variables, or one block's
 *  let and const. Its parent is where a name not found here is looked up. */
class Scope : public GcNode {
public:
  Scope() = default;
  ~Scope() override {
    std::vector<std::shared_ptr<GcNode>> stack;
    gcDrop(stack);
    releaseIteratively(stack);
    if (meter) meter->bytes -= charged;
  }
  void gcVisit(std::vector<const GcNode*>& out) const override {
    if (parent) out.push_back(parent.get());
    for (const Binding& b : vars) Access::visit(b.value, out);
    Access::visit(thisValue, out);
  }
  void gcDrop(std::vector<std::shared_ptr<GcNode>>& out) override {
    if (parent) out.push_back(std::shared_ptr<GcNode>(std::move(parent)));
    parent.reset();
    for (Binding& b : vars) Access::dropRef(b.value, out);
    vars.clear();
    Access::dropRef(thisValue, out);
  }
  Binding* find(const std::string& name) {
    for (Binding& b : vars)
      if (b.name.size() == name.size() && b.name == name) return &b;
    return nullptr;
  }
  void recharge() {
    if (!meter) return;
    const long long now =
        (long long) (sizeof(Scope) + 48 + vars.capacity() * (sizeof(Binding) + 16));
    meter->bytes += now - charged;
    charged = now;
  }

  std::shared_ptr<Scope> parent;
  std::vector<Binding> vars;
  JsVar thisValue;
  bool hasThis = false;    // a function's scope (not an arrow's): `this` lives here
  bool isFunction = false; // where `var` declarations land
  std::shared_ptr<Meter> meter;
  long long charged = 0;
};

inline void FunctionObject::gcVisit(std::vector<const GcNode*>& out) const {
  DynamicObject::gcVisit(out);
  if (scope) out.push_back(scope.get());
  Access::visit(boundTarget, out);
  Access::visit(boundThis, out);
  for (const JsVar& v : boundArgs) Access::visit(v, out);
}

inline void FunctionObject::dropOwn(std::vector<std::shared_ptr<GcNode>>& out) {
  if (scope) out.push_back(std::shared_ptr<GcNode>(std::move(scope)));
  scope.reset();
  Access::dropRef(boundTarget, out);
  Access::dropRef(boundThis, out);
  for (JsVar& v : boundArgs) Access::dropRef(v, out);
  boundArgs.clear();
  // A native function's closure may hold values too, which the collector
  // cannot see into; it is dropped with the rest, as garbage.
  native = nullptr;
}

// ── Tokens ──────────────────────────────────────────────────────────────────

enum class Tok : uint8_t {
  End, Name, Num, Str, Template,
  LParen, RParen, LBrace, RBrace, LBracket, RBracket, Semi, Comma, Dot, Ellipsis,
  Question, QuestionDot, Colon, Arrow,
  Assign, AddAssign, SubAssign, MulAssign, DivAssign, ModAssign, ExpAssign, ShlAssign,
  ShrAssign, UShrAssign, AndAssign, OrAssign, XorAssign, LogAndAssign, LogOrAssign,
  NullishAssign,
  Add, Sub, Mul, Div, Mod, Exp, Inc, Dec, Shl, Shr, UShr, BitAnd, BitOr, BitXor, BitNot,
  Not, LogAnd, LogOr, Nullish, Eq, Ne, StrictEq, StrictNe, Lt, Gt, Le, Ge,
  // Operators that are words, used in the tree rather than by the lexer.
  In, Instanceof, Typeof, Void, Delete, Neg, Pos
};

struct TemplateData;

struct Token {
  Tok type = Tok::End;
  bool newlineBefore = false;
  int line = 1;
  int col = 1;
  size_t start = 0;
  size_t end = 0;
  double number = 0.0;
  std::string text; // a name, or a string literal's value
  // A template literal: its n + 1 string parts, and the tokens of the n
  // expressions between them. Out of line, because a program is mostly other
  // tokens and the size of every token is what a long program costs.
  std::unique_ptr<TemplateData> tpl;
};

struct TemplateData {
  std::vector<std::string> parts;
  std::vector<std::vector<Token>> exprs;
};

/** A syntax error, while lexing or parsing. Becomes a SyntaxError in script. */
struct SyntaxErr {
  std::string message;
  int line;
  int col;
};

/** A budget of native stack, measured from where the engine was entered. */
struct StackGuard {
  uintptr_t base = 0;
  size_t limit = 256 * 1024;
  bool exceeded() const noexcept {
    const uintptr_t here = stackPosition();
    const uintptr_t used = here < base ? base - here : here - base;
    return used > limit;
  }
};

inline bool isIdentStart(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || c >= 0x80;
}
inline bool isIdentPart(unsigned char c) { return isIdentStart(c) || (c >= '0' && c <= '9'); }

class Lexer {
public:
  /** `maxTokens` bounds what lexing may allocate: a program of commas is a
   *  token per byte, and the tokens are freed only after parsing. */
  Lexer(const std::string& source, const StackGuard* guard, size_t maxTokens)
      : src_(source), guard_(guard), maxTokens_(maxTokens) {}

  void run(std::vector<Token>& out) {
    lex(out, false, 0);
  }

private:
  const std::string& src_;
  const StackGuard* guard_;
  size_t maxTokens_;
  size_t tokens_ = 0;
  size_t pos_ = 0;
  int line_ = 1;
  int col_ = 1;

  [[noreturn]] void fail(const std::string& message) { throw SyntaxErr{message, line_, col_}; }
  [[noreturn]] void failAt(const std::string& message, int line, int col) {
    throw SyntaxErr{message, line, col};
  }

  unsigned char peek(size_t k = 0) const {
    return pos_ + k < src_.size() ? (unsigned char) src_[pos_ + k] : 0;
  }
  bool more() const { return pos_ < src_.size(); }

  void advance() {
    const unsigned char c = (unsigned char) src_[pos_];
    if (c == '\n' || (c == '\r' && (pos_ + 1 >= src_.size() || src_[pos_ + 1] != '\n'))) {
      ++line_;
      col_ = 1;
    } else if ((c & 0xC0) != 0x80) {
      ++col_;
    }
    ++pos_;
  }
  void advance(size_t n) {
    while (n-- && more()) advance();
  }

  /** Skips white space and comments; true if a line ended on the way. */
  bool skipSpace() {
    bool newline = false;
    while (more()) {
      const unsigned char c = peek();
      if (c == '\n') {
        newline = true;
        advance();
      } else if (c == ' ' || c == '\t' || c == '\r' || c == 0x0B || c == 0x0C) {
        if (c == '\r') newline = true;
        advance();
      } else if (c == '/' && peek(1) == '/') {
        while (more() && peek() != '\n' && peek() != '\r') advance();
      } else if (c == '/' && peek(1) == '*') {
        const int line = line_, col = col_;
        advance(2);
        bool closed = false;
        while (more()) {
          if (peek() == '*' && peek(1) == '/') {
            advance(2);
            closed = true;
            break;
          }
          if (peek() == '\n' || peek() == '\r') newline = true;
          advance();
        }
        if (!closed) failAt("Invalid or unexpected token (unterminated comment)", line, col);
      } else if (c >= 0x80) {
        size_t i = pos_;
        const uint32_t cp = decodeCodePoint((const unsigned char*) src_.data(), src_.size(), i);
        if (cp == 0x2028 || cp == 0x2029) {
          newline = true;
        } else if (!isJsSpace(cp)) {
          break;
        }
        advance(i - pos_);
      } else if (c == '#' && pos_ == 0 && peek(1) == '!') {
        while (more() && peek() != '\n') advance(); // a hashbang line
      } else {
        break;
      }
    }
    return newline;
  }

  void lex(std::vector<Token>& out, bool untilBrace, int nesting) {
    if (nesting > 64 || (guard_ && guard_->exceeded()))
      fail("template literals nested too deeply");
    int depth = 0;
    for (;;) {
      if (++tokens_ > maxTokens_) fail("program too large for the memory limit");
      Token t;
      t.newlineBefore = skipSpace();
      t.line = line_;
      t.col = col_;
      t.start = pos_;
      if (!more()) {
        if (untilBrace) fail("Unterminated template literal");
        t.type = Tok::End;
        t.end = pos_;
        out.push_back(std::move(t));
        return;
      }
      const unsigned char c = peek();
      if (untilBrace && c == kRBrace && depth == 0) {
        advance();
        t.type = Tok::End;
        t.end = pos_;
        out.push_back(std::move(t));
        return;
      }
      if (isIdentStart(c)) {
        while (more() && isIdentPart(peek())) {
          if (peek() >= 0x80) {
            size_t i = pos_;
            const uint32_t cp =
                decodeCodePoint((const unsigned char*) src_.data(), src_.size(), i);
            if (isJsSpace(cp) || cp == 0x2028 || cp == 0x2029) break;
          }
          advance();
        }
        t.type = Tok::Name;
        // An identifier of invalid UTF-8 bytes must still be canonical before
        // it becomes a string -- a property key or a function's name -- or its
        // UTF-16 length (utf16Units, which assumes canonical text) would not
        // match what toUtf16 decodes, and indexing it would read out of bounds.
        t.text = canonicalText(src_.substr(t.start, pos_ - t.start));
      } else if ((c >= '0' && c <= '9') || (c == '.' && peek(1) >= '0' && peek(1) <= '9')) {
        lexNumber(t);
      } else if (c == '"' || c == '\'') {
        lexString(t);
      } else if (c == '`') {
        lexTemplate(t, nesting);
      } else {
        lexPunct(t);
        if (t.type == Tok::LBrace) ++depth;
        if (t.type == Tok::RBrace) --depth;
      }
      t.end = pos_;
      out.push_back(std::move(t));
    }
  }

  void lexNumber(Token& t) {
    const unsigned char c = peek();
    std::string digits;
    if (c == '0' && (peek(1) == 'x' || peek(1) == 'X' || peek(1) == 'o' || peek(1) == 'O' ||
                     peek(1) == 'b' || peek(1) == 'B')) {
      const char k = (char) (peek(1) | 0x20);
      const int radix = k == 'x' ? 16 : k == 'o' ? 8 : 2;
      advance(2);
      while (more()) {
        const unsigned char d = peek();
        if (d == '_') {
          advance();
          continue;
        }
        if (digitValue((char) d) >= radix) break;
        digits += (char) d;
        advance();
      }
      if (digits.empty()) fail("Invalid or unexpected token");
      t.number = digitsToDouble(digits.data(), digits.size(), radix);
    } else if (c == '0' && peek(1) >= '0' && peek(1) <= '9') {
      // A legacy octal literal, as sloppy-mode JavaScript reads one: 010 is 8,
      // but 089 is decimal because 8 and 9 are not octal digits.
      size_t i = pos_;
      bool octal = true;
      while (i < src_.size() && src_[i] >= '0' && src_[i] <= '9') {
        if (src_[i] >= '8') octal = false;
        ++i;
      }
      if (octal) {
        double v = 0.0;
        while (more() && peek() >= '0' && peek() <= '7') {
          v = v * 8 + (peek() - '0');
          advance();
        }
        t.number = v;
      } else {
        lexDecimal(t);
      }
    } else {
      lexDecimal(t);
    }
    if (more() && (isIdentStart(peek()) || (peek() >= '0' && peek() <= '9')))
      fail("Invalid or unexpected token");
    t.type = Tok::Num;
  }

  void lexDecimal(Token& t) {
    std::string digits;
    auto take = [&]() {
      while (more() && ((peek() >= '0' && peek() <= '9') || peek() == '_')) {
        if (peek() != '_') digits += (char) peek();
        advance();
      }
    };
    take();
    if (peek() == '.') {
      digits += '.';
      advance();
      take();
    }
    if ((peek() == 'e' || peek() == 'E') &&
        ((peek(1) >= '0' && peek(1) <= '9') ||
         ((peek(1) == '+' || peek(1) == '-') && peek(2) >= '0' && peek(2) <= '9'))) {
      digits += 'e';
      advance();
      if (peek() == '+' || peek() == '-') {
        digits += (char) peek();
        advance();
      }
      take();
    }
    if (digits == ".") fail("Invalid or unexpected token");
    t.number = parseDecimal(digits.data(), digits.size());
  }

  static int hexValue(unsigned char c) {
    const int v = digitValue((char) c);
    return v < 16 ? v : -1;
  }

  /** One escape sequence, after the backslash, into UTF-16 units. */
  void lexEscape(std::u16string& out, bool templateLiteral) {
    const unsigned char c = peek();
    if (!more()) fail("Invalid or unexpected token");
    switch (c) {
      case 'n': out += u'\n'; advance(); return;
      case 'r': out += u'\r'; advance(); return;
      case 't': out += u'\t'; advance(); return;
      case 'b': out += u'\b'; advance(); return;
      case 'f': out += u'\f'; advance(); return;
      case 'v': out += u'\v'; advance(); return;
      case '\r':
        advance();
        if (peek() == '\n') advance();
        return; // a line continuation
      case '\n': advance(); return;
      case 'x': {
        const int a = hexValue(peek(1)), b = hexValue(peek(2));
        if (a < 0 || b < 0) fail("Invalid hexadecimal escape sequence");
        out += (char16_t) (a * 16 + b);
        advance(3);
        return;
      }
      case 'u': {
        advance();
        uint32_t cp = 0;
        if (peek() == kLBrace) {
          advance();
          size_t count = 0;
          while (more() && peek() != kRBrace) {
            const int h = hexValue(peek());
            if (h < 0) fail("Invalid Unicode escape sequence");
            cp = cp * 16 + (uint32_t) h;
            if (cp > 0x10FFFF) fail("Undefined Unicode code-point");
            ++count;
            advance();
          }
          if (!more() || count == 0) fail("Invalid Unicode escape sequence");
          advance();
        } else {
          for (int i = 0; i < 4; ++i) {
            const int h = hexValue(peek());
            if (h < 0) fail("Invalid Unicode escape sequence");
            cp = cp * 16 + (uint32_t) h;
            advance();
          }
        }
        if (cp >= 0x10000) {
          out += (char16_t) (0xD800 + ((cp - 0x10000) >> 10));
          out += (char16_t) (0xDC00 + ((cp - 0x10000) & 0x3FF));
        } else {
          out += (char16_t) cp;
        }
        return;
      }
      default:
        break;
    }
    if (c >= '0' && c <= '7') {
      if (c == '0' && !(peek(1) >= '0' && peek(1) <= '9')) {
        out += u'\0';
        advance();
        return;
      }
      if (templateLiteral) fail("Octal escape sequences are not allowed in template strings");
      // Legacy octal: up to three digits, at most \377.
      int v = 0, count = 0;
      const int maxDigits = c <= '3' ? 3 : 2;
      while (count < maxDigits && peek() >= '0' && peek() <= '7') {
        v = v * 8 + (peek() - '0');
        advance();
        ++count;
      }
      out += (char16_t) v;
      return;
    }
    // Anything else stands for itself, including U+2028 and U+2029 (which
    // are line continuations) -- decoded as a whole code point.
    size_t i = pos_;
    const uint32_t cp = decodeCodePoint((const unsigned char*) src_.data(), src_.size(), i);
    advance(i - pos_);
    if (cp == 0x2028 || cp == 0x2029) return;
    appendUnits(out, cp);
  }

  static void appendUnits(std::u16string& out, uint32_t cp) {
    if (cp >= 0x10000) {
      out += (char16_t) (0xD800 + ((cp - 0x10000) >> 10));
      out += (char16_t) (0xDC00 + ((cp - 0x10000) & 0x3FF));
    } else {
      out += (char16_t) cp;
    }
  }

  void lexString(Token& t) {
    const unsigned char quote = peek();
    advance();
    std::u16string units;
    for (;;) {
      if (!more()) failAt("Invalid or unexpected token (unterminated string)", t.line, t.col);
      const unsigned char c = peek();
      if (c == quote) {
        advance();
        break;
      }
      if (c == '\n' || c == '\r')
        failAt("Invalid or unexpected token (unterminated string)", t.line, t.col);
      if (c == '\\') {
        advance();
        lexEscape(units, false);
        continue;
      }
      size_t i = pos_;
      const uint32_t cp = decodeCodePoint((const unsigned char*) src_.data(), src_.size(), i);
      advance(i - pos_);
      appendUnits(units, cp);
    }
    t.type = Tok::Str;
    t.text = fromUtf16(units);
  }

  void lexTemplate(Token& t, int nesting) {
    advance(); // `
    t.tpl.reset(new TemplateData());
    std::u16string units;
    for (;;) {
      if (!more()) failAt("Unterminated template literal", t.line, t.col);
      const unsigned char c = peek();
      if (c == '`') {
        advance();
        break;
      }
      if (c == '\\') {
        advance();
        lexEscape(units, true);
        continue;
      }
      if (c == '$' && peek(1) == kLBrace) {
        advance(2);
        t.tpl->parts.push_back(fromUtf16(units));
        units.clear();
        t.tpl->exprs.emplace_back();
        lex(t.tpl->exprs.back(), true, nesting + 1);
        continue;
      }
      if (c == '\r') {
        // A template's line breaks are \n whatever the file used.
        advance();
        if (peek() == '\n') advance();
        units += u'\n';
        continue;
      }
      size_t i = pos_;
      const uint32_t cp = decodeCodePoint((const unsigned char*) src_.data(), src_.size(), i);
      advance(i - pos_);
      appendUnits(units, cp);
    }
    t.tpl->parts.push_back(fromUtf16(units));
    t.type = Tok::Template;
  }

  void lexPunct(Token& t) {
    struct P {
      const char* text;
      Tok type;
    };
    // Longest first, so ">>>=" is not read as ">>" then ">=".
    static const P table[] = {
        {">>>=", Tok::UShrAssign}, {"...", Tok::Ellipsis}, {"===", Tok::StrictEq},
        {"!==", Tok::StrictNe}, {"**=", Tok::ExpAssign}, {"<<=", Tok::ShlAssign},
        {">>=", Tok::ShrAssign}, {">>>", Tok::UShr}, {"&&=", Tok::LogAndAssign},
        {"||=", Tok::LogOrAssign}, {"?" "?=", Tok::NullishAssign}, {"=>", Tok::Arrow},
        {"==", Tok::Eq}, {"!=", Tok::Ne}, {"<=", Tok::Le}, {">=", Tok::Ge},
        {"&&", Tok::LogAnd}, {"||", Tok::LogOr}, {"?" "?", Tok::Nullish}, {"?.", Tok::QuestionDot},
        {"++", Tok::Inc}, {"--", Tok::Dec}, {"+=", Tok::AddAssign}, {"-=", Tok::SubAssign},
        {"*=", Tok::MulAssign}, {"/=", Tok::DivAssign}, {"%=", Tok::ModAssign},
        {"&=", Tok::AndAssign}, {"|=", Tok::OrAssign}, {"^=", Tok::XorAssign},
        {"**", Tok::Exp}, {"<<", Tok::Shl}, {">>", Tok::Shr},
        {"(", Tok::LParen}, {")", Tok::RParen}, {"{", Tok::LBrace}, {"}", Tok::RBrace},
        {"[", Tok::LBracket}, {"]", Tok::RBracket}, {";", Tok::Semi}, {",", Tok::Comma},
        {".", Tok::Dot}, {"?", Tok::Question}, {":", Tok::Colon}, {"=", Tok::Assign},
        {"+", Tok::Add}, {"-", Tok::Sub}, {"*", Tok::Mul}, {"/", Tok::Div}, {"%", Tok::Mod},
        {"<", Tok::Lt}, {">", Tok::Gt}, {"&", Tok::BitAnd}, {"|", Tok::BitOr},
        {"^", Tok::BitXor}, {"~", Tok::BitNot}, {"!", Tok::Not},
    };
    for (const P& p : table) {
      const size_t n = std::strlen(p.text);
      if (src_.compare(pos_, n, p.text) == 0) {
        // "?." followed by a digit is a conditional and a number: a?.5:1.
        if (p.type == Tok::QuestionDot && peek(2) >= '0' && peek(2) <= '9') continue;
        advance(n);
        t.type = p.type;
        return;
      }
    }
    if (peek() == '/') fail("Invalid or unexpected token");
    std::string shown;
    size_t i = pos_;
    decodeCodePoint((const unsigned char*) src_.data(), src_.size(), i);
    shown = src_.substr(pos_, i - pos_);
    fail("Invalid or unexpected token '" + shown + "'");
  }
};

// ── The syntax tree ─────────────────────────────────────────────────────────

enum class N : uint8_t {
  Num, Str, Template, Ident, This, Null, True, False, ArrayLit, ObjectLit, Prop, Func,
  Member, Index, Call, New, Unary, Update, Binary, Logical, Conditional, Assign, Sequence,
  Spread, OptChain, Hole,
  VarDecl, Declarator, ExprStmt, Block, If, For, ForIn, ForOf, While, DoWhile, Break,
  Continue, Return, Throw, Try, Switch, Case, Empty, FuncDecl, Program
};

enum NodeFlags : uint8_t {
  kOptional = 1,     // a ?. link
  kPrefix = 2,       // ++x rather than x++
  kComputed = 4,     // { [key]: value }
  kHasLexical = 8,   // a block that declares let, const or a function
  kPerIteration = 16, // a loop whose closures need a binding per iteration
  kParen = 32,       // written in parentheses
};

enum class Decl : uint8_t { Var, Let, Const };

struct Node {
  N kind;
  Tok op = Tok::End;
  uint8_t flags = 0;
  Decl decl = Decl::Var;
  int line = 0;
  int col = 0;
  double num = 0.0;
  std::string str;
  std::vector<std::unique_ptr<Node>> kids;
  std::vector<std::string> strs; // a template's string parts
  std::unique_ptr<FunctionCode> fn;
  JsVar value; // a string literal, made once rather than on every evaluation

  Node(N k, int l, int c) : kind(k), line(l), col(c) {}
  Node(const Node&) = delete;
  Node& operator=(const Node&) = delete;
  ~Node();

  Node* kid(size_t i) const { return i < kids.size() ? kids[i].get() : nullptr; }
};

struct FunctionCode {
  std::string name;
  std::vector<std::string> params;
  std::vector<std::unique_ptr<Node>> defaults; // per parameter; null where none
  bool hasRest = false;
  bool isArrow = false;
  bool isExpressionBody = false;
  bool isNamedExpression = false; // `function f() {}` as an expression binds f inside
  bool usesArguments = false;
  std::unique_ptr<Node> body;
  std::vector<std::string> varNames;
  size_t sourceStart = 0;
  size_t sourceEnd = 0;
  int line = 0;
};

inline Node::~Node() {
  // Iteratively, so a tree as deep as a long chain of a.b.c.d... cannot
  // overflow the stack while it is freed.
  std::vector<std::unique_ptr<Node>> stack;
  auto take = [&stack](Node& n) {
    for (auto& k : n.kids)
      if (k) stack.push_back(std::move(k));
    n.kids.clear();
    if (n.fn) {
      for (auto& d : n.fn->defaults)
        if (d) stack.push_back(std::move(d));
      if (n.fn->body) stack.push_back(std::move(n.fn->body));
    }
  };
  take(*this);
  while (!stack.empty()) {
    std::unique_ptr<Node> n = std::move(stack.back());
    stack.pop_back();
    take(*n);
  }
}

/** A parsed program, kept alive by every function made from it. */
struct Program {
  std::string source;
  std::unique_ptr<Node> root;
  std::vector<std::string> varNames; // `var`s at the top level
  bool isExpression = false;
  std::shared_ptr<Meter> meter;
  long long charged = 0;
  Program() = default;
  Program(const Program&) = delete;
  Program& operator=(const Program&) = delete;
  ~Program() {
    root.reset();
    if (meter) meter->bytes -= charged;
  }
};

// ── The parser ──────────────────────────────────────────────────────────────

class Parser {
public:
  Parser(Program& program, const StackGuard* guard, size_t maxNodes)
      : prog_(program), guard_(guard), maxNodes_(maxNodes) {}

  /** The whole program. */
  std::unique_ptr<Node> parseProgram(const std::vector<Token>& tokens) {
    setTokens(tokens);
    Context top;
    top.varNames = &prog_.varNames;
    ctx_.push_back(top);
    auto root = node(N::Program, cur());
    while (cur().type != Tok::End) root->kids.push_back(parseStatement());
    markLexical(*root);
    ctx_.pop_back();
    return root;
  }

  /** One expression that must be all of the input, or null. */
  std::unique_ptr<Node> parseWholeExpression(const std::vector<Token>& tokens) {
    setTokens(tokens);
    Context top;
    top.varNames = &prog_.varNames;
    ctx_.push_back(top);
    std::unique_ptr<Node> e;
    try {
      e = parseExpression(false);
    } catch (const SyntaxErr&) {
      ctx_.pop_back();
      return nullptr;
    }
    ctx_.pop_back();
    if (cur().type != Tok::End) return nullptr;
    return e;
  }

  size_t nodeCount() const { return nodes_; }

private:
  struct Context {
    FunctionCode* fn = nullptr; // null at the top level
    std::vector<std::string>* varNames = nullptr;
    int loops = 0;
    int switches = 0;
  };

  Program& prog_;
  const StackGuard* guard_;
  size_t maxNodes_;
  size_t nodes_ = 0;
  const std::vector<Token>* toks_ = nullptr;
  std::vector<size_t> match_; // for each '(' the index of its ')'
  size_t at_ = 0;
  std::vector<Context> ctx_;
  int closures_ = 0;

  void setTokens(const std::vector<Token>& tokens) {
    toks_ = &tokens;
    at_ = 0;
    // Every bracket's partner, once: deciding whether "(" starts an arrow
    // function's parameters is then a lookup rather than a scan, and nested
    // brackets cannot make parsing quadratic.
    match_.assign(tokens.size(), (size_t) -1);
    std::vector<size_t> open;
    for (size_t i = 0; i < tokens.size(); ++i) {
      const Tok t = tokens[i].type;
      if (t == Tok::LParen || t == Tok::LBracket || t == Tok::LBrace) {
        open.push_back(i);
      } else if (t == Tok::RParen || t == Tok::RBracket || t == Tok::RBrace) {
        if (!open.empty()) {
          match_[open.back()] = i;
          open.pop_back();
        }
      }
    }
  }

  const Token& cur() const { return (*toks_)[at_]; }
  const Token& ahead(size_t k = 1) const {
    const size_t i = at_ + k;
    return i < toks_->size() ? (*toks_)[i] : toks_->back();
  }
  void next() {
    if (at_ + 1 < toks_->size()) ++at_;
  }
  bool isName(const char* word) const { return cur().type == Tok::Name && cur().text == word; }

  [[noreturn]] void failAt(const Token& t, const std::string& message) {
    throw SyntaxErr{message, t.line, t.col};
  }
  [[noreturn]] void unexpected() {
    const Token& t = cur();
    switch (t.type) {
      case Tok::End: failAt(t, "Unexpected end of input");
      case Tok::Num: failAt(t, "Unexpected number");
      case Tok::Str: failAt(t, "Unexpected string");
      case Tok::Template: failAt(t, "Unexpected template string");
      case Tok::Name:
        failAt(t, (isReserved(t.text) ? "Unexpected token '" : "Unexpected identifier '") +
                      t.text + "'");
      default:
        failAt(t, "Unexpected token '" + prog_.source.substr(t.start, t.end - t.start) + "'");
    }
  }
  void expect(Tok type) {
    if (cur().type != type) unexpected();
    next();
  }
  void guard() {
    if (guard_ && guard_->exceeded()) failAt(cur(), "Maximum call stack size exceeded");
  }

  std::unique_ptr<Node> node(N kind, const Token& at) {
    if (++nodes_ > maxNodes_) failAt(at, "program too large for the memory limit");
    return std::unique_ptr<Node>(new Node(kind, at.line, at.col));
  }

  static bool isReserved(const std::string& w) {
    static const char* words[] = {
        "break", "case", "catch", "class", "const", "continue", "debugger", "default",
        "delete", "do", "else", "enum", "export", "extends", "false", "finally", "for",
        "function", "if", "import", "in", "instanceof", "new", "null", "return", "super",
        "switch", "this", "throw", "true", "try", "typeof", "var", "void", "while", "with",
        "let"};
    for (const char* r : words)
      if (w == r) return true;
    return false;
  }

  std::string bindingName() {
    const Token& t = cur();
    if (t.type == Tok::LBracket || t.type == Tok::LBrace)
      failAt(t, "destructuring is not supported");
    if (t.type != Tok::Name || isReserved(t.text)) unexpected();
    std::string name = t.text;
    next();
    return name;
  }

  void semicolon() {
    if (cur().type == Tok::Semi) {
      next();
      return;
    }
    if (cur().type == Tok::RBrace || cur().type == Tok::End || cur().newlineBefore) return;
    unexpected();
  }

  void addVar(const std::string& name) {
    std::vector<std::string>* names = ctx_.back().varNames;
    if (std::find(names->begin(), names->end(), name) == names->end()) names->push_back(name);
  }

  /** Marks a block (or body) that declares something lexically, so that only
   *  those blocks pay for a scope of their own. */
  static void markLexical(Node& block) {
    for (auto& k : block.kids) {
      if (!k) continue;
      if ((k->kind == N::VarDecl && k->decl != Decl::Var) || k->kind == N::FuncDecl)
        block.flags |= kHasLexical;
    }
  }

  // ── Statements ────────────────────────────────────────────────────────────

  std::unique_ptr<Node> parseStatement() {
    guard();
    const Token& t = cur();
    switch (t.type) {
      case Tok::LBrace: return parseBlock();
      case Tok::Semi: {
        auto n = node(N::Empty, t);
        next();
        return n;
      }
      case Tok::Name: break;
      default: return parseExpressionStatement();
    }
    const std::string& w = t.text;
    if (w == "var" || w == "const" || (w == "let" && letStartsDeclaration())) {
      auto n = parseVarDecl(false);
      semicolon();
      return n;
    }
    if (w == "function") return parseFunction(true);
    if (w == "if") return parseIf();
    if (w == "for") return parseFor();
    if (w == "while") {
      auto n = node(N::While, t);
      next();
      expect(Tok::LParen);
      n->kids.push_back(parseExpression(false));
      expect(Tok::RParen);
      n->kids.push_back(loopBody());
      return n;
    }
    if (w == "do") {
      auto n = node(N::DoWhile, t);
      next();
      n->kids.push_back(loopBody());
      if (!isName("while")) unexpected();
      next();
      expect(Tok::LParen);
      n->kids.push_back(parseExpression(false));
      expect(Tok::RParen);
      if (cur().type == Tok::Semi) next(); // do-while needs no semicolon
      return n;
    }
    if (w == "return") {
      if (!ctx_.back().fn) failAt(t, "Illegal return statement");
      auto n = node(N::Return, t);
      next();
      if (cur().type != Tok::Semi && cur().type != Tok::RBrace && cur().type != Tok::End &&
          !cur().newlineBefore)
        n->kids.push_back(parseExpression(false));
      semicolon();
      return n;
    }
    if (w == "break" || w == "continue") {
      const bool isBreak = w == "break";
      auto n = node(isBreak ? N::Break : N::Continue, t);
      if (isBreak ? (ctx_.back().loops + ctx_.back().switches == 0) : ctx_.back().loops == 0)
        failAt(t, isBreak ? "Illegal break statement"
                          : "Illegal continue statement: no surrounding iteration statement");
      next();
      if (cur().type == Tok::Name && !cur().newlineBefore)
        failAt(cur(), "labels are not supported");
      semicolon();
      return n;
    }
    if (w == "throw") {
      auto n = node(N::Throw, t);
      next();
      if (cur().newlineBefore) failAt(cur(), "Illegal newline after throw");
      n->kids.push_back(parseExpression(false));
      semicolon();
      return n;
    }
    if (w == "try") return parseTry();
    if (w == "switch") return parseSwitch();
    if (w == "debugger") {
      auto n = node(N::Empty, t);
      next();
      semicolon();
      return n;
    }
    if (w == "class") failAt(t, "classes are not supported");
    if (w == "import" || w == "export") failAt(t, "modules are not supported");
    if (w == "with") failAt(t, "'with' is not supported");
    if (ahead().type == Tok::Colon && !isReserved(w)) failAt(t, "labels are not supported");
    return parseExpressionStatement();
  }

  bool letStartsDeclaration() const {
    const Token& n = ahead();
    return n.type == Tok::Name || n.type == Tok::LBracket || n.type == Tok::LBrace;
  }

  std::unique_ptr<Node> parseExpressionStatement() {
    auto n = node(N::ExprStmt, cur());
    n->kids.push_back(parseExpression(false));
    semicolon();
    return n;
  }

  std::unique_ptr<Node> parseBlock() {
    auto n = node(N::Block, cur());
    expect(Tok::LBrace);
    while (cur().type != Tok::RBrace) {
      if (cur().type == Tok::End) unexpected();
      n->kids.push_back(parseStatement());
    }
    next();
    markLexical(*n);
    return n;
  }

  std::unique_ptr<Node> loopBody() {
    ++ctx_.back().loops;
    auto body = parseStatement();
    --ctx_.back().loops;
    return body;
  }

  std::unique_ptr<Node> parseVarDecl(bool inForHead) {
    const Token& t = cur();
    auto n = node(N::VarDecl, t);
    n->decl = t.text == "var" ? Decl::Var : t.text == "let" ? Decl::Let : Decl::Const;
    next();
    for (;;) {
      auto d = node(N::Declarator, cur());
      d->str = bindingName();
      if (n->decl == Decl::Var) addVar(d->str);
      if (cur().type == Tok::Assign) {
        next();
        d->kids.push_back(parseAssignment(inForHead));
      } else if (n->decl == Decl::Const && !(inForHead && (isName("of") || isName("in")))) {
        failAt(cur(), "Missing initializer in const declaration");
      }
      n->kids.push_back(std::move(d));
      if (cur().type != Tok::Comma) break;
      next();
    }
    return n;
  }

  std::unique_ptr<Node> parseIf() {
    auto n = node(N::If, cur());
    next();
    expect(Tok::LParen);
    n->kids.push_back(parseExpression(false));
    expect(Tok::RParen);
    n->kids.push_back(parseStatement());
    if (isName("else")) {
      next();
      n->kids.push_back(parseStatement());
    }
    return n;
  }

  std::unique_ptr<Node> parseFor() {
    const Token& t = cur();
    next();
    if (isName("await")) failAt(cur(), "for await is not supported");
    expect(Tok::LParen);
    const int closuresBefore = closures_;
    std::unique_ptr<Node> init;
    if (cur().type == Tok::Semi) {
      // no initialiser
    } else if (isName("var") || isName("const") || (isName("let") && letStartsDeclaration())) {
      init = parseVarDecl(true);
    } else {
      init = parseExpression(true);
    }
    if (init && (isName("of") || isName("in"))) {
      const bool isOf = isName("of");
      auto n = node(isOf ? N::ForOf : N::ForIn, t);
      if (init->kind == N::VarDecl) {
        if (init->kids.size() != 1 || !init->kids[0]->kids.empty())
          failAt(cur(), std::string("Invalid left-hand side in for-") + (isOf ? "of" : "in") +
                            " loop: must have a single binding.");
      } else if (!isTarget(*init)) {
        failAt(cur(), std::string("Invalid left-hand side in for-") + (isOf ? "of" : "in"));
      }
      next();
      n->kids.push_back(std::move(init));
      n->kids.push_back(isOf ? parseAssignment(false) : parseExpression(false));
      expect(Tok::RParen);
      n->kids.push_back(loopBody());
      if (closures_ != closuresBefore) n->flags |= kPerIteration;
      return n;
    }
    auto n = node(N::For, t);
    expect(Tok::Semi);
    std::unique_ptr<Node> test, update;
    if (cur().type != Tok::Semi) test = parseExpression(false);
    expect(Tok::Semi);
    if (cur().type != Tok::RParen) update = parseExpression(false);
    expect(Tok::RParen);
    n->kids.push_back(std::move(init));
    n->kids.push_back(std::move(test));
    n->kids.push_back(std::move(update));
    n->kids.push_back(loopBody());
    if (closures_ != closuresBefore) n->flags |= kPerIteration;
    return n;
  }

  std::unique_ptr<Node> parseTry() {
    auto n = node(N::Try, cur());
    next();
    n->kids.push_back(parseBlock());
    std::unique_ptr<Node> handler, finalizer;
    if (isName("catch")) {
      next();
      if (cur().type == Tok::LParen) {
        next();
        n->str = bindingName();
        expect(Tok::RParen);
      }
      handler = parseBlock();
    }
    if (isName("finally")) {
      next();
      finalizer = parseBlock();
    }
    if (!handler && !finalizer) failAt(cur(), "Missing catch or finally after try");
    n->kids.push_back(std::move(handler));
    n->kids.push_back(std::move(finalizer));
    return n;
  }

  std::unique_ptr<Node> parseSwitch() {
    auto n = node(N::Switch, cur());
    next();
    expect(Tok::LParen);
    n->kids.push_back(parseExpression(false));
    expect(Tok::RParen);
    expect(Tok::LBrace);
    ++ctx_.back().switches;
    bool sawDefault = false;
    while (cur().type != Tok::RBrace) {
      auto c = node(N::Case, cur());
      if (isName("case")) {
        next();
        c->kids.push_back(parseExpression(false));
      } else if (isName("default")) {
        if (sawDefault) failAt(cur(), "More than one default clause in switch statement");
        sawDefault = true;
        next();
        c->kids.push_back(nullptr);
      } else {
        unexpected();
      }
      expect(Tok::Colon);
      while (cur().type != Tok::RBrace && !isName("case") && !isName("default")) {
        if (cur().type == Tok::End) unexpected();
        c->kids.push_back(parseStatement());
      }
      for (size_t i = 1; i < c->kids.size(); ++i) {
        const Node* k = c->kids[i].get();
        if ((k->kind == N::VarDecl && k->decl != Decl::Var) || k->kind == N::FuncDecl)
          n->flags |= kHasLexical;
      }
      n->kids.push_back(std::move(c));
    }
    --ctx_.back().switches;
    next();
    return n;
  }

  // ── Functions ─────────────────────────────────────────────────────────────

  std::unique_ptr<Node> parseFunction(bool declaration) {
    const Token& start = cur();
    auto n = node(declaration ? N::FuncDecl : N::Func, start);
    next(); // function
    if (cur().type == Tok::Mul) failAt(cur(), "generators are not supported");
    auto code = std::unique_ptr<FunctionCode>(new FunctionCode());
    code->line = start.line;
    code->sourceStart = start.start;
    if (cur().type == Tok::Name) {
      code->name = bindingName();
      code->isNamedExpression = !declaration;
    } else if (declaration) {
      unexpected();
    }
    if (declaration && ctx_.size() > 0) {
      // Annex B: a function declared in a block is also a var of the
      // enclosing function, once its block has run.
      addVar(code->name);
    }
    parseParams(*code);
    parseFunctionBody(*code);
    n->str = code->name;
    n->fn = std::move(code);
    ++closures_;
    return n;
  }

  void parseParams(FunctionCode& code) {
    expect(Tok::LParen);
    while (cur().type != Tok::RParen) {
      if (cur().type == Tok::Ellipsis) {
        next();
        code.params.push_back(bindingName());
        code.defaults.push_back(nullptr);
        code.hasRest = true;
        if (cur().type != Tok::RParen) failAt(cur(), "Rest parameter must be last formal parameter");
        break;
      }
      code.params.push_back(bindingName());
      if (cur().type == Tok::Assign) {
        next();
        code.defaults.push_back(parseAssignmentIn(code));
      } else {
        code.defaults.push_back(nullptr);
      }
      if (cur().type != Tok::Comma) break;
      next();
    }
    expect(Tok::RParen);
  }

  /** A default value is evaluated in the function's own scope, so it is
   *  parsed as part of the function: `arguments` in it is the function's. */
  std::unique_ptr<Node> parseAssignmentIn(FunctionCode& code) {
    Context c;
    c.fn = &code;
    c.varNames = &code.varNames;
    ctx_.push_back(c);
    auto e = parseAssignment(false);
    ctx_.pop_back();
    return e;
  }

  void parseFunctionBody(FunctionCode& code) {
    Context c;
    c.fn = &code;
    c.varNames = &code.varNames;
    ctx_.push_back(c);
    auto body = node(N::Block, cur());
    expect(Tok::LBrace);
    while (cur().type != Tok::RBrace) {
      if (cur().type == Tok::End) unexpected();
      body->kids.push_back(parseStatement());
    }
    code.sourceEnd = cur().end;
    next();
    markLexical(*body);
    code.body = std::move(body);
    ctx_.pop_back();
  }

  bool arrowAhead() const {
    const Token& t = cur();
    if (t.type == Tok::Name && !isReserved(t.text)) {
      return ahead().type == Tok::Arrow && !ahead().newlineBefore;
    }
    if (t.type == Tok::LParen) {
      const size_t close = match_[at_];
      if (close == (size_t) -1 || close + 1 >= toks_->size()) return false;
      const Token& after = (*toks_)[close + 1];
      return after.type == Tok::Arrow && !after.newlineBefore;
    }
    return false;
  }

  std::unique_ptr<Node> parseArrow() {
    const Token& start = cur();
    auto n = node(N::Func, start);
    auto code = std::unique_ptr<FunctionCode>(new FunctionCode());
    code->isArrow = true;
    code->line = start.line;
    code->sourceStart = start.start;
    if (cur().type == Tok::Name) {
      code->params.push_back(bindingName());
      code->defaults.push_back(nullptr);
    } else {
      parseParams(*code);
    }
    expect(Tok::Arrow);
    if (cur().type == Tok::LBrace) {
      parseFunctionBody(*code);
    } else {
      Context c;
      c.fn = code.get();
      c.varNames = &code->varNames;
      ctx_.push_back(c);
      code->isExpressionBody = true;
      code->body = parseAssignment(false);
      ctx_.pop_back();
      code->sourceEnd = (*toks_)[at_ > 0 ? at_ - 1 : 0].end;
    }
    // An arrow's `arguments` is its enclosing function's.
    if (code->usesArguments) markArguments();
    n->fn = std::move(code);
    ++closures_;
    return n;
  }

  void markArguments() {
    for (size_t i = ctx_.size(); i-- > 0;) {
      FunctionCode* f = ctx_[i].fn;
      if (!f) return;
      f->usesArguments = true;
      if (!f->isArrow) return;
    }
  }

  // ── Expressions ───────────────────────────────────────────────────────────

  static bool isTarget(const Node& n) {
    return n.kind == N::Ident || n.kind == N::Member || n.kind == N::Index;
  }

  std::unique_ptr<Node> parseExpression(bool noIn) {
    auto first = parseAssignment(noIn);
    if (cur().type != Tok::Comma) return first;
    auto seq = node(N::Sequence, cur());
    seq->line = first->line;
    seq->col = first->col;
    seq->kids.push_back(std::move(first));
    while (cur().type == Tok::Comma) {
      next();
      seq->kids.push_back(parseAssignment(noIn));
    }
    return seq;
  }

  static bool isAssignOp(Tok t) {
    return t >= Tok::Assign && t <= Tok::NullishAssign;
  }

  std::unique_ptr<Node> parseAssignment(bool noIn) {
    guard();
    if (arrowAhead()) return parseArrow();
    auto left = parseConditional(noIn);
    if (isAssignOp(cur().type)) {
      if (!isTarget(*left)) failAt(cur(), "Invalid left-hand side in assignment");
      auto n = node(N::Assign, cur());
      n->op = cur().type;
      n->line = left->line;
      n->col = left->col;
      next();
      n->kids.push_back(std::move(left));
      n->kids.push_back(parseAssignment(noIn));
      return n;
    }
    return left;
  }

  std::unique_ptr<Node> parseConditional(bool noIn) {
    auto test = parseBinary(1, noIn);
    if (cur().type != Tok::Question) return test;
    auto n = node(N::Conditional, cur());
    n->line = test->line;
    n->col = test->col;
    next();
    n->kids.push_back(std::move(test));
    n->kids.push_back(parseAssignment(false));
    expect(Tok::Colon);
    n->kids.push_back(parseAssignment(noIn));
    return n;
  }

  static int precedence(const Token& t, bool noIn) {
    switch (t.type) {
      case Tok::Nullish: return 1;
      case Tok::LogOr: return 2;
      case Tok::LogAnd: return 3;
      case Tok::BitOr: return 4;
      case Tok::BitXor: return 5;
      case Tok::BitAnd: return 6;
      case Tok::Eq: case Tok::Ne: case Tok::StrictEq: case Tok::StrictNe: return 7;
      case Tok::Lt: case Tok::Gt: case Tok::Le: case Tok::Ge: return 8;
      case Tok::Shl: case Tok::Shr: case Tok::UShr: return 9;
      case Tok::Add: case Tok::Sub: return 10;
      case Tok::Mul: case Tok::Div: case Tok::Mod: return 11;
      case Tok::Exp: return 12;
      case Tok::Name:
        if (t.text == "instanceof") return 8;
        if (t.text == "in" && !noIn) return 8;
        return 0;
      default: return 0;
    }
  }

  std::unique_ptr<Node> parseBinary(int minPrec, bool noIn) {
    guard();
    auto left = parseUnary();
    for (;;) {
      const Token& t = cur();
      const int prec = precedence(t, noIn);
      if (prec == 0 || prec < minPrec) return left;
      Tok op = t.type;
      if (op == Tok::Name) op = t.text == "in" ? Tok::In : Tok::Instanceof;
      if (op == Tok::Exp && left->kind == N::Unary && !(left->flags & kParen))
        failAt(t, "Unary operator used immediately before exponentiation expression. "
                  "Parenthesis must be used to disambiguate operator precedence");
      const bool logical = op == Tok::LogAnd || op == Tok::LogOr || op == Tok::Nullish;
      auto n = node(logical ? N::Logical : N::Binary, t);
      n->op = op;
      n->line = left->line;
      n->col = left->col;
      next();
      // ** is right-associative; everything else here is left-associative,
      // built iteratively by this loop.
      auto right = parseBinary(op == Tok::Exp ? prec : prec + 1, noIn);
      n->kids.push_back(std::move(left));
      n->kids.push_back(std::move(right));
      left = std::move(n);
    }
  }

  std::unique_ptr<Node> parseUnary() {
    guard();
    const Token& t = cur();
    Tok op = Tok::End;
    switch (t.type) {
      case Tok::Not: op = Tok::Not; break;
      case Tok::BitNot: op = Tok::BitNot; break;
      case Tok::Add: op = Tok::Pos; break;
      case Tok::Sub: op = Tok::Neg; break;
      case Tok::Inc:
      case Tok::Dec: {
        auto n = node(N::Update, t);
        n->op = t.type;
        n->flags |= kPrefix;
        next();
        auto target = parseUnary();
        if (!isTarget(*target))
          failAt(t, "Invalid left-hand side expression in prefix operation");
        n->kids.push_back(std::move(target));
        return n;
      }
      case Tok::Name:
        if (t.text == "typeof") op = Tok::Typeof;
        else if (t.text == "void") op = Tok::Void;
        else if (t.text == "delete") op = Tok::Delete;
        else if (t.text == "await") failAt(t, "async functions are not supported");
        break;
      default: break;
    }
    if (op != Tok::End) {
      auto n = node(N::Unary, t);
      n->op = op;
      next();
      n->kids.push_back(parseUnary());
      return n;
    }
    auto e = parseLeftHandSide();
    const Token& p = cur();
    if ((p.type == Tok::Inc || p.type == Tok::Dec) && !p.newlineBefore) {
      if (!isTarget(*e)) failAt(p, "Invalid left-hand side expression in postfix operation");
      auto n = node(N::Update, p);
      n->op = p.type;
      n->line = e->line;
      n->col = e->col;
      next();
      n->kids.push_back(std::move(e));
      return n;
    }
    return e;
  }

  void parseArguments(Node& call) {
    expect(Tok::LParen);
    while (cur().type != Tok::RParen) {
      if (cur().type == Tok::Ellipsis) {
        auto s = node(N::Spread, cur());
        next();
        s->kids.push_back(parseAssignment(false));
        call.kids.push_back(std::move(s));
      } else {
        call.kids.push_back(parseAssignment(false));
      }
      if (cur().type != Tok::Comma) break;
      next();
    }
    expect(Tok::RParen);
  }

  std::string propertyName() {
    // After a dot any identifier is a property name, keywords included.
    const Token& t = cur();
    if (t.type != Tok::Name) unexpected();
    std::string name = t.text;
    next();
    return name;
  }

  std::unique_ptr<Node> parseNew() {
    guard();
    const Token& t = cur();
    next(); // new
    if (cur().type == Tok::Dot) failAt(cur(), "new.target is not supported");
    auto n = node(N::New, t);
    std::unique_ptr<Node> callee = isName("new") ? parseNew() : parsePrimary();
    for (;;) {
      if (cur().type == Tok::Dot) {
        auto m = node(N::Member, cur());
        next();
        m->str = propertyName();
        m->kids.push_back(std::move(callee));
        callee = std::move(m);
      } else if (cur().type == Tok::LBracket) {
        auto m = node(N::Index, cur());
        next();
        m->kids.push_back(std::move(callee));
        m->kids.push_back(parseExpression(false));
        expect(Tok::RBracket);
        callee = std::move(m);
      } else {
        break;
      }
    }
    n->kids.push_back(std::move(callee));
    if (cur().type == Tok::LParen) parseArguments(*n);
    return n;
  }

  std::unique_ptr<Node> parseLeftHandSide() {
    guard();
    std::unique_ptr<Node> e = isName("new") ? parseNew() : parsePrimary();
    bool optional = false;
    for (;;) {
      const Token& t = cur();
      if (t.type == Tok::Dot) {
        auto m = node(N::Member, t);
        next();
        m->str = propertyName();
        m->kids.push_back(std::move(e));
        e = std::move(m);
      } else if (t.type == Tok::QuestionDot) {
        optional = true;
        next();
        if (cur().type == Tok::LParen) {
          auto c = node(N::Call, t);
          c->flags |= kOptional;
          c->kids.push_back(std::move(e));
          parseArguments(*c);
          e = std::move(c);
        } else if (cur().type == Tok::LBracket) {
          auto m = node(N::Index, t);
          m->flags |= kOptional;
          next();
          m->kids.push_back(std::move(e));
          m->kids.push_back(parseExpression(false));
          expect(Tok::RBracket);
          e = std::move(m);
        } else {
          auto m = node(N::Member, t);
          m->flags |= kOptional;
          m->str = propertyName();
          m->kids.push_back(std::move(e));
          e = std::move(m);
        }
      } else if (t.type == Tok::LBracket) {
        auto m = node(N::Index, t);
        next();
        m->kids.push_back(std::move(e));
        m->kids.push_back(parseExpression(false));
        expect(Tok::RBracket);
        e = std::move(m);
      } else if (t.type == Tok::LParen) {
        auto c = node(N::Call, t);
        c->kids.push_back(std::move(e));
        parseArguments(*c);
        e = std::move(c);
      } else if (t.type == Tok::Template) {
        failAt(t, "tagged templates are not supported");
      } else {
        break;
      }
    }
    if (optional) {
      auto chain = node(N::OptChain, cur());
      chain->line = e->line;
      chain->col = e->col;
      chain->kids.push_back(std::move(e));
      return chain;
    }
    return e;
  }

  std::unique_ptr<Node> parsePrimary() {
    guard();
    const Token& t = cur();
    switch (t.type) {
      case Tok::Num: {
        auto n = node(N::Num, t);
        n->num = t.number;
        next();
        return n;
      }
      case Tok::Str: {
        auto n = node(N::Str, t);
        n->str = t.text;
        n->value = JsVar(t.text);
        next();
        return n;
      }
      case Tok::Template: {
        auto n = node(N::Template, t);
        n->strs = t.tpl->parts;
        const std::vector<Token>* saved = toks_;
        const size_t savedAt = at_;
        std::vector<size_t> savedMatch = std::move(match_);
        for (const std::vector<Token>& expr : t.tpl->exprs) {
          setTokens(expr);
          if (cur().type == Tok::End) failAt(cur(), "Unexpected token '\x7d'");
          n->kids.push_back(parseExpression(false));
          if (cur().type != Tok::End) unexpected();
        }
        toks_ = saved;
        at_ = savedAt;
        match_ = std::move(savedMatch);
        next();
        return n;
      }
      case Tok::LParen: {
        next();
        auto e = parseExpression(false);
        expect(Tok::RParen);
        e->flags |= kParen;
        return e;
      }
      case Tok::LBracket: return parseArrayLiteral();
      case Tok::LBrace: return parseObjectLiteral();
      case Tok::Name: break;
      default: unexpected();
    }
    const std::string& w = t.text;
    if (w == "function") return parseFunction(false);
    if (w == "this") {
      auto n = node(N::This, t);
      next();
      return n;
    }
    if (w == "null") {
      auto n = node(N::Null, t);
      next();
      return n;
    }
    if (w == "true" || w == "false") {
      auto n = node(w == "true" ? N::True : N::False, t);
      next();
      return n;
    }
    if (w == "class") failAt(t, "classes are not supported");
    if (w == "async" && ahead().type == Tok::Name && ahead().text == "function")
      failAt(t, "async functions are not supported");
    if (isReserved(w)) unexpected();
    auto n = node(N::Ident, t);
    n->str = w;
    if (w == "arguments") markArguments();
    next();
    return n;
  }

  std::unique_ptr<Node> parseArrayLiteral() {
    auto n = node(N::ArrayLit, cur());
    next();
    while (cur().type != Tok::RBracket) {
      if (cur().type == Tok::Comma) {
        n->kids.push_back(node(N::Hole, cur()));
        next();
        continue;
      }
      if (cur().type == Tok::Ellipsis) {
        auto s = node(N::Spread, cur());
        next();
        s->kids.push_back(parseAssignment(false));
        n->kids.push_back(std::move(s));
      } else {
        n->kids.push_back(parseAssignment(false));
      }
      if (cur().type != Tok::Comma) break;
      next();
    }
    expect(Tok::RBracket);
    return n;
  }

  std::unique_ptr<Node> parseObjectLiteral() {
    auto n = node(N::ObjectLit, cur());
    next();
    while (cur().type != Tok::RBrace) {
      const Token& t = cur();
      if (t.type == Tok::Ellipsis) {
        auto s = node(N::Spread, t);
        next();
        s->kids.push_back(parseAssignment(false));
        n->kids.push_back(std::move(s));
      } else {
        auto p = node(N::Prop, t);
        bool shorthandAllowed = false;
        if (t.type == Tok::Name) {
          if ((t.text == "get" || t.text == "set") &&
              (ahead().type == Tok::Name || ahead().type == Tok::Str ||
               ahead().type == Tok::Num || ahead().type == Tok::LBracket))
            failAt(t, "getters and setters are not supported");
          if (t.text == "async" && ahead().type == Tok::Name)
            failAt(t, "async functions are not supported");
          p->str = t.text;
          shorthandAllowed = !isReserved(t.text);
          next();
        } else if (t.type == Tok::Str) {
          p->str = t.text;
          next();
        } else if (t.type == Tok::Num) {
          p->str = numberToString(t.number);
          next();
        } else if (t.type == Tok::LBracket) {
          next();
          p->flags |= kComputed;
          p->kids.push_back(parseAssignment(false));
          expect(Tok::RBracket);
        } else if (t.type == Tok::Mul) {
          failAt(t, "generators are not supported");
        } else {
          unexpected();
        }
        if (cur().type == Tok::Colon) {
          next();
          p->kids.push_back(parseAssignment(false));
        } else if (cur().type == Tok::LParen) {
          // A method: `name(params) { body }`.
          auto f = node(N::Func, t);
          auto code = std::unique_ptr<FunctionCode>(new FunctionCode());
          code->line = t.line;
          code->sourceStart = t.start;
          code->name = (p->flags & kComputed) ? std::string() : p->str;
          parseParams(*code);
          parseFunctionBody(*code);
          f->fn = std::move(code);
          ++closures_;
          p->kids.push_back(std::move(f));
        } else if (shorthandAllowed && (cur().type == Tok::Comma || cur().type == Tok::RBrace)) {
          auto id = node(N::Ident, t);
          id->str = p->str;
          if (id->str == "arguments") markArguments();
          p->kids.push_back(std::move(id));
        } else {
          unexpected();
        }
        n->kids.push_back(std::move(p));
      }
      if (cur().type != Tok::Comma) break;
      next();
    }
    expect(Tok::RBrace);
    return n;
  }
};

} // namespace js
} // namespace detail

namespace detail {
namespace js {

/** A JavaScript exception in flight: what a script's try/catch catches. */
struct Throw {
  JsVar value;
  int line;
  int col;
};

/** A limit was hit -- time, stop(), stack, memory. Not catchable by script:
 *  a limit a script could catch is a limit it could retry forever. */
struct Abort {
  std::string message;
  int line;
  int col;
};

/** An optional chain met null or undefined: the whole chain is undefined. */
struct ShortCircuit {};

enum class Flow : uint8_t { Normal, Break, Continue, Return };

using ScopePtr = std::shared_ptr<Scope>;

/** The engine running on this thread, for built-in functions to reach. A
 *  built-in called with no engine running (a function value carried out to
 *  C++ and called there) does nothing rather than reaching a dead one. */
inline Interp*& currentInterp() noexcept {
  static thread_local Interp* current = nullptr;
  return current;
}

/** Arguments to a built-in, with undefined past the end. */
struct Args {
  const JsVar* v;
  int n;
  const JsVar& operator[](int i) const noexcept {
    static const JsVar undef;
    return (i >= 0 && i < n) ? v[i] : undef;
  }
};

class Interp {
public:
  using Kind = JsVar::Kind;
  using Builtin = JsVar (*)(Interp&, const JsVar&, const Args&);

  // ── Limits: the engine copies its settings here at each outermost entry ──
  double timeLimit = 15.0;
  int maxDepth = 1000;
  size_t maxStack = (size_t) 4 << 20;
  long long maxMemory = 64ll << 20;
  size_t maxString = (size_t) 1 << 23;
  size_t maxArray = (size_t) 1 << 20;
  std::function<void(const std::string&)> consoleHook;
  std::atomic<bool> stopFlag{false};

  std::shared_ptr<Meter> meter_ = std::make_shared<Meter>();
  std::shared_ptr<DynamicObject> global_;
  ScopePtr globalScope_;
  std::shared_ptr<DynamicObject> objectProto_, functionProto_, arrayProto_, stringProto_,
      numberProto_, booleanProto_, errorProto_, typeErrorProto_, rangeErrorProto_,
      referenceErrorProto_, syntaxErrorProto_, evalErrorProto_;

  Interp() {
    std::random_device device;
    rng_[0] = ((uint64_t) device() << 32) ^ device() ^ 0x9E3779B97F4A7C15ull;
    rng_[1] = ((uint64_t) device() << 32) ^ device() ^ 0xD1B54A32D192ED03ull;
    for (int c = 0; c < 128; ++c) charCache_[c] = JsVar(std::string(1, (char) c));
    installBuiltins();
  }

  ~Interp() {
    // Let go of the roots, then collect: whatever scripts made that nothing
    // outside the engine still holds is freed now, cycles included. What a
    // plugin kept (a value returned from evaluate, the root object) stays.
    global_.reset();
    globalScope_.reset();
    objectProto_.reset();
    functionProto_.reset();
    arrayProto_.reset();
    stringProto_.reset();
    numberProto_.reset();
    booleanProto_.reset();
    errorProto_.reset();
    typeErrorProto_.reset();
    rangeErrorProto_.reset();
    referenceErrorProto_.reset();
    syntaxErrorProto_.reset();
    evalErrorProto_.reset();
    returnValue_ = JsVar();
    completion_ = JsVar();
    try {
      collect();
    } catch (...) {
    }
  }

  /** Marks the outermost entry into the engine: the stack is measured from
   *  here, the clock starts, and a previous stop() is forgotten. */
  struct Entry {
    Interp& I;
    Interp* previous;
    explicit Entry(Interp& interp) : I(interp), previous(currentInterp()) {
      currentInterp() = &I;
      if (I.entries_++ == 0) {
        I.stack_.base = stackPosition();
        I.stack_.limit = stackBudget(I.stack_.base, I.maxStack);
        I.depth_ = 0;
        I.ticks_ = 0;
        I.stopFlag.store(false);
        I.hasDeadline_ = I.timeLimit > 0 && I.timeLimit == I.timeLimit;
        if (I.hasDeadline_) {
          const double seconds = (std::min)(I.timeLimit, 1.0e7);
          I.deadline_ = std::chrono::steady_clock::now() +
                        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                            std::chrono::duration<double>(seconds));
        }
      }
    }
    ~Entry() {
      --I.entries_;
      currentInterp() = previous;
    }
    Entry(const Entry&) = delete;
    Entry& operator=(const Entry&) = delete;
  };

  bool running() const { return entries_ > 0; }

  // ── Limits ───────────────────────────────────────────────────────────────

  [[noreturn]] SONORE_JS_NOINLINE void abort(const std::string& message) { throw Abort{message, line_, col_}; }

  void checkStack() {
    if (stack_.exceeded()) abort("Maximum call stack size exceeded");
  }

  /** Every loop iteration and every call. */
  void tick() {
    if (stopFlag.load(std::memory_order_relaxed)) abort("Interrupted");
    if (++ticks_ >= 256) {
      ticks_ = 0;
      if (hasDeadline_ && std::chrono::steady_clock::now() >= deadline_)
        abort("Execution timed-out");
    }
    if (meter_->bytes.load(std::memory_order_relaxed) > maxMemory) memoryPressure(0);
  }

  /** Inside a built-in's own long loop, every few thousand steps: stop() and
   *  the time limit reach work that runs no script, which tick() (counted
   *  per script step) would otherwise never see. The clock is read on every
   *  call, so callers space them out. */
  void interruptPoint() {
    if (stopFlag.load(std::memory_order_relaxed)) abort("Interrupted");
    if (hasDeadline_ && std::chrono::steady_clock::now() >= deadline_) abort("Execution timed-out");
  }

  void memoryPressure(size_t wanted) {
    collect();
    if (meter_->bytes.load() + (long long) wanted > maxMemory)
      abort("Out of memory: the script exceeded maximumMemoryBytes");
  }

  /** Before allocating `bytes` at once. */
  void ensureRoom(size_t bytes) {
    if ((long long) bytes > maxMemory || meter_->bytes.load() + (long long) bytes > maxMemory)
      memoryPressure(bytes);
  }

  /** Before making a string of `units` bytes. */
  void checkLength(size_t bytes) {
    if (bytes > maxString) throwRange("Invalid string length");
    if (bytes > 4096) ensureRoom(bytes);
  }

  // ── The collector ────────────────────────────────────────────────────────

  void track(const std::shared_ptr<GcNode>& node) {
    if (node->gcTracked) return;
    node->gcTracked = true;
    registry_.push_back(node);
    if (registry_.size() >= registryLimit_) maintain();
  }

  void prune() {
    registry_.erase(std::remove_if(registry_.begin(), registry_.end(),
                                   [](const std::weak_ptr<GcNode>& w) { return w.expired(); }),
                    registry_.end());
  }

  void maintain() {
    prune();
    if (registry_.size() * 2 >= registryLimit_) collect();
    registryLimit_ = (std::max)((size_t) 4096, registry_.size() * 2);
  }

  /** Trial deletion: a node whose use count is fully explained by references
   *  from other tracked nodes is held from nowhere else; whatever nothing
   *  held from outside can reach is garbage, and gives up its references. */
  void collect() {
    prune();
    std::vector<std::shared_ptr<GcNode>> nodes;
    nodes.reserve(registry_.size());
    for (const auto& w : registry_)
      if (auto p = w.lock()) nodes.push_back(std::move(p));
    std::unordered_map<const GcNode*, size_t> index;
    index.reserve(nodes.size() * 2);
    for (size_t i = 0; i < nodes.size(); ++i) index[nodes[i].get()] = i;
    std::vector<long> internal(nodes.size(), 0);
    std::vector<const GcNode*> refs;
    for (size_t i = 0; i < nodes.size(); ++i) {
      refs.clear();
      nodes[i]->gcVisit(refs);
      for (const GcNode* r : refs) {
        auto it = index.find(r);
        if (it != index.end()) ++internal[it->second];
      }
    }
    std::vector<char> live(nodes.size(), 0);
    std::vector<size_t> work;
    for (size_t i = 0; i < nodes.size(); ++i)
      if (nodes[i].use_count() - 1 > internal[i]) {
        live[i] = 1;
        work.push_back(i);
      }
    while (!work.empty()) {
      const size_t i = work.back();
      work.pop_back();
      refs.clear();
      nodes[i]->gcVisit(refs);
      for (const GcNode* r : refs) {
        auto it = index.find(r);
        if (it != index.end() && !live[it->second]) {
          live[it->second] = 1;
          work.push_back(it->second);
        }
      }
    }
    std::vector<std::shared_ptr<GcNode>> trash;
    for (size_t i = 0; i < nodes.size(); ++i)
      if (!live[i]) nodes[i]->gcDrop(trash);
    nodes.clear();
    releaseIteratively(trash);
    prune();
  }

  // ── Allocation ───────────────────────────────────────────────────────────

  /** A string from canonical text. */
  JsVar str(std::string text) {
    if (text.size() > maxString) throwRange("Invalid string length");
    if (text.size() == 1 && (unsigned char) text[0] < 128) return charCache_[(unsigned char) text[0]];
    // Every string is measured against the limit as it is made: a built-in
    // that makes a million of them in one call (Object.keys of a big array)
    // must not be able to finish first and be measured after.
    ensureRoom(text.size() + sizeof(StringRep) + 48);
    return Access::fromRep(makeRep(std::move(text), meter_));
  }
  JsVar strUnits(const char16_t* units, size_t n) { return str(fromUtf16(units, n)); }

  void adopt(const std::shared_ptr<DynamicObject>& o) {
    if (!o->meter_) {
      o->meter_ = meter_;
      o->charged_ = 0;
    }
    o->recharge();
    track(o);
  }

  std::shared_ptr<DynamicObject> newObject() {
    auto o = std::make_shared<DynamicObject>();
    adopt(o);
    return o;
  }
  std::shared_ptr<DynamicObject> newObjectWithProto(const std::shared_ptr<DynamicObject>& proto) {
    auto o = std::make_shared<DynamicObject>();
    o->explicitProto_ = true;
    o->proto_ = proto;
    adopt(o);
    return o;
  }
  std::shared_ptr<ArrayObject> newArray() {
    auto a = std::make_shared<ArrayObject>();
    adopt(a);
    return a;
  }
  JsVar arrayOf(std::vector<JsVar> items) {
    if (items.size() > maxArray) throwRange("Invalid array length");
    ensureRoom(items.size() * sizeof(JsVar));
    auto a = newArray();
    a->elements = std::move(items);
    a->touch();
    return Access::fromObject(a);
  }
  std::shared_ptr<FunctionObject> newNative(const std::string& name, int arity,
                                            JsVar::NativeFunction fn) {
    auto f = std::make_shared<FunctionObject>();
    f->type = FunctionObject::Type::Native;
    f->native = std::move(fn);
    f->name = name;
    f->arity = arity;
    adopt(f);
    return f;
  }
  ScopePtr newScope(const ScopePtr& parent) {
    auto s = std::make_shared<Scope>();
    s->parent = parent;
    s->meter = meter_;
    s->recharge();
    return s;
  }
  void trackScopes(const ScopePtr& s) {
    for (ScopePtr p = s; p && !p->gcTracked; p = p->parent) track(p);
  }

  // ── Errors ───────────────────────────────────────────────────────────────

  JsVar makeError(const std::shared_ptr<DynamicObject>& proto, const std::string& message) {
    auto e = newObjectWithProto(proto);
    e->put("message", str(message), 0);
    return Access::fromObject(e);
  }
  [[noreturn]] SONORE_JS_NOINLINE void throwError(const std::shared_ptr<DynamicObject>& proto,
                               const std::string& message) {
    JsVar e = makeError(proto, message);
    throw Throw{e, line_, col_};
  }
  [[noreturn]] void throwType(const std::string& m) { throwError(typeErrorProto_, m); }
  [[noreturn]] void throwRange(const std::string& m) { throwError(rangeErrorProto_, m); }
  [[noreturn]] void throwReference(const std::string& m) { throwError(referenceErrorProto_, m); }
  [[noreturn]] void throwSyntax(const std::string& m) { throwError(syntaxErrorProto_, m); }
  void at(const Node* n) {
    line_ = n->line;
    col_ = n->col;
  }

  // ── Conversions ──────────────────────────────────────────────────────────

  static Kind kind(const JsVar& v) { return Access::kind(v); }
  static double num(const JsVar& v) { return Access::num(v); }
  static bool isObj(const JsVar& v) { return Access::kind(v) >= Kind::Array; }

  JsVar toPrimitive(const JsVar& v, int hint) {
    if (!isObj(v)) return v;
    const char* order[2] = {"valueOf", "toString"};
    if (hint == 2) std::swap(order[0], order[1]);
    for (const char* name : order) {
      JsVar m = getProp(v, name);
      if (m.isFunction()) {
        JsVar r = call(m, v, nullptr, 0);
        if (!isObj(r)) return r;
      }
    }
    throwType("Cannot convert object to primitive value");
  }

  double toNumber(const JsVar& v) {
    if (kind(v) == Kind::Number) return num(v);
    if (!isObj(v)) return v.toDouble();
    return toNumber(toPrimitive(v, 1));
  }

  std::string toText(const JsVar& v) {
    switch (kind(v)) {
      case Kind::String: return Access::str(v)->text;
      case Kind::Number: return numberToString(num(v));
      case Kind::Boolean: return num(v) != 0 ? "true" : "false";
      case Kind::Undefined: return "undefined";
      case Kind::Null: return "null";
      default: return toText(toPrimitive(v, 2));
    }
  }

  JsVar toStr(const JsVar& v) {
    if (kind(v) == Kind::String) return v;
    return str(toText(v));
  }

  std::string keyOf(const JsVar& v) {
    if (kind(v) == Kind::String) return Access::str(v)->text;
    if (kind(v) == Kind::Number) return numberToString(num(v));
    return toText(isObj(v) ? toPrimitive(v, 2) : v);
  }

  static double jsPow(double x, double y) {
    // C's pow says pow(1, NaN) and pow(+-1, +-Infinity) are 1; JavaScript
    // says NaN.
    if (y != y) return std::numeric_limits<double>::quiet_NaN();
    if (std::isinf(y) && std::fabs(x) == 1.0) return std::numeric_limits<double>::quiet_NaN();
    return std::pow(x, y);
  }

  static bool strictEquals(const JsVar& a, const JsVar& b) {
    if (kind(a) != kind(b)) return false;
    switch (kind(a)) {
      case Kind::Undefined:
      case Kind::Null: return true;
      case Kind::Boolean:
      case Kind::Number: return num(a) == num(b);
      case Kind::String: return Access::str(a)->text == Access::str(b)->text;
      default: return Access::obj(a) == Access::obj(b);
    }
  }

  bool looseEquals(const JsVar& a, const JsVar& b) {
    for (int guard = 0; guard < 8; ++guard) {
      const Kind ka = kind(a), kb = kind(b);
      if (ka == kb || (isObj(a) && isObj(b))) return strictEquals(a, b);
      if (a.isVoid() && b.isVoid()) return true;
      if (a.isVoid() || b.isVoid()) return false;
      if (ka == Kind::Number && kb == Kind::String) return num(a) == b.toDouble();
      if (ka == Kind::String && kb == Kind::Number) return a.toDouble() == num(b);
      if (ka == Kind::Boolean) return looseEquals(JsVar(num(a)), b);
      if (kb == Kind::Boolean) return looseEquals(a, JsVar(num(b)));
      if (isObj(a) && !isObj(b)) return looseEquals(toPrimitive(a, 0), b);
      if (isObj(b) && !isObj(a)) return looseEquals(a, toPrimitive(b, 0));
      return false;
    }
    return false;
  }

  /** Code-unit order, as JavaScript compares strings. UTF-8 bytes compare in
   *  code-point order, which differs only for characters past U+FFFF against
   *  U+E000..U+FFFF -- so only then is it worth decoding. */
  static int compareText(const StringRep* a, const StringRep* b) {
    if (a->ascii || b->ascii || (a->units == a->text.size() && b->units == b->text.size())) {
      const int c = a->text.compare(b->text);
      return c < 0 ? -1 : c > 0 ? 1 : 0;
    }
    std::u16string ua, ub;
    toUtf16(a->text, ua);
    toUtf16(b->text, ub);
    const int c = ua.compare(ub);
    return c < 0 ? -1 : c > 0 ? 1 : 0;
  }

  // ── Strings ──────────────────────────────────────────────────────────────

  JsVar charAt(const StringRep* r, size_t i) {
    if (r->ascii) return charCache_[(unsigned char) r->text[i] & 0x7F];
    std::u16string u;
    toUtf16(r->text, u);
    return strUnits(&u[i], 1);
  }

  JsVar concat(const JsVar& a, const JsVar& b) {
    const StringRep* ra = Access::str(a);
    const StringRep* rb = Access::str(b);
    if (ra->text.empty()) return b;
    if (rb->text.empty()) return a;
    checkLength(ra->text.size() + rb->text.size());
    auto rep = std::make_shared<StringRep>();
    rep->text = concatText(ra->text, rb->text);
    rep->ascii = ra->ascii && rb->ascii;
    rep->units = ra->units + rb->units;
    rep->meter = meter_;
    rep->charged = (long long) (sizeof(StringRep) + rep->text.capacity() + 48);
    meter_->bytes += rep->charged;
    return Access::fromRep(std::move(rep));
  }

  /** Appends canonical text to canonical text, joining a split pair. */
  static void appendText(std::string& acc, const std::string& piece) {
    const size_t n = acc.size();
    if (n >= 3 && piece.size() >= 3 && (unsigned char) acc[n - 3] == 0xED &&
        ((unsigned char) acc[n - 2] & 0xF0) == 0xA0 && (unsigned char) piece[0] == 0xED &&
        ((unsigned char) piece[1] & 0xF0) == 0xB0) {
      std::string tail = acc.substr(n - 3);
      acc.resize(n - 3);
      acc += concatText(tail, piece);
      return;
    }
    acc += piece;
  }

  // ── Properties ───────────────────────────────────────────────────────────

  DynamicObject* protoOf(const DynamicObject* o) const {
    if (o->explicitProto_) return o->proto_.get();
    switch (o->cellKind_) {
      case Kind::Array: return arrayProto_.get();
      case Kind::Function: return functionProto_.get();
      default: return objectProto_.get();
    }
  }

  const DynamicObject::Property* chainFind(const DynamicObject* o, const std::string& key) const {
    for (int guard = 0; o && guard < 100000; ++guard) {
      if (const DynamicObject::Property* p = o->find(key)) return p;
      o = protoOf(o);
    }
    return nullptr;
  }

  JsVar chainLookup(const DynamicObject* o, const std::string& key) const {
    const DynamicObject::Property* p = chainFind(o, key);
    return p ? p->value : JsVar();
  }

  JsVar makePrototype(const JsVar& fn, FunctionObject* f) {
    auto proto = newObject();
    proto->put("constructor", fn, 0);
    f->put("prototype", Access::fromObject(proto), 0);
    return Access::fromObject(proto);
  }

  SONORE_JS_NOINLINE JsVar getProp(const JsVar& base, const std::string& key) {
    switch (kind(base)) {
      case Kind::Undefined:
      case Kind::Null:
        throwType(std::string("Cannot read properties of ") +
                  (base.isNull() ? "null" : "undefined") + " (reading '" + key + "')");
      case Kind::Boolean: return chainLookup(booleanProto_.get(), key);
      case Kind::Number: return chainLookup(numberProto_.get(), key);
      case Kind::String: {
        const StringRep* r = Access::str(base);
        if (key == "length") return JsVar((double) r->units);
        uint32_t i = 0;
        if (isIndexText(key, &i)) return i < r->units ? charAt(r, i) : JsVar();
        return chainLookup(stringProto_.get(), key);
      }
      case Kind::Array: {
        ArrayObject* a = static_cast<ArrayObject*>(Access::obj(base));
        if (key == "length") return JsVar((double) a->elements.size());
        uint32_t i = 0;
        if (isIndexText(key, &i)) return i < a->elements.size() ? a->elements[i] : JsVar();
        return chainLookup(a, key);
      }
      case Kind::Function: {
        FunctionObject* f = static_cast<FunctionObject*>(Access::obj(base));
        if (const DynamicObject::Property* p = f->find(key)) return p->value;
        if (key == "prototype" && f->type == FunctionObject::Type::Script && f->code &&
            !f->code->isArrow)
          return makePrototype(base, f);
        if (key == "name") return str(f->name);
        if (key == "length") return JsVar((double) f->arity);
        return chainLookup(protoOf(f), key);
      }
      default: return chainLookup(Access::obj(base), key);
    }
  }

  JsVar getIndex(const JsVar& base, const JsVar& key) {
    if (kind(key) == Kind::Number) {
      const double d = num(key);
      if (kind(base) == Kind::Array) {
        const auto& e = static_cast<ArrayObject*>(Access::obj(base))->elements;
        if (d >= 0 && d < (double) e.size() && d == std::floor(d)) return e[(size_t) d];
      } else if (kind(base) == Kind::String) {
        const StringRep* r = Access::str(base);
        if (d >= 0 && d < (double) r->units && d == std::floor(d)) return charAt(r, (size_t) d);
      }
    }
    return getProp(base, keyOf(key));
  }

  void ensureMetered(const JsVar& v) {
    DynamicObject* o = Access::obj(v);
    if (o && !o->meter_) adopt(Access::objPtr(v));
  }

  void assignOwn(DynamicObject* o, const std::string& key, const JsVar& value) {
    if (DynamicObject::Property* p = o->find(key)) {
      if (!(p->flags & DynamicObject::kReadOnly)) p->value = value;
      return;
    }
    // Every new property is measured: Object.assign or a loop of
    // assignments adds them without a script step in between.
    ensureRoom(key.size() * 3 + sizeof(DynamicObject::Property) + 64);
    o->put(key, value, DynamicObject::kEnumerable);
  }

  void growArray(ArrayObject* a, size_t n) {
    if (n > maxArray) throwRange("Invalid array length");
    if (n > a->elements.capacity())
      ensureRoom((n - a->elements.size()) * sizeof(JsVar) * 2);
    a->elements.resize(n);
    a->touch();
  }

  void push(ArrayObject* a, const JsVar& v) {
    if (a->elements.size() >= maxArray) throwRange("Invalid array length");
    const size_t capacity = a->elements.capacity();
    if (a->elements.size() == capacity) ensureRoom((capacity + 4) * sizeof(JsVar));
    a->elements.push_back(v);
    if (a->elements.capacity() != capacity) a->touch();
  }

  void setElement(ArrayObject* a, uint32_t i, const JsVar& v) {
    if (i < a->elements.size()) {
      a->elements[i] = v;
      return;
    }
    if ((size_t) i >= maxArray) throwRange("Invalid array length");
    growArray(a, (size_t) i + 1);
    a->elements[i] = v;
  }

  void setLength(ArrayObject* a, const JsVar& v) {
    const double d = toNumber(v);
    const uint32_t u = toUint32(d);
    if ((double) u != d) throwRange("Invalid array length");
    if (u > a->elements.size()) {
      growArray(a, u);
    } else {
      a->elements.resize(u);
      a->touch();
    }
  }

  SONORE_JS_NOINLINE void setProp(const JsVar& base, const std::string& key, const JsVar& value) {
    switch (kind(base)) {
      case Kind::Undefined:
      case Kind::Null:
        throwType(std::string("Cannot set properties of ") +
                  (base.isNull() ? "null" : "undefined") + " (setting '" + key + "')");
      case Kind::Boolean:
      case Kind::Number:
      case Kind::String: return; // sloppy mode: quietly nothing
      case Kind::Array: {
        ensureMetered(base);
        ArrayObject* a = static_cast<ArrayObject*>(Access::obj(base));
        if (key == "length") {
          setLength(a, value);
          return;
        }
        uint32_t i = 0;
        if (isIndexText(key, &i)) {
          setElement(a, i, value);
          return;
        }
        assignOwn(a, key, value);
        return;
      }
      default:
        ensureMetered(base);
        assignOwn(Access::obj(base), key, value);
    }
  }

  void setIndex(const JsVar& base, const JsVar& key, const JsVar& value) {
    if (kind(key) == Kind::Number && kind(base) == Kind::Array) {
      const double d = num(key);
      if (d >= 0 && d < 4294967295.0 && d == std::floor(d)) {
        ensureMetered(base);
        setElement(static_cast<ArrayObject*>(Access::obj(base)), (uint32_t) d, value);
        return;
      }
    }
    setProp(base, keyOf(key), value);
  }

  bool hasProperty(const JsVar& base, const std::string& key) {
    switch (kind(base)) {
      case Kind::Array: {
        ArrayObject* a = static_cast<ArrayObject*>(Access::obj(base));
        if (key == "length") return true;
        uint32_t i = 0;
        if (isIndexText(key, &i)) return i < a->elements.size();
        return chainFind(a, key) != nullptr;
      }
      case Kind::Function: {
        FunctionObject* f = static_cast<FunctionObject*>(Access::obj(base));
        if (key == "name" || key == "length") return true;
        if (key == "prototype" && f->type == FunctionObject::Type::Script && f->code &&
            !f->code->isArrow)
          return true;
        return chainFind(f, key) != nullptr;
      }
      case Kind::Object: return chainFind(Access::obj(base), key) != nullptr;
      default:
        throwType("Cannot use 'in' operator to search for '" + key + "' in " + toText(base));
    }
  }

  /** for-in's keys: own enumerable ones in order, then inherited ones not
   *  shadowed. Built-in prototypes have nothing enumerable.
   *
   *  A string's or an array's indices come first, and are not in the list:
   *  `*indices` says how many there are, and the loop makes each key as it
   *  reaches it. Made up front they were a std::string per character, which
   *  no meter saw -- for-in over a script's 8 MB string held 500 MB of keys
   *  under a 64 MB limit. */
  std::vector<std::string> forInKeys(const JsVar& v, size_t* indices) {
    std::vector<std::string> keys;
    *indices = 0;
    if (kind(v) == Kind::String) {
      *indices = Access::str(v)->units;
      return keys;
    }
    if (!isObj(v)) return keys;
    const DynamicObject* o = Access::obj(v);
    if (kind(v) == Kind::Array) *indices = static_cast<const ArrayObject*>(o)->elements.size();
    std::vector<std::string> own = o->keysInOrder(true);
    keys.insert(keys.end(), own.begin(), own.end());
    const DynamicObject* p = protoOf(o);
    std::unordered_set<std::string> seen;
    bool seenFilled = false;
    for (int guard = 0; p && guard < 100000; ++guard, p = protoOf(p)) {
      bool any = false;
      for (const auto& prop : p->props_)
        if (prop.flags & DynamicObject::kEnumerable) any = true;
      if (!any) continue;
      if (!seenFilled) {
        seen.insert(keys.begin(), keys.end());
        for (const auto& prop : o->props_) seen.insert(prop.key);
        seenFilled = true;
      }
      for (const std::string& k : p->keysInOrder(true)) {
        uint32_t i = 0;
        if (isIndexText(k, &i) && i < *indices) continue; // shadowed by an index
        if (seen.insert(k).second) keys.push_back(k);
      }
      for (const auto& prop : p->props_) seen.insert(prop.key);
    }
    return keys;
  }

  /** Own enumerable keys, for Object.keys / values / entries and spread. A
   *  string's or an array's index keys are charged before they are made,
   *  as everything else the engine allocates is: Object.keys of an 8 MB
   *  string built 256 MB of them before the result was refused. */
  std::vector<std::string> ownKeys(const JsVar& v) {
    std::vector<std::string> keys;
    if (kind(v) == Kind::String) {
      const size_t n = Access::str(v)->units;
      ensureRoom(n * sizeof(std::string));
      keys.reserve(n);
      for (size_t i = 0; i < n; ++i) keys.push_back(std::to_string(i));
      return keys;
    }
    if (!isObj(v)) return keys;
    const DynamicObject* o = Access::obj(v);
    if (kind(v) == Kind::Array) {
      const size_t n = static_cast<const ArrayObject*>(o)->elements.size();
      ensureRoom(n * sizeof(std::string));
      keys.reserve(n);
      for (size_t i = 0; i < n; ++i) keys.push_back(std::to_string(i));
    }
    std::vector<std::string> own = o->keysInOrder(true);
    keys.insert(keys.end(), own.begin(), own.end());
    return keys;
  }

  // ── Calls ────────────────────────────────────────────────────────────────

  struct DepthGuard {
    int& d;
    explicit DepthGuard(int& depth) : d(depth) { ++d; }
    ~DepthGuard() { --d; }
  };

  JsVar call(const JsVar& fn, const JsVar& thisValue, const JsVar* args, size_t n) {
    if (kind(fn) != Kind::Function) throwType(toText(fn) + " is not a function");
    FunctionObject* f = static_cast<FunctionObject*>(Access::obj(fn));
    switch (f->type) {
      case FunctionObject::Type::Script: return callScript(f, fn, thisValue, args, n);
      case FunctionObject::Type::Native: return callNative(f, thisValue, args, n);
      case FunctionObject::Type::Bound: return callBound(f, args, n);
    }
    return JsVar();
  }

  SONORE_JS_NOINLINE JsVar callBound(FunctionObject* f, const JsVar* args, size_t n) {
    checkStack();
    std::vector<JsVar> all(f->boundArgs);
    all.insert(all.end(), args, args + n);
    JsVar target = f->boundTarget, self = f->boundThis;
    return call(target, self, all.data(), all.size());
  }

  SONORE_JS_NOINLINE JsVar callNative(FunctionObject* f, const JsVar& thisValue, const JsVar* args, size_t n) {
    if (depth_ >= maxDepth) abort("Maximum call stack size exceeded");
    DepthGuard guard(depth_);
    checkStack();
    tick();
    JsVar::NativeFunctionArgs a(thisValue, args, (int) n);
    const int savedLine = line_, savedCol = col_;
    // Only the C++ exceptions a native may throw are caught here; the
    // engine's own (Throw, Abort) pass straight through. Catching them to
    // rethrow would cost stack at every level: MSVC runs a catch block, and
    // the rethrow's dispatch, BELOW the frames it is unwinding, and an Abort
    // from a thousand natives deep overflowed the stack on its way out.
    std::string failure;
    bool outOfMemory = false;
    try {
      JsVar::NativeFunction fn = f->native; // a copy: the function may be replaced while it runs
      JsVar r = fn(a);
      line_ = savedLine;
      col_ = savedCol;
      return r;
    } catch (const std::bad_alloc&) {
      outOfMemory = true;
    } catch (const std::exception& e) {
      failure = e.what();
    }
    // Outside the catch block, for the same reason.
    if (outOfMemory) abort("Out of memory");
    line_ = savedLine;
    col_ = savedCol;
    throwError(errorProto_, failure);
  }

  /** A call's scope: `this`, the parameters (defaults and rest included),
   *  `arguments` if the body uses it, and the body's var declarations. */
  SONORE_JS_NOINLINE ScopePtr enterFunction(FunctionObject* f, const JsVar& fnVar,
                                            const JsVar& thisValue, const JsVar* args, size_t n) {
    const FunctionCode& code = *f->code;
    ScopePtr scope = newScope(f->scope);
    scope->isFunction = true;
    scope->vars.reserve(code.params.size() + code.varNames.size() + 2);
    if (!code.isArrow) {
      scope->hasThis = true;
      // Sloppy mode: a plain call's `this` is the global object.
      scope->thisValue = thisValue.isVoid() ? Access::fromObject(global_) : thisValue;
      if (code.isNamedExpression) bind(*scope, code.name, fnVar);
    }
    for (size_t i = 0; i < code.params.size(); ++i) {
      JsVar value;
      if (code.hasRest && i + 1 == code.params.size()) {
        std::vector<JsVar> rest;
        if (n > i) rest.assign(args + i, args + n);
        value = arrayOf(std::move(rest));
      } else if (i < n) {
        value = args[i];
      }
      if (value.isUndefined() && code.defaults[i]) value = eval(code.defaults[i].get(), scope);
      bind(*scope, code.params[i], value);
    }
    if (code.usesArguments && !code.isArrow && !scope->find("arguments"))
      bind(*scope, "arguments", arrayOf(std::vector<JsVar>(args, args + n)));
    for (const std::string& v : code.varNames)
      if (!scope->find(v)) scope->vars.push_back(Binding{v, JsVar(), 0});
    scope->recharge();
    return scope;
  }

  SONORE_JS_NOINLINE JsVar callScript(FunctionObject* f, const JsVar& fnVar, const JsVar& thisValue,
                   const JsVar* args, size_t n) {
    if (depth_ >= maxDepth) abort("Maximum call stack size exceeded");
    DepthGuard guard(depth_);
    checkStack();
    tick();
    const FunctionCode& code = *f->code;
    const std::shared_ptr<const Program>* savedProgram = program_;
    program_ = &f->program;
    struct Restore {
      Interp& I;
      const std::shared_ptr<const Program>* p;
      int line, col;
      ~Restore() {
        I.program_ = p;
        I.line_ = line;
        I.col_ = col;
      }
    } restore{*this, savedProgram, line_, col_};

    ScopePtr scope = enterFunction(f, fnVar, thisValue, args, n);
    if (code.isExpressionBody) return eval(code.body.get(), scope);
    const Node* body = code.body.get();
    if (body->flags & kHasLexical) declareLexical(body->kids, 0, scope, false);
    const Flow flow = execList(body->kids, 0, scope);
    if (flow == Flow::Return) {
      JsVar r = std::move(returnValue_);
      returnValue_ = JsVar();
      return r;
    }
    return JsVar();
  }

  SONORE_JS_NOINLINE JsVar construct(const JsVar& fn, const JsVar* args, size_t n, const Node* site) {
    // A chain of bound functions unwraps here by recursion, so a script that
    // binds a constructor thousands of times must meet the stack limit rather
    // than overflow the native stack.
    checkStack();
    if (kind(fn) != Kind::Function) throwType(describe(site) + " is not a constructor");
    FunctionObject* f = static_cast<FunctionObject*>(Access::obj(fn));
    if (f->type == FunctionObject::Type::Bound) {
      std::vector<JsVar> all(f->boundArgs);
      all.insert(all.end(), args, args + n);
      JsVar target = f->boundTarget;
      return construct(target, all.data(), all.size(), site);
    }
    if (f->type == FunctionObject::Type::Script && f->code->isArrow)
      throwType(describe(site) + " is not a constructor");
    JsVar protoValue = getProp(fn, "prototype");
    auto obj = newObjectWithProto(isObj(protoValue) ? Access::objPtr(protoValue) : objectProto_);
    JsVar self = Access::fromObject(obj);
    JsVar r = call(fn, self, args, n);
    return isObj(r) ? r : self;
  }

  static std::string describe(const Node* n) {
    if (!n) return "expression";
    switch (n->kind) {
      case N::Ident: return n->str;
      case N::This: return "this";
      case N::Member: return describe(n->kid(0)) + "." + n->str;
      case N::Index: return describe(n->kid(0)) + "[...]";
      case N::Call: return describe(n->kid(0)) + "(...)";
      case N::Str: return "\"" + n->str + "\"";
      case N::Num: return numberToString(n->num);
      case N::OptChain: return describe(n->kid(0));
      default: return "expression";
    }
  }

  // ── Scopes ───────────────────────────────────────────────────────────────

  static void bind(Scope& s, const std::string& name, const JsVar& value, uint8_t flags = 0) {
    if (Binding* b = s.find(name)) {
      b->value = value;
      b->flags = flags;
      return;
    }
    s.vars.push_back(Binding{name, value, flags});
  }

  SONORE_JS_NOINLINE JsVar lookup(const std::string& name, Scope* s) {
    for (Scope* p = s; p; p = p->parent.get()) {
      if (Binding* b = p->find(name)) {
        if (b->flags & Binding::kUninitialised)
          throwReference("Cannot access '" + name + "' before initialization");
        return b->value;
      }
    }
    if (const DynamicObject::Property* p = global_->find(name)) return p->value;
    throwReference(name + " is not defined");
  }

  SONORE_JS_NOINLINE void assignVariable(const std::string& name, const JsVar& value, Scope* s) {
    for (Scope* p = s; p; p = p->parent.get()) {
      if (Binding* b = p->find(name)) {
        if (b->flags & Binding::kUninitialised)
          throwReference("Cannot access '" + name + "' before initialization");
        if (b->flags & Binding::kConst) throwType("Assignment to constant variable.");
        b->value = value;
        return;
      }
    }
    // Sloppy mode: assigning to an undeclared name makes a global.
    assignOwn(global_.get(), name, value);
  }

  JsVar thisOf(Scope* s) {
    for (Scope* p = s; p; p = p->parent.get())
      if (p->hasThis) return p->thisValue;
    return Access::fromObject(global_);
  }

  JsVar makeClosure(const Node* n, const ScopePtr& scope) {
    const FunctionCode* code = n->fn.get();
    auto f = std::make_shared<FunctionObject>();
    f->type = FunctionObject::Type::Script;
    f->code = code;
    if (program_) f->program = *program_;
    f->scope = scope;
    f->name = code->name;
    int arity = 0;
    for (size_t i = 0; i < code->params.size(); ++i) {
      if (code->defaults[i] || (code->hasRest && i + 1 == code->params.size())) break;
      ++arity;
    }
    f->arity = arity;
    adopt(f);
    trackScopes(scope);
    return Access::fromObject(f);
  }

  static void nameFunction(const JsVar& v, const Node* valueNode, const std::string& name) {
    if (valueNode && valueNode->kind == N::Func && kind(v) == Kind::Function) {
      FunctionObject* f = static_cast<FunctionObject*>(Access::obj(v));
      if (f->name.empty()) f->name = name;
    }
  }

  /** let, const and function declarations of a block, at its start: let and
   *  const exist but cannot be read until their declaration runs (the
   *  temporal dead zone); functions are ready at once. */
  void declareLexical(const std::vector<std::unique_ptr<Node>>& kids, size_t from,
                      const ScopePtr& scope, bool annexB) {
    for (size_t i = from; i < kids.size(); ++i) {
      const Node* k = kids[i].get();
      if (!k) continue;
      if (k->kind == N::VarDecl && k->decl != Decl::Var) {
        for (const auto& d : k->kids)
          bind(*scope, d->str, JsVar(),
               (uint8_t) (Binding::kUninitialised |
                          (k->decl == Decl::Const ? Binding::kConst : 0)));
      }
    }
    for (size_t i = from; i < kids.size(); ++i) {
      const Node* k = kids[i].get();
      if (!k || k->kind != N::FuncDecl) continue;
      JsVar fn = makeClosure(k, scope);
      bind(*scope, k->fn->name, fn);
      if (annexB && scope->parent) assignVariable(k->fn->name, fn, scope->parent.get());
    }
    scope->recharge();
  }

  ScopePtr copyScope(const ScopePtr& s) {
    ScopePtr c = newScope(s->parent);
    c->vars = s->vars;
    c->recharge();
    return c;
  }

  // ── Statements ───────────────────────────────────────────────────────────

  Flow execList(const std::vector<std::unique_ptr<Node>>& kids, size_t from,
                const ScopePtr& s) {
    for (size_t i = from; i < kids.size(); ++i) {
      const Flow f = exec(kids[i].get(), s);
      if (f != Flow::Normal) return f;
    }
    return Flow::Normal;
  }

  // exec() and eval() are the two functions every level of a script's
  // recursion passes through, so their frames are what a script's call depth
  // costs in native stack. Each keeps only the cheapest cases inline and
  // hands the rest to a function of its own that is never inlined back --
  // one switch holding every case's temporaries measured 2.3 KB a level.

  Flow exec(const Node* n, const ScopePtr& s) {
    checkStack();
    at(n);
    switch (n->kind) {
      case N::ExprStmt: return execExpression(n, s);
      case N::VarDecl: execVarDecl(n, s); return Flow::Normal;
      case N::FuncDecl:
      case N::Empty: return Flow::Normal;
      case N::Block: return execBlock(n, s);
      case N::If: return execIf(n, s);
      case N::For: return execFor(n, s);
      case N::ForIn:
      case N::ForOf: return execForInOf(n, s);
      case N::While:
      case N::DoWhile: return execWhile(n, s);
      case N::Break: return Flow::Break;
      case N::Continue: return Flow::Continue;
      case N::Return: return execReturn(n, s);
      case N::Throw: execThrow(n, s);
      case N::Try: return execTry(n, s);
      case N::Switch: return execSwitch(n, s);
      default: return execExpression(n, s);
    }
  }

  SONORE_JS_NOINLINE Flow execExpression(const Node* n, const ScopePtr& s) {
    JsVar v = eval(n->kind == N::ExprStmt ? n->kids[0].get() : n, s);
    if (depth_ == completionDepth_) completion_ = std::move(v);
    return Flow::Normal;
  }

  SONORE_JS_NOINLINE Flow execBlock(const Node* n, const ScopePtr& s) {
    if (n->flags & kHasLexical) {
      ScopePtr inner = newScope(s);
      declareLexical(n->kids, 0, inner, true);
      return execList(n->kids, 0, inner);
    }
    return execList(n->kids, 0, s);
  }

  SONORE_JS_NOINLINE Flow execIf(const Node* n, const ScopePtr& s) {
    if (eval(n->kids[0].get(), s).toBool()) return exec(n->kids[1].get(), s);
    if (n->kids.size() > 2) return exec(n->kids[2].get(), s);
    return Flow::Normal;
  }

  SONORE_JS_NOINLINE Flow execWhile(const Node* n, const ScopePtr& s) {
    const bool doWhile = n->kind == N::DoWhile;
    const Node* test = n->kids[doWhile ? 1 : 0].get();
    const Node* body = n->kids[doWhile ? 0 : 1].get();
    for (bool first = true;; first = false) {
      tick();
      if ((!doWhile || !first) && !eval(test, s).toBool()) break;
      const Flow f = exec(body, s);
      if (f == Flow::Break) break;
      if (f == Flow::Return) return f;
    }
    return Flow::Normal;
  }

  SONORE_JS_NOINLINE Flow execReturn(const Node* n, const ScopePtr& s) {
    returnValue_ = n->kids.empty() ? JsVar() : eval(n->kids[0].get(), s);
    return Flow::Return;
  }

  [[noreturn]] SONORE_JS_NOINLINE void execThrow(const Node* n, const ScopePtr& s) {
    JsVar v = eval(n->kids[0].get(), s);
    throw Throw{v, n->line, n->col};
  }

  SONORE_JS_NOINLINE void execVarDecl(const Node* n, const ScopePtr& s) {
    for (const auto& d : n->kids) {
      const Node* init = d->kid(0);
      if (n->decl == Decl::Var) {
        if (!init) continue;
        JsVar v = eval(init, s);
        nameFunction(v, init, d->str);
        at(d.get());
        assignVariable(d->str, v, s.get());
      } else {
        JsVar v = init ? eval(init, s) : JsVar();
        nameFunction(v, init, d->str);
        initialiseLexical(*s, d->str, v, n->decl == Decl::Const);
      }
    }
  }

  void initialiseLexical(Scope& s, const std::string& name, const JsVar& v, bool isConst) {
    Binding* b = s.find(name);
    if (!b) {
      s.vars.push_back(Binding{name, v, (uint8_t) (isConst ? Binding::kConst : 0)});
      s.recharge();
      return;
    }
    b->value = v;
    b->flags = (uint8_t) (isConst ? Binding::kConst : 0);
  }

  SONORE_JS_NOINLINE Flow execFor(const Node* n, const ScopePtr& s) {
    const Node* init = n->kid(0);
    const Node* test = n->kid(1);
    const Node* update = n->kid(2);
    const Node* body = n->kid(3);
    ScopePtr loop = s;
    const bool lexical = init && init->kind == N::VarDecl && init->decl != Decl::Var;
    if (lexical) {
      loop = newScope(s);
      for (const auto& d : init->kids)
        bind(*loop, d->str, JsVar(),
             (uint8_t) (Binding::kUninitialised |
                        (init->decl == Decl::Const ? Binding::kConst : 0)));
    }
    if (init) {
      if (init->kind == N::VarDecl) execVarDecl(init, loop);
      else eval(init, loop);
    }
    // A closure made in the body must see the i of ITS iteration, so each
    // iteration gets a copy of the loop's bindings -- but only when the body
    // makes a closure at all; otherwise the copy is invisible and is skipped.
    const bool perIteration = lexical && (n->flags & kPerIteration);
    ScopePtr iter = perIteration ? copyScope(loop) : loop;
    for (;;) {
      tick();
      if (test && !eval(test, iter).toBool()) break;
      const Flow f = exec(body, iter);
      if (f == Flow::Break) break;
      if (f == Flow::Return) return f;
      if (perIteration) iter = copyScope(iter);
      if (update) eval(update, iter);
    }
    return Flow::Normal;
  }

  void assignTo(const Node* target, const JsVar& value, const ScopePtr& s) {
    switch (target->kind) {
      case N::Ident: assignVariable(target->str, value, s.get()); return;
      case N::Member: {
        JsVar base = eval(target->kids[0].get(), s);
        at(target);
        setProp(base, target->str, value);
        return;
      }
      case N::Index: {
        JsVar base = eval(target->kids[0].get(), s);
        JsVar key = eval(target->kids[1].get(), s);
        at(target);
        setIndex(base, key, value);
        return;
      }
      default: throwSyntax("Invalid assignment target");
    }
  }

  SONORE_JS_NOINLINE Flow execForInOf(const Node* n, const ScopePtr& s) {
    const Node* left = n->kid(0);
    const bool isOf = n->kind == N::ForOf;
    JsVar subject = eval(n->kids[1].get(), s);
    const Node* body = n->kid(2);
    const bool declared = left->kind == N::VarDecl;
    const bool lexical = declared && left->decl != Decl::Var;
    const std::string* name = declared ? &left->kids[0]->str : nullptr;
    const bool perIteration = lexical && (n->flags & kPerIteration);
    ScopePtr shared = lexical && !perIteration ? newScope(s) : s;

    auto step = [&](const JsVar& value) -> Flow {
      tick();
      if (lexical) {
        ScopePtr iter = perIteration ? newScope(s) : shared;
        bind(*iter, *name, value, left->decl == Decl::Const ? Binding::kConst : 0);
        if (perIteration) iter->recharge();
        return exec(body, iter);
      }
      if (declared) assignVariable(*name, value, s.get());
      else assignTo(left, value, s);
      return exec(body, s);
    };

    if (!isOf) {
      if (subject.isVoid()) return Flow::Normal;
      size_t indices = 0;
      const std::vector<std::string> keys = forInKeys(subject, &indices);
      for (size_t turn = 0; turn < indices + keys.size(); ++turn) {
        const std::string k = turn < indices ? std::to_string(turn) : keys[turn - indices];
        // A property deleted before its turn is skipped, as the language says.
        if (isObj(subject) && !hasProperty(subject, k)) continue;
        const Flow f = step(str(k));
        if (f == Flow::Break) break;
        if (f == Flow::Return) return f;
      }
      return Flow::Normal;
    }
    if (kind(subject) == Kind::Array) {
      ArrayObject* a = static_cast<ArrayObject*>(Access::obj(subject));
      for (size_t i = 0; i < a->elements.size(); ++i) { // live: the body may push
        JsVar e = a->elements[i];
        const Flow f = step(e);
        if (f == Flow::Break) break;
        if (f == Flow::Return) return f;
      }
      return Flow::Normal;
    }
    if (kind(subject) == Kind::String) {
      const std::string text = Access::str(subject)->text;
      const unsigned char* p = (const unsigned char*) text.data();
      size_t i = 0;
      while (i < text.size()) {
        const size_t from = i;
        decodeCodePoint(p, text.size(), i);
        const Flow f = step(str(text.substr(from, i - from)));
        if (f == Flow::Break) break;
        if (f == Flow::Return) return f;
      }
      return Flow::Normal;
    }
    at(n->kids[1].get());
    throwType(describe(n->kid(1)) + " is not iterable");
  }

  Flow runCatch(const Node* n, const JsVar& error, const ScopePtr& s) {
    const Node* handler = n->kid(1);
    if (n->str.empty()) return exec(handler, s);
    ScopePtr scope = newScope(s);
    bind(*scope, n->str, error);
    return exec(handler, scope);
  }

  SONORE_JS_NOINLINE Flow execTry(const Node* n, const ScopePtr& s) {
    const Node* block = n->kid(0);
    const Node* handler = n->kid(1);
    const Node* finalizer = n->kid(2);
    // The script's catch block runs AFTER the C++ catch block has ended, never
    // inside it: MSVC runs a catch block below the frames it unwinds, so a
    // script that recursed inside its catch blocks would spend stack the
    // budget measures but cannot see being freed.
    Flow flow = Flow::Normal;
    bool caught = false;
    JsVar error;
    int errorLine = 0, errorCol = 0;
    try {
      flow = exec(block, s);
    } catch (Throw& t) {
      caught = true;
      error = std::move(t.value);
      errorLine = t.line;
      errorCol = t.col;
    }
    if (!finalizer) return caught ? runCatch(n, error, s) : flow;
    bool threw = false;
    Throw pending{JsVar(), 0, 0};
    if (caught) {
      if (handler) {
        try {
          flow = runCatch(n, error, s);
        } catch (Throw& t) {
          threw = true;
          pending = std::move(t);
        }
      } else {
        threw = true;
        pending.value = std::move(error);
        pending.line = errorLine;
        pending.col = errorCol;
      }
    }
    // An Abort is not caught here: a limit ends the script, finally or not.
    JsVar savedReturn = returnValue_;
    const Flow f = exec(finalizer, s);
    if (f != Flow::Normal) return f;
    if (threw) throw pending;
    returnValue_ = std::move(savedReturn);
    return flow;
  }

  SONORE_JS_NOINLINE Flow execSwitch(const Node* n, const ScopePtr& s) {
    JsVar subject = eval(n->kids[0].get(), s);
    ScopePtr scope = s;
    if (n->flags & kHasLexical) {
      scope = newScope(s);
      for (size_t i = 1; i < n->kids.size(); ++i) declareLexical(n->kids[i]->kids, 1, scope, true);
    }
    size_t start = 0, fallback = 0;
    for (size_t i = 1; i < n->kids.size(); ++i) {
      const Node* c = n->kids[i].get();
      if (!c->kids[0]) {
        fallback = i;
        continue;
      }
      if (strictEquals(subject, eval(c->kids[0].get(), scope))) {
        start = i;
        break;
      }
    }
    if (!start) start = fallback;
    if (!start) return Flow::Normal;
    for (size_t i = start; i < n->kids.size(); ++i) {
      const Flow f = execList(n->kids[i]->kids, 1, scope);
      if (f == Flow::Break) return Flow::Normal;
      if (f != Flow::Normal) return f;
    }
    return Flow::Normal;
  }

  // ── Expressions ──────────────────────────────────────────────────────────

  JsVar eval(const Node* n, const ScopePtr& s) {
    checkStack();
    switch (n->kind) {
      case N::Num: return JsVar(n->num);
      case N::Str: return n->value;
      case N::True: return JsVar(true);
      case N::False: return JsVar(false);
      case N::Null: return JsVar(nullptr);
      case N::Hole: return JsVar();
      case N::Ident: return evalIdent(n, s);
      case N::This: return thisOf(s.get());
      case N::Template: return evalTemplate(n, s);
      case N::ArrayLit: return evalArray(n, s);
      case N::ObjectLit: return evalObject(n, s);
      case N::Func: return makeClosure(n, s);
      case N::Member: return evalMember(n, s);
      case N::Index: return evalIndex(n, s);
      case N::OptChain: return evalOptChain(n, s);
      case N::Call: return evalCall(n, s);
      case N::New: return evalNew(n, s);
      case N::Unary: return evalUnary(n, s);
      case N::Update: return evalUpdate(n, s);
      case N::Binary: return evalBinary(n, s);
      case N::Logical: return evalLogical(n, s);
      case N::Conditional: return evalConditional(n, s);
      case N::Assign: return evalAssign(n, s);
      case N::Sequence: return evalSequence(n, s);
      default: evalInvalid(n);
    }
  }

  SONORE_JS_NOINLINE JsVar evalIdent(const Node* n, const ScopePtr& s) {
    at(n);
    return lookup(n->str, s.get());
  }

  SONORE_JS_NOINLINE JsVar evalMember(const Node* n, const ScopePtr& s) {
    JsVar base = eval(n->kids[0].get(), s);
    if ((n->flags & kOptional) && base.isVoid()) throw ShortCircuit();
    at(n);
    return getProp(base, n->str);
  }

  SONORE_JS_NOINLINE JsVar evalIndex(const Node* n, const ScopePtr& s) {
    JsVar base = eval(n->kids[0].get(), s);
    if ((n->flags & kOptional) && base.isVoid()) throw ShortCircuit();
    JsVar key = eval(n->kids[1].get(), s);
    at(n);
    return getIndex(base, key);
  }

  SONORE_JS_NOINLINE JsVar evalOptChain(const Node* n, const ScopePtr& s) {
    try {
      return eval(n->kids[0].get(), s);
    } catch (const ShortCircuit&) {
      return JsVar();
    }
  }

  SONORE_JS_NOINLINE JsVar evalNew(const Node* n, const ScopePtr& s) {
    JsVar fn = eval(n->kids[0].get(), s);
    std::vector<JsVar> args;
    evalArgs(n, 1, s, args);
    at(n);
    return construct(fn, args.data(), args.size(), n->kid(0));
  }

  SONORE_JS_NOINLINE JsVar evalLogical(const Node* n, const ScopePtr& s) {
    JsVar left = eval(n->kids[0].get(), s);
    switch (n->op) {
      case Tok::LogAnd: return left.toBool() ? eval(n->kids[1].get(), s) : left;
      case Tok::LogOr: return left.toBool() ? left : eval(n->kids[1].get(), s);
      default: return left.isVoid() ? eval(n->kids[1].get(), s) : left;
    }
  }

  SONORE_JS_NOINLINE JsVar evalConditional(const Node* n, const ScopePtr& s) {
    return eval(n->kids[0].get(), s).toBool() ? eval(n->kids[1].get(), s)
                                              : eval(n->kids[2].get(), s);
  }

  SONORE_JS_NOINLINE JsVar evalSequence(const Node* n, const ScopePtr& s) {
    JsVar v;
    for (const auto& k : n->kids) v = eval(k.get(), s);
    return v;
  }

  [[noreturn]] SONORE_JS_NOINLINE void evalInvalid(const Node* n) {
    at(n);
    throwSyntax(n->kind == N::Spread ? "Unexpected spread" : "Unexpected statement in an expression");
  }

  SONORE_JS_NOINLINE JsVar evalTemplate(const Node* n, const ScopePtr& s) {
    std::string out = n->strs.empty() ? std::string() : n->strs[0];
    for (size_t i = 0; i < n->kids.size(); ++i) {
      JsVar v = eval(n->kids[i].get(), s);
      at(n);
      const std::string piece = toText(v);
      checkLength(out.size() + piece.size());
      appendText(out, piece);
      if (i + 1 < n->strs.size()) {
        checkLength(out.size() + n->strs[i + 1].size());
        appendText(out, n->strs[i + 1]);
      }
    }
    return str(std::move(out));
  }

  /** Spreads an iterable -- an array or a string -- into `out`. */
  void spreadInto(const JsVar& v, std::vector<JsVar>& out, const Node* site) {
    if (kind(v) == Kind::Array) {
      const auto& e = static_cast<ArrayObject*>(Access::obj(v))->elements;
      if (out.size() + e.size() > maxArray) throwRange("Invalid array length");
      // A literal of spreads runs no script step between them, so only this
      // stands between [[...a], [...a], ...] and every byte of the machine.
      ensureRoom((out.size() + e.size()) * sizeof(JsVar));
      out.insert(out.end(), e.begin(), e.end());
      return;
    }
    if (kind(v) == Kind::String) {
      const std::string& text = Access::str(v)->text;
      const unsigned char* p = (const unsigned char*) text.data();
      size_t i = 0;
      while (i < text.size()) {
        const size_t from = i;
        decodeCodePoint(p, text.size(), i);
        if (out.size() >= maxArray) throwRange("Invalid array length");
        out.push_back(str(text.substr(from, i - from)));
      }
      return;
    }
    at(site);
    throwType(describe(site) + " is not iterable");
  }

  void evalArgs(const Node* n, size_t from, const ScopePtr& s, std::vector<JsVar>& out) {
    out.reserve(n->kids.size() - from);
    for (size_t i = from; i < n->kids.size(); ++i) {
      const Node* k = n->kids[i].get();
      if (k->kind == N::Spread) {
        JsVar v = eval(k->kids[0].get(), s);
        spreadInto(v, out, k->kid(0));
      } else {
        out.push_back(eval(k, s));
      }
    }
  }

  SONORE_JS_NOINLINE JsVar evalArray(const Node* n, const ScopePtr& s) {
    std::vector<JsVar> items;
    evalArgs(n, 0, s, items);
    at(n);
    return arrayOf(std::move(items));
  }

  SONORE_JS_NOINLINE JsVar evalObject(const Node* n, const ScopePtr& s) {
    auto o = newObject();
    JsVar result = Access::fromObject(o);
    for (const auto& k : n->kids) {
      if (k->kind == N::Spread) {
        JsVar src = eval(k->kids[0].get(), s);
        at(k.get());
        const std::vector<std::string> keys = ownKeys(src);
        ensureRoom(keys.size() * (sizeof(DynamicObject::Property) + 64));
        for (const std::string& key : keys) o->put(key, getProp(src, key), DynamicObject::kEnumerable);
        continue;
      }
      std::string key;
      const Node* valueNode;
      if (k->flags & kComputed) {
        JsVar kv = eval(k->kids[0].get(), s);
        key = keyOf(kv);
        valueNode = k->kids[1].get();
      } else {
        key = k->str;
        valueNode = k->kids[0].get();
      }
      JsVar v = eval(valueNode, s);
      nameFunction(v, valueNode, key);
      o->put(key, v, DynamicObject::kEnumerable);
    }
    return result;
  }

  SONORE_JS_NOINLINE JsVar evalCall(const Node* n, const ScopePtr& s) {
    const Node* callee = n->kids[0].get();
    JsVar fn, self;
    if (callee->kind == N::Member) {
      self = eval(callee->kids[0].get(), s);
      if ((callee->flags & kOptional) && self.isVoid()) throw ShortCircuit();
      at(callee);
      fn = getProp(self, callee->str);
    } else if (callee->kind == N::Index) {
      self = eval(callee->kids[0].get(), s);
      if ((callee->flags & kOptional) && self.isVoid()) throw ShortCircuit();
      JsVar key = eval(callee->kids[1].get(), s);
      at(callee);
      fn = getIndex(self, key);
    } else {
      fn = eval(callee, s);
    }
    if ((n->flags & kOptional) && fn.isVoid()) throw ShortCircuit();
    std::vector<JsVar> args;
    evalArgs(n, 1, s, args);
    at(n);
    if (kind(fn) != Kind::Function) throwType(describe(callee) + " is not a function");
    return call(fn, self, args.data(), args.size());
  }

  JsVar typeOf(const JsVar& v) { return str(v.typeName()); }

  SONORE_JS_NOINLINE JsVar evalUnary(const Node* n, const ScopePtr& s) {
    const Node* operand = n->kids[0].get();
    switch (n->op) {
      case Tok::Typeof: {
        if (operand->kind == N::Ident) {
          // typeof of an undeclared name is "undefined", not an error.
          for (Scope* p = s.get(); p; p = p->parent.get())
            if (p->find(operand->str)) return typeOf(eval(operand, s));
          const DynamicObject::Property* g = global_->find(operand->str);
          return g ? typeOf(g->value) : str("undefined");
        }
        return typeOf(eval(operand, s));
      }
      case Tok::Delete: {
        if (operand->kind == N::Member || operand->kind == N::Index) {
          JsVar base = eval(operand->kids[0].get(), s);
          std::string key;
          JsVar keyValue;
          if (operand->kind == N::Member) key = operand->str;
          else keyValue = eval(operand->kids[1].get(), s);
          at(n);
          if (operand->kind == N::Index) key = keyOf(keyValue);
          if (base.isVoid())
            throwType("Cannot convert undefined or null to object");
          if (!isObj(base)) return JsVar(true);
          DynamicObject* o = Access::obj(base);
          if (kind(base) == Kind::Array) {
            uint32_t i = 0;
            ArrayObject* a = static_cast<ArrayObject*>(o);
            if (isIndexText(key, &i)) {
              if (i < a->elements.size()) a->elements[i] = JsVar();
              return JsVar(true);
            }
            if (key == "length") return JsVar(false);
          }
          if (const DynamicObject::Property* p = o->find(key))
            if (p->flags & DynamicObject::kReadOnly) return JsVar(false);
          o->removeProperty(key);
          return JsVar(true);
        }
        if (operand->kind == N::Ident) return JsVar(false);
        eval(operand, s);
        return JsVar(true);
      }
      case Tok::Void: eval(operand, s); return JsVar();
      default: break;
    }
    JsVar v = eval(operand, s);
    at(n);
    switch (n->op) {
      case Tok::Not: return JsVar(!v.toBool());
      case Tok::Neg: return JsVar(-toNumber(v));
      case Tok::Pos: return JsVar(toNumber(v));
      case Tok::BitNot: return JsVar((double) ~toInt32(toNumber(v)));
      default: return JsVar();
    }
  }

  /** Reads an assignment target, keeping what is needed to write it back. */
  struct Ref {
    JsVar base;
    JsVar key;
    bool isIndex = false;
  };

  JsVar readTarget(const Node* t, const ScopePtr& s, Ref& ref) {
    switch (t->kind) {
      case N::Ident: at(t); return lookup(t->str, s.get());
      case N::Member:
        ref.base = eval(t->kids[0].get(), s);
        at(t);
        return getProp(ref.base, t->str);
      default:
        ref.base = eval(t->kids[0].get(), s);
        ref.key = eval(t->kids[1].get(), s);
        ref.isIndex = true;
        at(t);
        if (isObj(ref.key)) ref.key = str(keyOf(ref.key)); // converted once, not twice
        return getIndex(ref.base, ref.key);
    }
  }

  void writeTarget(const Node* t, const ScopePtr& s, const Ref& ref, const JsVar& value) {
    at(t);
    switch (t->kind) {
      case N::Ident: assignVariable(t->str, value, s.get()); return;
      case N::Member: setProp(ref.base, t->str, value); return;
      default: setIndex(ref.base, ref.key, value); return;
    }
  }

  SONORE_JS_NOINLINE JsVar evalUpdate(const Node* n, const ScopePtr& s) {
    const Node* t = n->kids[0].get();
    Ref ref;
    const double old = toNumber(readTarget(t, s, ref));
    const double now = n->op == Tok::Inc ? old + 1 : old - 1;
    writeTarget(t, s, ref, JsVar(now));
    return JsVar((n->flags & kPrefix) ? now : old);
  }

  static Tok baseOperator(Tok assign) {
    switch (assign) {
      case Tok::AddAssign: return Tok::Add;
      case Tok::SubAssign: return Tok::Sub;
      case Tok::MulAssign: return Tok::Mul;
      case Tok::DivAssign: return Tok::Div;
      case Tok::ModAssign: return Tok::Mod;
      case Tok::ExpAssign: return Tok::Exp;
      case Tok::ShlAssign: return Tok::Shl;
      case Tok::ShrAssign: return Tok::Shr;
      case Tok::UShrAssign: return Tok::UShr;
      case Tok::AndAssign: return Tok::BitAnd;
      case Tok::OrAssign: return Tok::BitOr;
      case Tok::XorAssign: return Tok::BitXor;
      default: return Tok::End;
    }
  }

  SONORE_JS_NOINLINE JsVar evalAssign(const Node* n, const ScopePtr& s) {
    const Node* t = n->kids[0].get();
    const Node* valueNode = n->kids[1].get();
    if (n->op == Tok::Assign) {
      switch (t->kind) {
        case N::Ident: {
          JsVar v = eval(valueNode, s);
          nameFunction(v, valueNode, t->str);
          at(t);
          assignVariable(t->str, v, s.get());
          return v;
        }
        case N::Member: {
          JsVar base = eval(t->kids[0].get(), s);
          JsVar v = eval(valueNode, s);
          at(t);
          setProp(base, t->str, v);
          return v;
        }
        default: {
          JsVar base = eval(t->kids[0].get(), s);
          JsVar key = eval(t->kids[1].get(), s);
          JsVar v = eval(valueNode, s);
          at(t);
          setIndex(base, key, v);
          return v;
        }
      }
    }
    Ref ref;
    JsVar old = readTarget(t, s, ref);
    if (n->op == Tok::LogAndAssign || n->op == Tok::LogOrAssign || n->op == Tok::NullishAssign) {
      const bool assign = n->op == Tok::LogAndAssign   ? old.toBool()
                          : n->op == Tok::LogOrAssign ? !old.toBool()
                                                       : old.isVoid();
      if (!assign) return old;
      JsVar v = eval(valueNode, s);
      if (t->kind == N::Ident) nameFunction(v, valueNode, t->str);
      writeTarget(t, s, ref, v);
      return v;
    }
    JsVar right = eval(valueNode, s);
    at(n);
    JsVar v = binary(baseOperator(n->op), old, right);
    writeTarget(t, s, ref, v);
    return v;
  }

  SONORE_JS_NOINLINE JsVar evalBinary(const Node* n, const ScopePtr& s) {
    // A long chain -- a + b + c + ... -- nests to the LEFT, and evaluating it
    // by recursion would spend a stack frame per term. Its left spine is
    // walked iteratively instead.
    const Node* leftmost = n->kids[0].get();
    if (leftmost->kind != N::Binary) {
      JsVar a = eval(leftmost, s);
      JsVar b = eval(n->kids[1].get(), s);
      at(n);
      return binary(n->op, a, b);
    }
    std::vector<const Node*> spine;
    const Node* p = n;
    while (p->kind == N::Binary) {
      spine.push_back(p);
      p = p->kids[0].get();
    }
    JsVar acc = eval(p, s);
    for (size_t i = spine.size(); i-- > 0;) {
      const Node* b = spine[i];
      JsVar right = eval(b->kids[1].get(), s);
      at(b);
      acc = binary(b->op, acc, right);
    }
    return acc;
  }

  bool instanceOf(const JsVar& v, const JsVar& fn) {
    // A chain of bound functions reduces to its final bound target's test.
    // Unwrapped in a loop rather than by recursion, so a chain thousands deep
    // cannot overflow the native stack.
    JsVar callee = fn;
    for (;;) {
      if (kind(callee) != Kind::Function) throwType("Right-hand side of 'instanceof' is not callable");
      FunctionObject* f = static_cast<FunctionObject*>(Access::obj(callee));
      if (f->type != FunctionObject::Type::Bound) break;
      callee = f->boundTarget;
    }
    if (!isObj(v)) return false;
    JsVar proto = getProp(callee, "prototype");
    if (!isObj(proto))
      throwType("Function has non-object prototype in instanceof check");
    const DynamicObject* target = Access::obj(proto);
    const DynamicObject* o = protoOf(Access::obj(v));
    for (int guard = 0; o && guard < 100000; ++guard, o = protoOf(o))
      if (o == target) return true;
    return false;
  }

  JsVar binary(Tok op, const JsVar& a, const JsVar& b) {
    if (kind(a) == Kind::Number && kind(b) == Kind::Number) {
      const double x = num(a), y = num(b);
      switch (op) {
        case Tok::Add: return JsVar(x + y);
        case Tok::Sub: return JsVar(x - y);
        case Tok::Mul: return JsVar(x * y);
        case Tok::Div: return JsVar(x / y);
        case Tok::Mod: return JsVar(std::fmod(x, y));
        case Tok::Lt: return JsVar(x < y);
        case Tok::Gt: return JsVar(x > y);
        case Tok::Le: return JsVar(x <= y);
        case Tok::Ge: return JsVar(x >= y);
        case Tok::Eq:
        case Tok::StrictEq: return JsVar(x == y);
        case Tok::Ne:
        case Tok::StrictNe: return JsVar(x != y);
        default: break;
      }
    }
    switch (op) {
      case Tok::Add: {
        JsVar pa = toPrimitive(a, 0);
        JsVar pb = toPrimitive(b, 0);
        if (kind(pa) == Kind::String || kind(pb) == Kind::String) {
          JsVar sa = toStr(pa);
          JsVar sb = toStr(pb);
          return concat(sa, sb);
        }
        return JsVar(toNumber(pa) + toNumber(pb));
      }
      case Tok::StrictEq: return JsVar(strictEquals(a, b));
      case Tok::StrictNe: return JsVar(!strictEquals(a, b));
      case Tok::Eq: return JsVar(looseEquals(a, b));
      case Tok::Ne: return JsVar(!looseEquals(a, b));
      case Tok::Lt:
      case Tok::Gt:
      case Tok::Le:
      case Tok::Ge: {
        JsVar pa = toPrimitive(a, 1);
        JsVar pb = toPrimitive(b, 1);
        if (kind(pa) == Kind::String && kind(pb) == Kind::String) {
          const int c = compareText(Access::str(pa), Access::str(pb));
          switch (op) {
            case Tok::Lt: return JsVar(c < 0);
            case Tok::Gt: return JsVar(c > 0);
            case Tok::Le: return JsVar(c <= 0);
            default: return JsVar(c >= 0);
          }
        }
        const double x = toNumber(pa), y = toNumber(pb);
        switch (op) {
          case Tok::Lt: return JsVar(x < y);
          case Tok::Gt: return JsVar(x > y);
          case Tok::Le: return JsVar(x <= y);
          default: return JsVar(x >= y);
        }
      }
      case Tok::In: {
        if (!isObj(b))
          throwType("Cannot use 'in' operator to search for '" + keyOf(a) + "' in " + toText(b));
        return JsVar(hasProperty(b, keyOf(a)));
      }
      case Tok::Instanceof: return JsVar(instanceOf(a, b));
      default: break;
    }
    const double x = toNumber(a);
    const double y = toNumber(b);
    switch (op) {
      case Tok::Sub: return JsVar(x - y);
      case Tok::Mul: return JsVar(x * y);
      case Tok::Div: return JsVar(x / y);
      case Tok::Mod: return JsVar(std::fmod(x, y));
      case Tok::Exp: return JsVar(jsPow(x, y));
      case Tok::Shl: return JsVar((double) (int32_t) (toUint32(x) << (toUint32(y) & 31)));
      case Tok::Shr: return JsVar((double) (toInt32(x) >> (toUint32(y) & 31)));
      case Tok::UShr: return JsVar((double) (toUint32(x) >> (toUint32(y) & 31)));
      case Tok::BitAnd: return JsVar((double) (toInt32(x) & toInt32(y)));
      case Tok::BitOr: return JsVar((double) (toInt32(x) | toInt32(y)));
      case Tok::BitXor: return JsVar((double) (toInt32(x) ^ toInt32(y)));
      default: return JsVar();
    }
  }

  // ── Programs ─────────────────────────────────────────────────────────────

  std::shared_ptr<Program> parse(const std::string& code, bool tryExpression) {
    auto prog = std::make_shared<Program>();
    prog->source = code;
    const long long room = (std::max)(0ll, maxMemory - meter_->bytes.load());
    ensureRoom(code.size() * 2);
    std::vector<Token> tokens;
    Lexer lexer(prog->source, &stack_, (size_t) (std::max)(4096ll, room / 96));
    lexer.run(tokens);
    const size_t maxNodes = (size_t) (std::max)(1024ll, room / 192);
    bool parsed = false;
    if (tryExpression && !(tokens[0].type == Tok::Name && tokens[0].text == "function")) {
      Parser p(*prog, &stack_, maxNodes);
      std::unique_ptr<Node> e = p.parseWholeExpression(tokens);
      if (e) {
        prog->root = std::move(e);
        prog->isExpression = true;
        prog->charged = (long long) (p.nodeCount() * 192 + code.size());
        parsed = true;
      } else {
        prog->varNames.clear();
      }
    }
    if (!parsed) {
      Parser p(*prog, &stack_, maxNodes);
      prog->root = p.parseProgram(tokens);
      prog->charged = (long long) (p.nodeCount() * 192 + code.size());
    }
    prog->meter = meter_;
    meter_->bytes += prog->charged;
    return prog;
  }

  /** Runs a parsed program at global scope; its completion value is the
   *  value of the last expression statement that ran at the top level. */
  JsVar runProgram(const std::shared_ptr<const Program>& prog) {
    const std::shared_ptr<const Program>* savedProgram = program_;
    const int savedCompletionDepth = completionDepth_;
    JsVar savedCompletion = std::move(completion_);
    struct Restore {
      Interp& I;
      const std::shared_ptr<const Program>* p;
      int depth;
      JsVar& saved;
      ~Restore() {
        I.program_ = p;
        I.completionDepth_ = depth;
        I.completion_ = std::move(saved);
      }
    } restore{*this, savedProgram, savedCompletionDepth, savedCompletion};
    program_ = &prog;
    if (prog->isExpression) return eval(prog->root.get(), globalScope_);
    const Node* root = prog->root.get();
    for (const std::string& name : prog->varNames)
      if (!global_->find(name)) global_->put(name, JsVar(), DynamicObject::kEnumerable);
    for (const auto& k : root->kids)
      if (k->kind == N::VarDecl && k->decl != Decl::Var)
        for (const auto& d : k->kids)
          bind(*globalScope_, d->str, JsVar(),
               (uint8_t) (Binding::kUninitialised |
                          (k->decl == Decl::Const ? Binding::kConst : 0)));
    globalScope_->recharge();
    for (const auto& k : root->kids)
      if (k->kind == N::FuncDecl) assignOwn(global_.get(), k->fn->name, makeClosure(k.get(), globalScope_));
    completion_ = JsVar();
    completionDepth_ = depth_;
    execList(root->kids, 0, globalScope_);
    return completion_;
  }

  /** Parses and runs `code` -- for eval(), and for the engine. A syntax error
   *  becomes a SyntaxError a script can catch. */
  JsVar evaluateCode(const std::string& code, bool tryExpression) {
    std::shared_ptr<Program> prog;
    std::string failure;
    try {
      prog = parse(code, tryExpression);
    } catch (const SyntaxErr& e) {
      line_ = e.line;
      col_ = e.col;
      failure = e.message;
    }
    if (!prog) throwSyntax(failure); // thrown outside the catch block, as in execTry
    std::shared_ptr<const Program> constProg = std::move(prog);
    return runProgram(constProg);
  }

  // ── Built-ins (defined below) ────────────────────────────────────────────

  void installBuiltins();
  static void installGlobalBuiltins(Interp& R);
  static void installObjectBuiltins(Interp& R);
  static void installArrayBuiltins(Interp& R);
  static void installStringBuiltins(Interp& R);
  static void installNumberBuiltins(Interp& R);
  static void installMathBuiltins(Interp& R);
  static std::shared_ptr<FunctionObject> constructorFor(Interp& R, const std::string& name,
                                                        int arity, Builtin fn,
                                                        const std::shared_ptr<DynamicObject>& proto);
  std::shared_ptr<FunctionObject> builtin(const std::string& name, int arity, Builtin fn);
  void method(const std::shared_ptr<DynamicObject>& target, const std::string& name, int arity,
              Builtin fn);
  /** What console.log may still write, and a count of the objects it has
   *  shown, for spacing out the interrupt checks. */
  struct InspectBudget {
    size_t left;
    unsigned steps;
  };
  std::string inspect(const JsVar& v, int depthLeft, std::vector<const DynamicObject*>& seen,
                      bool top, InspectBudget& budget);
  std::string join(const JsVar& arrayValue, const std::string& separator);
  JsVar parseJson(const std::string& text);
  bool stringifyJson(const JsVar& value, const JsVar& replacer, const std::string& indent,
                     std::string& out);
  double random() {
    // xorshift128+: plenty for Math.random, which promises nothing more.
    uint64_t s1 = rng_[0];
    const uint64_t s0 = rng_[1];
    rng_[0] = s0;
    s1 ^= s1 << 23;
    rng_[1] = s1 ^ s0 ^ (s1 >> 17) ^ (s0 >> 26);
    return (double) ((rng_[1] + s0) >> 11) * (1.0 / 9007199254740992.0);
  }

  // ── State ────────────────────────────────────────────────────────────────

  JsVar charCache_[128];
  int entries_ = 0;
  int depth_ = 0;
  StackGuard stack_;
  bool hasDeadline_ = false;
  std::chrono::steady_clock::time_point deadline_;
  unsigned ticks_ = 0;
  int line_ = 0;
  int col_ = 0;
  JsVar returnValue_;
  JsVar completion_;
  int completionDepth_ = -1;
  const std::shared_ptr<const Program>* program_ = nullptr;
  std::vector<std::weak_ptr<GcNode>> registry_;
  size_t registryLimit_ = 4096;
  std::vector<const DynamicObject*> joining_;
  uint64_t rng_[2] = {1, 2};
};

} // namespace js
} // namespace detail

namespace detail {
namespace js {

// ── JSON ────────────────────────────────────────────────────────────────────
//
// Its own reader and writer rather than JsonValue's: JSON.parse must keep
// the order the text gave the keys in (JsonValue sorts them), and a script's
// JSON is charged to its engine's memory meter like everything else. With no
// engine (JsVar::parseJson from C++) the same code allocates plainly.

/** What the conversions that run without an engine -- JsVar::toString,
 *  toJsonString, toJson, and the message of an uncaught throw -- may produce
 *  (in bytes) or visit (in arrays). They have no meter and no clock, and an
 *  array shared many times over is expanded every time it is met: three
 *  nested arrays of a thousand references each are a billion elements from a
 *  few kilobytes of script. Past this, text ends in "...", toJsonString gives
 *  an empty string, and toJson cuts the rest with null. */
constexpr size_t kPlainTextLimit = (size_t) 8 << 20;

class JsonReader {
public:
  JsonReader(const std::string& text, Interp* interp) : t_(text), I(interp) {}

  /** The value, or throws SyntaxErr. */
  JsVar read() {
    JsVar v = value(0);
    space();
    if (at_ < t_.size()) fail("Unexpected non-whitespace character after JSON");
    return v;
  }

private:
  const std::string& t_;
  Interp* I;
  size_t at_ = 0;

  [[noreturn]] void fail(const std::string& message) {
    throw SyntaxErr{message + " at position " + std::to_string(at_), 1, (int) at_ + 1};
  }
  void space() {
    while (at_ < t_.size() &&
           (t_[at_] == ' ' || t_[at_] == '\t' || t_[at_] == '\n' || t_[at_] == '\r'))
      ++at_;
  }
  JsVar makeString(std::string s) {
    if (I) return I->str(std::move(s));
    return JsVar(s);
  }

  JsVar value(int depth) {
    if (depth > 512 || (I && I->stack_.exceeded())) fail("JSON nested too deeply");
    space();
    if (at_ >= t_.size()) fail("Unexpected end of JSON input");
    const char c = t_[at_];
    if (c == kLBrace) return object(depth);
    if (c == '[') return array(depth);
    if (c == '"') return makeString(string());
    if (t_.compare(at_, 4, "true") == 0) {
      at_ += 4;
      return JsVar(true);
    }
    if (t_.compare(at_, 5, "false") == 0) {
      at_ += 5;
      return JsVar(false);
    }
    if (t_.compare(at_, 4, "null") == 0) {
      at_ += 4;
      return JsVar(nullptr);
    }
    if (c == '-' || (c >= '0' && c <= '9')) return number();
    fail(std::string("Unexpected token '") + c + "' in JSON");
  }

  JsVar number() {
    const size_t from = at_;
    if (t_[at_] == '-') ++at_;
    if (at_ >= t_.size() || !(t_[at_] >= '0' && t_[at_] <= '9')) fail("No number after minus sign in JSON");
    if (t_[at_] == '0') {
      ++at_;
    } else {
      while (at_ < t_.size() && t_[at_] >= '0' && t_[at_] <= '9') ++at_;
    }
    if (at_ < t_.size() && t_[at_] == '.') {
      ++at_;
      if (at_ >= t_.size() || !(t_[at_] >= '0' && t_[at_] <= '9')) fail("Unterminated fractional number in JSON");
      while (at_ < t_.size() && t_[at_] >= '0' && t_[at_] <= '9') ++at_;
    }
    if (at_ < t_.size() && (t_[at_] == 'e' || t_[at_] == 'E')) {
      ++at_;
      if (at_ < t_.size() && (t_[at_] == '+' || t_[at_] == '-')) ++at_;
      if (at_ >= t_.size() || !(t_[at_] >= '0' && t_[at_] <= '9')) fail("Exponent part is missing a number in JSON");
      while (at_ < t_.size() && t_[at_] >= '0' && t_[at_] <= '9') ++at_;
    }
    return JsVar(parseDecimal(t_.data() + from, at_ - from));
  }

  std::string string() {
    ++at_; // "
    std::u16string units;
    for (;;) {
      if (at_ >= t_.size()) fail("Unterminated string in JSON");
      const unsigned char c = (unsigned char) t_[at_];
      if (c == '"') {
        ++at_;
        break;
      }
      if (c < 0x20) fail("Bad control character in string literal in JSON");
      if (c == '\\') {
        ++at_;
        if (at_ >= t_.size()) fail("Unterminated string in JSON");
        const char e = t_[at_++];
        switch (e) {
          case '"': units += u'"'; break;
          case '\\': units += u'\\'; break;
          case '/': units += u'/'; break;
          case 'b': units += u'\b'; break;
          case 'f': units += u'\f'; break;
          case 'n': units += u'\n'; break;
          case 'r': units += u'\r'; break;
          case 't': units += u'\t'; break;
          case 'u': {
            uint32_t v = 0;
            for (int i = 0; i < 4; ++i) {
              const int d = at_ < t_.size() ? digitValue(t_[at_]) : 99;
              if (d >= 16) fail("Bad Unicode escape in JSON");
              v = v * 16 + (uint32_t) d;
              ++at_;
            }
            units += (char16_t) v;
            break;
          }
          default: fail("Bad escaped character in JSON");
        }
        continue;
      }
      size_t i = at_;
      const uint32_t cp = decodeCodePoint((const unsigned char*) t_.data(), t_.size(), i);
      at_ = i;
      if (cp >= 0x10000) {
        units += (char16_t) (0xD800 + ((cp - 0x10000) >> 10));
        units += (char16_t) (0xDC00 + ((cp - 0x10000) & 0x3FF));
      } else {
        units += (char16_t) cp;
      }
    }
    return fromUtf16(units);
  }

  JsVar array(int depth);
  JsVar object(int depth);
};

// ── Interp's built-ins ──────────────────────────────────────────────────────

inline std::shared_ptr<FunctionObject> Interp::builtin(const std::string& name, int arity,
                                                       Builtin fn) {
  return newNative(name, arity, [fn](const JsVar::NativeFunctionArgs& a) -> JsVar {
    Interp* I = currentInterp();
    if (!I) return JsVar();
    const Args args{a.arguments, a.numArguments};
    return fn(*I, a.thisObject, args);
  });
}

inline void Interp::method(const std::shared_ptr<DynamicObject>& target, const std::string& name,
                           int arity, Builtin fn) {
  target->put(name, Access::fromObject(builtin(name, arity, fn)), 0);
}

inline JsVar JsonReader::array(int depth) {
  ++at_; // [
  std::vector<JsVar> items;
  space();
  if (at_ < t_.size() && t_[at_] == ']') {
    ++at_;
  } else {
    for (;;) {
      items.push_back(value(depth + 1));
      if (I && items.size() > I->maxArray) I->throwRange("Invalid array length");
      space();
      if (at_ < t_.size() && t_[at_] == ',') {
        ++at_;
        continue;
      }
      if (at_ < t_.size() && t_[at_] == ']') {
        ++at_;
        break;
      }
      fail(at_ >= t_.size() ? "Unexpected end of JSON input" : "Expected ',' or ']' after array element in JSON");
    }
  }
  if (I) return I->arrayOf(std::move(items));
  return JsVar::array(std::move(items));
}

inline JsVar JsonReader::object(int depth) {
  ++at_; // {
  std::shared_ptr<DynamicObject> o = I ? I->newObject() : std::make_shared<DynamicObject>();
  JsVar result(o);
  space();
  if (at_ < t_.size() && t_[at_] == kRBrace) {
    ++at_;
    return result;
  }
  for (;;) {
    space();
    if (at_ >= t_.size() || t_[at_] != '"')
      fail(at_ >= t_.size() ? "Unexpected end of JSON input" : "Expected double-quoted property name in JSON");
    std::string key = string();
    space();
    if (at_ >= t_.size() || t_[at_] != ':') fail("Expected ':' after property name in JSON");
    ++at_;
    JsVar v = value(depth + 1);
    // Measured per property: 8 MB of {"k1":1,"k2":1,...} is a million of them.
    if (I) I->ensureRoom(key.size() + 128); // the key, the property and its index entry
    o->setProperty(key, v); // a repeated key keeps its first position, as JSON.parse does
    space();
    if (at_ < t_.size() && t_[at_] == ',') {
      ++at_;
      continue;
    }
    if (at_ < t_.size() && t_[at_] == kRBrace) {
      ++at_;
      return result;
    }
    fail(at_ >= t_.size() ? "Unexpected end of JSON input" : "Expected ',' or '\x7d' after property value in JSON");
  }
}

inline JsVar Interp::parseJson(const std::string& text) {
  std::string failure;
  try {
    JsonReader reader(text, this);
    return reader.read();
  } catch (const SyntaxErr& e) {
    failure = e.message;
  }
  throwSyntax(failure);
}

inline void quoteJson(std::string& out, const std::string& text) {
  out += '"';
  const unsigned char* s = (const unsigned char*) text.data();
  const size_t n = text.size();
  size_t i = 0;
  while (i < n) {
    const unsigned char c = s[i];
    switch (c) {
      case '"': out += "\\\""; ++i; continue;
      case '\\': out += "\\\\"; ++i; continue;
      case '\b': out += "\\b"; ++i; continue;
      case '\f': out += "\\f"; ++i; continue;
      case '\n': out += "\\n"; ++i; continue;
      case '\r': out += "\\r"; ++i; continue;
      case '\t': out += "\\t"; ++i; continue;
      default: break;
    }
    if (c < 0x20) {
      char buffer[8];
      std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
      out += buffer;
      ++i;
      continue;
    }
    if (c == 0xED && i + 2 < n && (s[i + 1] & 0xE0) == 0xA0) {
      // A lone surrogate is written as an escape: well-formed JSON.stringify.
      const uint32_t u = 0xD000u | ((uint32_t) (s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3Fu);
      char buffer[8];
      std::snprintf(buffer, sizeof(buffer), "\\u%04x", (unsigned) u);
      out += buffer;
      i += 3;
      continue;
    }
    out += (char) c;
    ++i;
  }
  out += '"';
}

/** JSON.stringify's SerializeJSONProperty, with an engine (toJSON and a
 *  replacer are called) or without one (plain data only). */
class JsonWriter {
public:
  JsonWriter(Interp* interp, const JsVar& replacer, const std::string& indent)
      : I(interp), replacer_(replacer), indent_(indent) {
    if (I && replacer.isArray()) {
      for (const JsVar& k : static_cast<ArrayObject*>(Access::obj(replacer))->elements) {
        if (k.isString() || k.isNumber()) {
          const std::string key = I->keyOf(k);
          if (std::find(allow_.begin(), allow_.end(), key) == allow_.end()) allow_.push_back(key);
        }
      }
      useAllow_ = true;
    }
  }

  /** False when the value has no JSON form (undefined, a function). */
  bool write(const JsVar& value, std::string& out) {
    JsVar holder = I ? Access::fromObject(I->newObject()) : JsVar::object();
    if (I) Access::obj(holder)->put("", value, DynamicObject::kEnumerable);
    return property(holder, "", value, out);
  }

private:
  Interp* I;
  JsVar replacer_;
  std::string indent_;
  std::string gap_;
  std::vector<std::string> allow_;
  bool useAllow_ = false;
  std::vector<const DynamicObject*> stack_;
  size_t meterMark_ = 65536;

  void check(const std::string& out) {
    if (I) {
      if (out.size() > I->maxString) I->throwRange("Invalid string length");
      if (I->stack_.exceeded()) I->abort("Maximum call stack size exceeded");
      if (out.size() >= meterMark_) {
        // The text is not a string yet, so nothing has charged it: measured
        // against the memory limit every 64 KB it grows, and stop() and the
        // clock looked at, since a big value is written without a script step.
        I->ensureRoom(out.size());
        I->interruptPoint();
        meterMark_ = out.size() + 65536;
      }
    } else if (stack_.size() > 512) {
      throw std::length_error("JSON nested too deeply");
    } else if (out.size() > kPlainTextLimit) {
      // No engine, so no string limit to meet: shared structure would
      // otherwise be written out in full every time it is met.
      throw std::length_error("JSON text too large");
    }
  }

  bool property(const JsVar& holder, const std::string& key, JsVar value, std::string& out) {
    check(out);
    if (I) {
      if (Access::kind(value) >= JsVar::Kind::Array) {
        JsVar toJson = I->getProp(value, "toJSON");
        if (toJson.isFunction()) {
          JsVar k = I->str(key);
          value = I->call(toJson, value, &k, 1);
        }
      }
      if (replacer_.isFunction()) {
        JsVar argv[2] = {I->str(key), value};
        value = I->call(replacer_, holder, argv, 2);
      }
    }
    switch (Access::kind(value)) {
      case JsVar::Kind::Null: out += "null"; return true;
      case JsVar::Kind::Boolean: out += value.toBool() ? "true" : "false"; return true;
      case JsVar::Kind::Number: {
        const double d = Access::num(value);
        out += std::isfinite(d) ? numberToString(d) : "null";
        return true;
      }
      case JsVar::Kind::String: quoteJson(out, Access::str(value)->text); return true;
      case JsVar::Kind::Array: return array(value, out);
      case JsVar::Kind::Object: return object(value, out);
      default: return false; // undefined, functions
    }
  }

  void enter(const DynamicObject* o) {
    if (std::find(stack_.begin(), stack_.end(), o) != stack_.end()) {
      if (I) I->throwType("Converting circular structure to JSON");
      throw std::length_error("Converting circular structure to JSON");
    }
    stack_.push_back(o);
  }

  void newline(std::string& out) {
    if (indent_.empty()) return;
    out += '\n';
    out += gap_;
  }

  bool array(const JsVar& value, std::string& out) {
    ArrayObject* a = static_cast<ArrayObject*>(Access::obj(value));
    enter(a);
    const std::string stepBack = gap_;
    gap_ += indent_;
    out += '[';
    const size_t n = a->elements.size();
    for (size_t i = 0; i < n && i < a->elements.size(); ++i) {
      if (i) out += ',';
      newline(out);
      JsVar e = a->elements[i];
      if (!property(value, std::to_string(i), e, out)) out += "null";
    }
    gap_ = stepBack;
    if (n) newline(out);
    out += ']';
    stack_.pop_back();
    return true;
  }

  bool object(const JsVar& value, std::string& out) {
    DynamicObject* o = Access::obj(value);
    enter(o);
    const std::string stepBack = gap_;
    gap_ += indent_;
    out += kLBrace;
    bool any = false;
    const std::vector<std::string> keys = useAllow_ ? allow_ : o->keysInOrder(true);
    for (const std::string& k : keys) {
      const DynamicObject::Property* p = o->find(k);
      if (!p) continue;
      JsVar v = p->value;
      const size_t mark = out.size();
      if (any) out += ',';
      newline(out);
      quoteJson(out, k);
      out += indent_.empty() ? ":" : ": ";
      if (!property(value, k, v, out)) {
        out.resize(mark); // a member with no JSON form is left out altogether
        continue;
      }
      any = true;
    }
    gap_ = stepBack;
    if (any) newline(out);
    out += kRBrace;
    stack_.pop_back();
    return true;
  }
};

inline bool Interp::stringifyJson(const JsVar& value, const JsVar& replacer,
                                  const std::string& indent, std::string& out) {
  JsonWriter writer(this, replacer, indent);
  return writer.write(value, out);
}

// ── Joining and showing ─────────────────────────────────────────────────────

inline std::string Interp::join(const JsVar& arrayValue, const std::string& separator) {
  ArrayObject* a = static_cast<ArrayObject*>(Access::obj(arrayValue));
  // An array that contains itself joins as empty there, as engines do,
  // rather than recursing until the stack is gone.
  if (std::find(joining_.begin(), joining_.end(), a) != joining_.end()) return std::string();
  checkStack();
  joining_.push_back(a);
  struct Pop {
    std::vector<const DynamicObject*>& v;
    ~Pop() { v.pop_back(); }
  } pop{joining_};
  std::string out;
  for (size_t i = 0; i < a->elements.size(); ++i) {
    if (i) {
      checkLength(out.size() + separator.size());
      out += separator;
    }
    JsVar e = a->elements[i];
    if (e.isVoid()) continue;
    const std::string piece = toText(e);
    checkLength(out.size() + piece.size());
    appendText(out, piece);
  }
  return out;
}

inline bool isPlainIdentifier(const std::string& s) {
  if (s.empty() || !(isIdentStart((unsigned char) s[0]) && (unsigned char) s[0] < 0x80)) return false;
  for (unsigned char c : s)
    if (!(isIdentPart(c) && c < 0x80)) return false;
  return true;
}

inline std::string quoteSingle(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') out += "\\'";
    else if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else out += c;
  }
  return out + "'";
}

/** What console.log shows for a value: node's style for the simple cases --
 *  [ 1, 2, 3 ], { a: 1, b: 'x' }, [Function: f] -- on one line.
 *
 *  An object shared many times over is shown every time it is met, so a few
 *  objects of a few hundred keys each, nested three deep, are hundreds of
 *  megabytes of text: the budget caps what one console.log writes (past it,
 *  "..."), and the walk checks stop() and the time limit as it goes. An
 *  Error's name and message are converted by script, which may change what
 *  is being walked, so every value is walked from a copy the walk holds, and
 *  a key deleted meanwhile is skipped. */
inline std::string Interp::inspect(const JsVar& v, int depthLeft,
                                   std::vector<const DynamicObject*>& seen, bool top,
                                   InspectBudget& budget) {
  switch (kind(v)) {
    case Kind::String: return top ? Access::str(v)->text : quoteSingle(Access::str(v)->text);
    case Kind::Number: {
      const double d = num(v);
      return (d == 0 && std::signbit(d)) ? "-0" : numberToString(d);
    }
    case Kind::Boolean: return num(v) != 0 ? "true" : "false";
    case Kind::Undefined: return "undefined";
    case Kind::Null: return "null";
    case Kind::Function: {
      const FunctionObject* f = static_cast<const FunctionObject*>(Access::obj(v));
      return f->name.empty() ? "[Function (anonymous)]" : "[Function: " + f->name + "]";
    }
    default: break;
  }
  checkStack();
  if (budget.left == 0) return "...";
  if ((++budget.steps & 0xFF) == 0) interruptPoint();
  const DynamicObject* o = Access::obj(v);
  if (std::find(seen.begin(), seen.end(), o) != seen.end()) return "[Circular *1]";
  for (const DynamicObject* p = protoOf(o); p; p = protoOf(p)) {
    if (p == errorProto_.get()) {
      const std::string name = toText(chainLookup(o, "name"));
      const std::string message = toText(chainLookup(o, "message"));
      return message.empty() ? name : name + ": " + message;
    }
  }
  const bool isArray = kind(v) == Kind::Array;
  if (depthLeft < 0) return isArray ? "[Array]" : "[Object]";
  seen.push_back(o);
  std::vector<std::string> parts;
  // Each part is charged as it is added; one that does not fit ends the
  // container with "...". (A nested container's text is charged again by its
  // parent, so the budget runs out somewhat early -- never late.)
  bool full = false;
  auto add = [&budget, &parts, &full](std::string part) {
    if (part.size() + 2 > budget.left) {
      budget.left = 0;
      parts.push_back("...");
      full = true;
      return;
    }
    budget.left -= part.size() + 2;
    parts.push_back(std::move(part));
  };
  if (isArray) {
    const auto& e = static_cast<const ArrayObject*>(o)->elements;
    for (size_t i = 0; i < e.size() && i < 100 && !full; ++i) {
      const JsVar element = e[i];
      add(inspect(element, depthLeft - 1, seen, false, budget));
    }
    if (e.size() > 100 && !full) add("... " + std::to_string(e.size() - 100) + " more items");
  }
  for (const std::string& k : o->keysInOrder(true)) {
    if (full) break;
    const DynamicObject::Property* p = o->find(k);
    if (!p) continue; // deleted by script while the walk was elsewhere
    const JsVar value = p->value;
    add((isPlainIdentifier(k) ? k : quoteSingle(k)) + ": " + inspect(value, depthLeft - 1, seen, false, budget));
  }
  seen.pop_back();
  if (parts.empty()) return isArray ? "[]" : "{}";
  std::string out = isArray ? "[ " : "\x7b ";
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i) out += ", ";
    out += parts[i];
  }
  return out + (isArray ? " ]" : " \x7d");
}

// ── String helpers for the built-ins ────────────────────────────────────────

/** Knuth-Morris-Pratt: the first i >= from where pattern p (m >= 1 units)
 *  starts in text t (n units), or npos. `t` and `p` are index -> unit
 *  functions, so the same code searches backwards over reversed views. Linear
 *  in n + m whatever the text, and it checks stop() and the time limit as it
 *  goes. Its table is m entries, asked of the memory limit first. */
template <typename Text, typename Pattern>
inline size_t kmpSearch(Interp& I, Text t, size_t n, Pattern p, size_t m, size_t from) {
  I.ensureRoom(m * sizeof(uint32_t));
  std::vector<uint32_t> fail(m, 0);
  for (size_t i = 1, k = 0; i < m; ++i) {
    while (k && p(i) != p(k)) k = fail[k - 1];
    if (p(i) == p(k)) ++k;
    fail[i] = (uint32_t) k;
    if ((i & 0xFFFF) == 0) I.interruptPoint();
  }
  for (size_t i = from, k = 0; i < n; ++i) {
    while (k && t(i) != p(k)) k = fail[k - 1];
    if (t(i) == p(k)) ++k;
    if (k == m) return i + 1 - m;
    if ((i & 0xFFFF) == 0) I.interruptPoint();
  }
  return (size_t) -1;
}

/**
 * String search for indexOf, includes, split, replace and lastIndexOf, with
 * std::basic_string's find / rfind results (npos for none; an empty needle
 * matches at `from`, clamped). A naive search is n * m in the worst case --
 * 'a' repeated a million times, searched for half a million 'a's and a 'b',
 * took twelve seconds that neither stop() nor the time limit could end. A
 * short needle uses the library's (fast) search a window of a million
 * positions at a time, which bounds each window's cost, with an interrupt
 * check between windows; a long one uses KMP, which is linear.
 */
template <typename C>
inline size_t searchUnits(Interp& I, const C* hay, size_t n, const C* needle, size_t m, size_t from,
                          bool last) {
  const size_t npos = (size_t) -1;
  const size_t kWindow = (size_t) 1 << 20;
  const size_t kShort = 16;
  if (m > n) return npos;
  if (!last) {
    if (m == 0) return from <= n ? from : npos;
    if (from > n - m) return npos;
    if (m <= kShort) {
      const std::basic_string_view<C> h(hay, n), p(needle, m);
      for (size_t pos = from;;) {
        const size_t lastStart = (std::min)(n - m, pos + kWindow - 1);
        const size_t at = h.substr(0, lastStart + m).find(p, pos);
        if (at != std::basic_string_view<C>::npos) return at;
        if (lastStart == n - m) return npos;
        pos = lastStart + 1;
        I.interruptPoint();
      }
    }
    return kmpSearch(I, [hay](size_t i) { return hay[i]; }, n, [needle](size_t j) { return needle[j]; }, m,
                     from);
  }
  const size_t start = (std::min)(from, n - m); // the last start allowed
  if (m == 0) return start;
  if (m <= kShort) {
    const std::basic_string_view<C> h(hay, n), p(needle, m);
    for (size_t hi = start;;) {
      const size_t lo = hi >= kWindow ? hi - kWindow + 1 : 0;
      const size_t at = h.substr(lo, hi - lo + m).rfind(p);
      if (at != std::basic_string_view<C>::npos) return lo + at;
      if (lo == 0) return npos;
      hi = lo - 1;
      I.interruptPoint();
    }
  }
  // The last match ending by start + m is the first match of the reversed
  // needle in the reversed text that ends there.
  const size_t length = start + m;
  const size_t r = kmpSearch(
      I, [hay, length](size_t i) { return hay[length - 1 - i]; }, length,
      [needle, m](size_t j) { return needle[m - 1 - j]; }, m, 0);
  return r == npos ? npos : length - r - m;
}

/** A string seen as UTF-16 units: ASCII text is used as it is, anything else
 *  decoded once. */
struct Units {
  const StringRep* rep;
  std::u16string wide;
  bool ascii;
  // An ASCII string widened to units, made at most once and only when it is
  // searched against a non-ASCII one: split() searches the same string over
  // and over, and widening (or decoding) it on every search made that
  // quadratic.
  mutable std::u16string widened;
  mutable bool widenedReady = false;
  explicit Units(const StringRep* r) : rep(r), ascii(r->ascii) {
    if (!ascii) toUtf16(r->text, wide);
  }
  const char16_t* data16() const {
    if (!ascii) return wide.data();
    if (!widenedReady) {
      widened.reserve(rep->text.size());
      for (unsigned char c : rep->text) widened += (char16_t) c;
      widenedReady = true;
    }
    return widened.data();
  }
  size_t size() const { return ascii ? rep->text.size() : wide.size(); }
  char16_t at(size_t i) const { return ascii ? (char16_t) (unsigned char) rep->text[i] : wide[i]; }
  JsVar slice(Interp& I, size_t from, size_t to) const {
    if (to <= from) return I.str(std::string());
    if (ascii) return I.str(rep->text.substr(from, to - from));
    return I.strUnits(wide.data() + from, to - from);
  }
  std::u16string units() const {
    if (!ascii) return wide;
    std::u16string u;
    u.reserve(rep->text.size());
    for (unsigned char c : rep->text) u += (char16_t) c;
    return u;
  }
  size_t find(Interp& I, const Units& needle, size_t from) const { return search(I, needle, from, false); }
  size_t rfind(Interp& I, const Units& needle, size_t from) const { return search(I, needle, from, true); }

private:
  size_t search(Interp& I, const Units& needle, size_t from, bool last) const {
    if (ascii && needle.ascii)
      return searchUnits<char>(I, rep->text.data(), rep->text.size(), needle.rep->text.data(),
                               needle.rep->text.size(), from, last);
    return searchUnits<char16_t>(I, data16(), size(), needle.data16(), needle.size(), from, last);
  }
};

inline const StringRep* thisString(Interp& I, const JsVar& self, const char* name, JsVar& keep) {
  if (self.isVoid())
    I.throwType(std::string("String.prototype.") + name + " called on null or undefined");
  keep = I.toStr(self);
  return Access::str(keep);
}

inline double argInteger(Interp& I, const JsVar& v) { return toInteger(I.toNumber(v)); }

/** A relative index into a length, as slice() and its relatives read one. */
inline size_t relativeIndex(double rel, size_t length) {
  if (rel < 0) {
    const double r = (double) length + rel;
    return r <= 0 ? 0 : (size_t) r;
  }
  return rel >= (double) length ? length : (size_t) rel;
}

inline size_t clampIndex(double v, size_t length) {
  if (v <= 0) return 0;
  return v >= (double) length ? length : (size_t) v;
}

/** Case mapping for ASCII, Latin-1, Latin Extended-A, Greek and Cyrillic --
 *  the European alphabets. Other scripts are returned unchanged. */
inline uint32_t caseMap(uint32_t c, bool upper) {
  if (upper) {
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c < 0x80) return c;
    if ((c >= 0xE0 && c <= 0xFE && c != 0xF7)) return c - 32;
    if (c == 0xFF) return 0x178;
    if (c == 0xB5) return 0x39C;
    if (c >= 0x100 && c <= 0x17F) {
      if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c % 2 == 0) ? c - 1 : c;
      if (c == 0x131) return 'I';
      if (c == 0x17F) return 'S';
      if (c == 0x130 || c == 0x138 || c == 0x149 || c == 0x178) return c;
      return (c % 2 == 1) ? c - 1 : c;
    }
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 32;
    if (c == 0x3C2) return 0x3A3;
    if (c >= 0x430 && c <= 0x44F) return c - 32;
    if (c >= 0x450 && c <= 0x45F) return c - 80;
    return c;
  }
  if (c >= 'A' && c <= 'Z') return c + 32;
  if (c < 0x80) return c;
  if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
  if (c == 0x178) return 0xFF;
  if (c >= 0x100 && c <= 0x17F) {
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c % 2 == 1) ? c + 1 : c;
    if (c == 0x130) return 'i';
    if (c == 0x131 || c == 0x138 || c == 0x149 || c == 0x17F) return c;
    return (c % 2 == 0) ? c + 1 : c;
  }
  if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 32;
  if (c >= 0x410 && c <= 0x42F) return c + 32;
  if (c >= 0x400 && c <= 0x40F) return c + 80;
  return c;
}

inline std::string changeCase(const std::string& text, bool upper) {
  std::string out;
  out.reserve(text.size());
  const unsigned char* s = (const unsigned char*) text.data();
  size_t i = 0;
  while (i < text.size()) {
    const uint32_t cp = decodeCodePoint(s, text.size(), i);
    if (upper && cp == 0xDF) { // ß has no capital in one letter
      out += "SS";
      continue;
    }
    appendCodePoint(out, caseMap(cp, upper));
  }
  return out;
}

inline JsVar callbackArg(Interp& I, const Args& a, const char* what) {
  const JsVar& fn = a[0];
  if (!fn.isFunction()) I.throwType(I.toText(fn) + " is not a function" + what);
  return fn;
}

inline ArrayObject* thisArray(Interp& I, const JsVar& self, const char* name) {
  if (!self.isArray())
    I.throwType(std::string("Array.prototype.") + name + " called on a value that is not an array");
  I.ensureMetered(self);
  return static_cast<ArrayObject*>(Access::obj(self));
}

/** Merge sort, because std::sort with a comparator that is not a strict weak
 *  ordering -- `() => Math.random() - 0.5` is the classic -- is undefined
 *  behaviour, which in practice means reading past the end of the array. A
 *  merge sort reads each element where it is, whatever the answers. */
template <typename T, typename Less>
inline void mergeSort(std::vector<T>& v, Less less) {
  std::vector<T> tmp(v.size());
  for (size_t width = 1; width < v.size(); width *= 2) {
    for (size_t lo = 0; lo < v.size(); lo += 2 * width) {
      const size_t mid = (std::min)(lo + width, v.size());
      const size_t hi = (std::min)(lo + 2 * width, v.size());
      size_t i = lo, j = mid, k = lo;
      while (i < mid && j < hi) {
        if (less(v[j], v[i])) tmp[k++] = v[j++];
        else tmp[k++] = v[i++];
      }
      while (i < mid) tmp[k++] = v[i++];
      while (j < hi) tmp[k++] = v[j++];
    }
    v.swap(tmp);
  }
}

inline void Interp::installBuiltins() {
  objectProto_ = std::make_shared<DynamicObject>();
  objectProto_->explicitProto_ = true; // the end of every chain
  adopt(objectProto_);
  functionProto_ = newObjectWithProto(objectProto_);
  arrayProto_ = newObjectWithProto(objectProto_);
  stringProto_ = newObjectWithProto(objectProto_);
  numberProto_ = newObjectWithProto(objectProto_);
  booleanProto_ = newObjectWithProto(objectProto_);
  errorProto_ = newObjectWithProto(objectProto_);
  typeErrorProto_ = newObjectWithProto(errorProto_);
  rangeErrorProto_ = newObjectWithProto(errorProto_);
  referenceErrorProto_ = newObjectWithProto(errorProto_);
  syntaxErrorProto_ = newObjectWithProto(errorProto_);
  evalErrorProto_ = newObjectWithProto(errorProto_);
  global_ = newObjectWithProto(objectProto_);
  globalScope_ = newScope(nullptr);
  installGlobalBuiltins(*this);
  installObjectBuiltins(*this);
  installArrayBuiltins(*this);
  installStringBuiltins(*this);
  installNumberBuiltins(*this);
  installMathBuiltins(*this);
}

inline std::shared_ptr<FunctionObject> Interp::constructorFor(
    Interp& R, const std::string& name, int arity, Builtin fn,
    const std::shared_ptr<DynamicObject>& proto) {
  auto f = R.builtin(name, arity, fn);
  f->put("prototype", Access::fromObject(proto), 0);
  proto->put("constructor", Access::fromObject(f), 0);
  R.global_->put(name, Access::fromObject(f), 0);
  return f;
}

inline std::string functionSource(const FunctionObject* f) {
  if (f->type == FunctionObject::Type::Script && f->code && f->program) {
    const std::string& src = f->program->source;
    const size_t from = (std::min)(f->code->sourceStart, src.size());
    const size_t to = (std::min)((std::max)(f->code->sourceEnd, from), src.size());
    return src.substr(from, to - from);
  }
  return "function " + f->name + "() { [native code] }";
}

// ── Global functions, errors, console, JSON ────────────────────────────────

inline void Interp::installGlobalBuiltins(Interp& I) {
  auto& g = I.global_;
  g->put("undefined", JsVar(), DynamicObject::kReadOnly);
  g->put("NaN", JsVar(std::numeric_limits<double>::quiet_NaN()), DynamicObject::kReadOnly);
  g->put("Infinity", JsVar(std::numeric_limits<double>::infinity()), DynamicObject::kReadOnly);
  g->put("globalThis", Access::fromObject(g), 0);

  auto parseIntFn = I.builtin("parseInt", 2, [](Interp& I, const JsVar&, const Args& a) {
    const std::string text = I.toText(a[0]);
    return JsVar(parseIntText(text, a[1].isUndefined() ? 0.0 : I.toNumber(a[1])));
  });
  auto parseFloatFn = I.builtin("parseFloat", 1, [](Interp& I, const JsVar&, const Args& a) {
    return JsVar(parseFloatPrefix(I.toText(a[0])));
  });
  g->put("parseInt", Access::fromObject(parseIntFn), 0);
  g->put("parseFloat", Access::fromObject(parseFloatFn), 0);
  I.method(g, "isNaN", 1, [](Interp& I, const JsVar&, const Args& a) {
    const double d = I.toNumber(a[0]);
    return JsVar(d != d);
  });
  I.method(g, "isFinite", 1, [](Interp& I, const JsVar&, const Args& a) {
    return JsVar((bool) std::isfinite(I.toNumber(a[0])));
  });
  I.method(g, "eval", 1, [](Interp& I, const JsVar&, const Args& a) {
    if (!a[0].isString()) return a[0];
    return I.evaluateCode(Access::str(a[0])->text, false);
  });
  auto log = [](Interp& I, const JsVar&, const Args& a) {
    // One line is at most a script's longest string, whatever it shows.
    InspectBudget budget{I.maxString, 0};
    std::string line;
    for (int i = 0; i < a.n; ++i) {
      if (i) line += ' ';
      std::vector<const DynamicObject*> seen;
      const JsVar value = a[i];
      line += I.inspect(value, 2, seen, true, budget);
    }
    if (I.consoleHook) I.consoleHook(line);
    return JsVar();
  };
  I.method(g, "trace", 1, log); // the reference's name for it
  I.method(g, "charToInt", 1, [](Interp& I, const JsVar&, const Args& a) {
    JsVar s = I.toStr(a[0]);
    const Units u(Access::str(s));
    return u.size() ? JsVar((double) u.at(0)) : JsVar(0);
  });
  auto console = I.newObject();
  for (const char* name : {"log", "info", "warn", "error", "debug"}) I.method(console, name, 0, log);
  g->put("console", Access::fromObject(console), 0);

  // Errors.
  struct ErrorKind {
    const char* name;
    std::shared_ptr<DynamicObject> Interp::*proto;
  };
  const ErrorKind kinds[] = {
      {"Error", &Interp::errorProto_},           {"TypeError", &Interp::typeErrorProto_},
      {"RangeError", &Interp::rangeErrorProto_}, {"ReferenceError", &Interp::referenceErrorProto_},
      {"SyntaxError", &Interp::syntaxErrorProto_}, {"EvalError", &Interp::evalErrorProto_}};
  for (const ErrorKind& k : kinds) {
    const std::shared_ptr<DynamicObject>& proto = I.*(k.proto);
    proto->put("name", I.str(k.name), 0);
    proto->put("message", I.str(""), 0);
    // Each constructor finds its prototype through its own "prototype"
    // property, so one native serves all six.
    auto ctor = I.builtin(k.name, 1, [](Interp&, const JsVar&, const Args&) { return JsVar(); });
    std::weak_ptr<FunctionObject> weakCtor = ctor;
    ctor->native = [weakCtor](const JsVar::NativeFunctionArgs& a) -> JsVar {
      Interp* I = currentInterp();
      auto self = weakCtor.lock();
      if (!I || !self) return JsVar();
      const DynamicObject::Property* p = self->find("prototype");
      std::shared_ptr<DynamicObject> proto =
          p && Access::kind(p->value) >= Kind::Array ? Access::objPtr(p->value) : I->errorProto_;
      auto e = I->newObjectWithProto(proto);
      if (a.numArguments > 0 && !a[0].isUndefined()) e->put("message", I->toStr(a[0]), 0);
      return Access::fromObject(e);
    };
    ctor->put("prototype", Access::fromObject(proto), 0);
    proto->put("constructor", Access::fromObject(ctor), 0);
    g->put(k.name, Access::fromObject(ctor), 0);
  }
  I.method(I.errorProto_, "toString", 0, [](Interp& I, const JsVar& self, const Args&) {
    if (Access::kind(self) < Kind::Array)
      I.throwType("Error.prototype.toString called on a non-object");
    JsVar nameValue = I.getProp(self, "name");
    JsVar messageValue = I.getProp(self, "message");
    const std::string name = nameValue.isUndefined() ? "Error" : I.toText(nameValue);
    const std::string message = messageValue.isUndefined() ? "" : I.toText(messageValue);
    if (name.empty()) return I.str(message);
    if (message.empty()) return I.str(name);
    return I.str(name + ": " + message);
  });

  // JSON.
  auto json = I.newObject();
  I.method(json, "parse", 2, [](Interp& I, const JsVar&, const Args& a) {
    return I.parseJson(I.toText(a[0]));
  });
  I.method(json, "stringify", 3, [](Interp& I, const JsVar&, const Args& a) {
    std::string indent;
    JsVar space = a[2];
    if (space.isNumber()) {
      const double n = (std::min)(10.0, toInteger(Access::num(space)));
      if (n >= 1) indent.assign((size_t) n, ' ');
    } else if (space.isString()) {
      const Units u(Access::str(space));
      indent = Access::str(u.slice(I, 0, (std::min)((size_t) 10, u.size())))->text;
    }
    std::string out;
    if (!I.stringifyJson(a[0], a[1], indent, out)) return JsVar();
    return I.str(std::move(out));
  });
  g->put("JSON", Access::fromObject(json), 0);
}

// ── Object, Function ────────────────────────────────────────────────────────

inline void Interp::installObjectBuiltins(Interp& I) {
  auto objectCtor = constructorFor(I, "Object", 1, [](Interp& I, const JsVar&, const Args& a) {
    if (Access::kind(a[0]) >= Kind::Array) return a[0];
    return Access::fromObject(I.newObject());
  }, I.objectProto_);
  I.method(objectCtor, "keys", 1, [](Interp& I, const JsVar&, const Args& a) {
    if (a[0].isVoid()) I.throwType("Cannot convert undefined or null to object");
    std::vector<JsVar> out;
    for (const std::string& k : I.ownKeys(a[0])) out.push_back(I.str(k));
    return I.arrayOf(std::move(out));
  });
  I.method(objectCtor, "values", 1, [](Interp& I, const JsVar&, const Args& a) {
    if (a[0].isVoid()) I.throwType("Cannot convert undefined or null to object");
    std::vector<JsVar> out;
    for (const std::string& k : I.ownKeys(a[0])) out.push_back(I.getProp(a[0], k));
    return I.arrayOf(std::move(out));
  });
  I.method(objectCtor, "entries", 1, [](Interp& I, const JsVar&, const Args& a) {
    if (a[0].isVoid()) I.throwType("Cannot convert undefined or null to object");
    std::vector<JsVar> out;
    for (const std::string& k : I.ownKeys(a[0])) {
      std::vector<JsVar> pair{I.str(k), I.getProp(a[0], k)};
      out.push_back(I.arrayOf(std::move(pair)));
    }
    return I.arrayOf(std::move(out));
  });
  I.method(objectCtor, "assign", 2, [](Interp& I, const JsVar&, const Args& a) {
    if (a[0].isVoid()) I.throwType("Cannot convert undefined or null to object");
    for (int i = 1; i < a.n; ++i) {
      if (a[i].isVoid()) continue;
      for (const std::string& k : I.ownKeys(a[i])) I.setProp(a[0], k, I.getProp(a[i], k));
    }
    return a[0];
  });
  I.method(objectCtor, "create", 1, [](Interp& I, const JsVar&, const Args& a) {
    if (!a[0].isNull() && Access::kind(a[0]) < Kind::Array)
      I.throwType("Object prototype may only be an Object or null: " + I.toText(a[0]));
    return Access::fromObject(I.newObjectWithProto(a[0].isNull() ? nullptr : Access::objPtr(a[0])));
  });
  I.method(objectCtor, "getPrototypeOf", 1, [](Interp& I, const JsVar&, const Args& a) {
    if (a[0].isVoid()) I.throwType("Cannot convert undefined or null to object");
    const DynamicObject* p = nullptr;
    switch (Access::kind(a[0])) {
      case Kind::Boolean: p = I.booleanProto_.get(); break;
      case Kind::Number: p = I.numberProto_.get(); break;
      case Kind::String: p = I.stringProto_.get(); break;
      default: p = I.protoOf(Access::obj(a[0])); break;
    }
    if (!p) return JsVar(nullptr);
    // A prototype is reachable from a root, so a non-owning handle to it is
    // safe; a shared one is found the slow way, through whoever owns it.
    for (const auto& candidate :
         {I.objectProto_, I.functionProto_, I.arrayProto_, I.stringProto_, I.numberProto_,
          I.booleanProto_, I.errorProto_, I.typeErrorProto_, I.rangeErrorProto_,
          I.referenceErrorProto_, I.syntaxErrorProto_, I.evalErrorProto_})
      if (candidate.get() == p) return Access::fromObject(candidate);
    const DynamicObject* o = Access::obj(a[0]);
    return o && o->explicitProto_ ? Access::fromObject(o->proto_) : JsVar(nullptr);
  });
  I.method(objectCtor, "fromEntries", 1, [](Interp& I, const JsVar&, const Args& a) {
    auto o = I.newObject();
    if (a[0].isArray()) {
      std::vector<JsVar> entries = static_cast<ArrayObject*>(Access::obj(a[0]))->elements;
      for (const JsVar& e : entries) {
        if (Access::kind(e) < Kind::Array) I.throwType("Iterator value " + I.toText(e) + " is not an entry object");
        const std::string key = I.keyOf(I.getIndex(e, JsVar(0)));
        I.ensureRoom(key.size() + sizeof(DynamicObject::Property) + 64);
        o->setProperty(key, I.getIndex(e, JsVar(1)));
      }
    } else if (!a[0].isVoid()) {
      I.throwType(I.toText(a[0]) + " is not iterable");
    }
    return Access::fromObject(o);
  });
  I.method(objectCtor, "getOwnPropertyNames", 1, [](Interp& I, const JsVar&, const Args& a) {
    if (a[0].isVoid()) I.throwType("Cannot convert undefined or null to object");
    std::vector<JsVar> out;
    if (a[0].isString()) {
      for (size_t i = 0; i < Access::str(a[0])->units; ++i) out.push_back(I.str(std::to_string(i)));
      out.push_back(I.str("length"));
    } else if (Access::kind(a[0]) >= Kind::Array) {
      const DynamicObject* o = Access::obj(a[0]);
      if (a[0].isArray()) {
        const size_t n = static_cast<const ArrayObject*>(o)->elements.size();
        for (size_t i = 0; i < n; ++i) out.push_back(I.str(std::to_string(i)));
      }
      for (const std::string& k : o->keysInOrder(false)) out.push_back(I.str(k));
      if (a[0].isArray()) out.push_back(I.str("length"));
    }
    return I.arrayOf(std::move(out));
  });
  I.method(objectCtor, "freeze", 1, [](Interp&, const JsVar&, const Args& a) {
    if (Access::kind(a[0]) == Kind::Object || Access::kind(a[0]) == Kind::Function)
      for (auto& p : Access::obj(a[0])->props_) p.flags |= DynamicObject::kReadOnly;
    return a[0];
  });

  auto& op = I.objectProto_;
  I.method(op, "hasOwnProperty", 1, [](Interp& I, const JsVar& self, const Args& a) {
    const std::string k = I.keyOf(a[0]);
    if (self.isVoid()) I.throwType("Cannot convert undefined or null to object");
    if (self.isString()) {
      uint32_t i = 0;
      return JsVar(k == "length" || (isIndexText(k, &i) && i < Access::str(self)->units));
    }
    if (Access::kind(self) < Kind::Array) return JsVar(false);
    const DynamicObject* o = Access::obj(self);
    if (self.isArray()) {
      uint32_t i = 0;
      if (k == "length") return JsVar(true);
      if (isIndexText(k, &i)) return JsVar(i < static_cast<const ArrayObject*>(o)->elements.size());
    }
    if (self.isFunction() && (k == "name" || k == "length")) return JsVar(true);
    return JsVar(o->find(k) != nullptr);
  });
  I.method(op, "isPrototypeOf", 1, [](Interp& I, const JsVar& self, const Args& a) {
    if (Access::kind(a[0]) < Kind::Array || Access::kind(self) < Kind::Array) return JsVar(false);
    const DynamicObject* target = Access::obj(self);
    for (const DynamicObject* p = I.protoOf(Access::obj(a[0])); p; p = I.protoOf(p))
      if (p == target) return JsVar(true);
    return JsVar(false);
  });
  I.method(op, "propertyIsEnumerable", 1, [](Interp& I, const JsVar& self, const Args& a) {
    const std::string k = I.keyOf(a[0]);
    if (self.isArray()) {
      uint32_t i = 0;
      if (isIndexText(k, &i)) return JsVar(i < static_cast<ArrayObject*>(Access::obj(self))->elements.size());
    }
    if (Access::kind(self) < Kind::Array) return JsVar(false);
    const DynamicObject::Property* p = Access::obj(self)->find(k);
    return JsVar(p && (p->flags & DynamicObject::kEnumerable));
  });
  auto objectToString = [](Interp& I, const JsVar& self, const Args&) {
    const char* tag = "Object";
    switch (Access::kind(self)) {
      case Kind::Undefined: tag = "Undefined"; break;
      case Kind::Null: tag = "Null"; break;
      case Kind::Boolean: tag = "Boolean"; break;
      case Kind::Number: tag = "Number"; break;
      case Kind::String: tag = "String"; break;
      case Kind::Array: tag = "Array"; break;
      case Kind::Function: tag = "Function"; break;
      default:
        for (const DynamicObject* p = I.protoOf(Access::obj(self)); p; p = I.protoOf(p))
          if (p == I.errorProto_.get()) tag = "Error";
        break;
    }
    return I.str(std::string("[object ") + tag + "]");
  };
  I.method(op, "toString", 0, objectToString);
  I.method(op, "toLocaleString", 0, [](Interp& I, const JsVar& self, const Args&) {
    JsVar fn = I.getProp(self, "toString");
    return I.call(fn, self, nullptr, 0);
  });
  I.method(op, "valueOf", 0, [](Interp& I, const JsVar& self, const Args&) {
    if (self.isVoid()) I.throwType("Cannot convert undefined or null to object");
    return self;
  });

  // Function.prototype.
  constructorFor(I, "Function", 1, [](Interp& I, const JsVar&, const Args&) -> JsVar {
    I.throwType("the Function constructor is not supported; write a function expression");
  }, I.functionProto_);
  auto& fp = I.functionProto_;
  I.method(fp, "call", 1, [](Interp& I, const JsVar& self, const Args& a) {
    if (!self.isFunction()) I.throwType("Function.prototype.call called on a non-function");
    return I.call(self, a[0], a.n > 1 ? a.v + 1 : nullptr, a.n > 1 ? (size_t) a.n - 1 : 0);
  });
  I.method(fp, "apply", 2, [](Interp& I, const JsVar& self, const Args& a) {
    if (!self.isFunction()) I.throwType("Function.prototype.apply called on a non-function");
    std::vector<JsVar> args;
    if (a[1].isArray()) {
      args = static_cast<ArrayObject*>(Access::obj(a[1]))->elements;
    } else if (!a[1].isVoid()) {
      if (Access::kind(a[1]) < Kind::Array) I.throwType("CreateListFromArrayLike called on non-object");
      const double len = toInteger(I.toNumber(I.getProp(a[1], "length")));
      if (len > (double) I.maxArray) I.throwRange("Invalid array length");
      for (size_t i = 0; i < (size_t) (std::max)(0.0, len); ++i)
        args.push_back(I.getProp(a[1], std::to_string(i)));
    }
    return I.call(self, a[0], args.data(), args.size());
  });
  I.method(fp, "bind", 1, [](Interp& I, const JsVar& self, const Args& a) {
    if (!self.isFunction()) I.throwType("Bind must be called on a function");
    const FunctionObject* target = static_cast<const FunctionObject*>(Access::obj(self));
    auto f = std::make_shared<FunctionObject>();
    f->type = FunctionObject::Type::Bound;
    f->boundTarget = self;
    f->boundThis = a[0];
    if (a.n > 1) f->boundArgs.assign(a.v + 1, a.v + a.n);
    f->name = "bound " + target->name;
    f->arity = (std::max)(0, target->arity - (a.n > 1 ? a.n - 1 : 0));
    I.adopt(f);
    return Access::fromObject(f);
  });
  I.method(fp, "toString", 0, [](Interp& I, const JsVar& self, const Args&) {
    if (!self.isFunction()) I.throwType("Function.prototype.toString requires that 'this' be a Function");
    return I.str(functionSource(static_cast<const FunctionObject*>(Access::obj(self))));
  });

  // Boolean.
  constructorFor(I, "Boolean", 1, [](Interp&, const JsVar&, const Args& a) {
    return JsVar(a[0].toBool());
  }, I.booleanProto_);
  I.method(I.booleanProto_, "toString", 0, [](Interp& I, const JsVar& self, const Args&) {
    if (!self.isBool()) I.throwType("Boolean.prototype.toString requires that 'this' be a Boolean");
    return I.str(self.toBool() ? "true" : "false");
  });
  I.method(I.booleanProto_, "valueOf", 0, [](Interp& I, const JsVar& self, const Args&) {
    if (!self.isBool()) I.throwType("Boolean.prototype.valueOf requires that 'this' be a Boolean");
    return self;
  });
}

// ── Array ───────────────────────────────────────────────────────────────────

inline void Interp::installArrayBuiltins(Interp& I) {
  auto arrayCtor = constructorFor(I, "Array", 1, [](Interp& I, const JsVar&, const Args& a) {
    if (a.n == 1 && a[0].isNumber()) {
      const double d = Access::num(a[0]);
      const uint32_t u = toUint32(d);
      if ((double) u != d) I.throwRange("Invalid array length");
      auto arr = I.newArray();
      I.growArray(arr.get(), u);
      return Access::fromObject(arr);
    }
    return I.arrayOf(std::vector<JsVar>(a.v, a.v + a.n));
  }, I.arrayProto_);
  I.method(arrayCtor, "isArray", 1, [](Interp&, const JsVar&, const Args& a) {
    return JsVar(a[0].isArray());
  });
  I.method(arrayCtor, "of", 0, [](Interp& I, const JsVar&, const Args& a) {
    return I.arrayOf(std::vector<JsVar>(a.v, a.v + a.n));
  });
  I.method(arrayCtor, "from", 1, [](Interp& I, const JsVar&, const Args& a) {
    std::vector<JsVar> items;
    if (a[0].isVoid()) I.throwType(I.toText(a[0]) + " is not iterable");
    if (a[0].isArray() || a[0].isString()) {
      I.spreadInto(a[0], items, nullptr);
    } else if (Access::kind(a[0]) >= Kind::Array) {
      const double len = toInteger(I.toNumber(I.getProp(a[0], "length")));
      if (len > (double) I.maxArray) I.throwRange("Invalid array length");
      I.ensureRoom((size_t) (std::max)(0.0, len) * sizeof(JsVar));
      for (size_t i = 0; i < (size_t) (std::max)(0.0, len); ++i)
        items.push_back(I.getProp(a[0], std::to_string(i)));
    }
    if (!a[1].isUndefined()) {
      if (!a[1].isFunction()) I.throwType(I.toText(a[1]) + " is not a function");
      for (size_t i = 0; i < items.size(); ++i) {
        JsVar argv[2] = {items[i], JsVar((double) i)};
        items[i] = I.call(a[1], a[2], argv, 2);
      }
    }
    return I.arrayOf(std::move(items));
  });

  auto& ap = I.arrayProto_;
  I.method(ap, "push", 1, [](Interp& I, const JsVar& self, const Args& a) {
    ArrayObject* arr = thisArray(I, self, "push");
    for (int i = 0; i < a.n; ++i) I.push(arr, a[i]);
    return JsVar((double) arr->elements.size());
  });
  I.method(ap, "pop", 0, [](Interp& I, const JsVar& self, const Args&) {
    ArrayObject* arr = thisArray(I, self, "pop");
    if (arr->elements.empty()) return JsVar();
    JsVar v = std::move(arr->elements.back());
    arr->elements.pop_back();
    return v;
  });
  I.method(ap, "shift", 0, [](Interp& I, const JsVar& self, const Args&) {
    ArrayObject* arr = thisArray(I, self, "shift");
    if (arr->elements.empty()) return JsVar();
    JsVar v = std::move(arr->elements.front());
    arr->elements.erase(arr->elements.begin());
    return v;
  });
  I.method(ap, "unshift", 1, [](Interp& I, const JsVar& self, const Args& a) {
    ArrayObject* arr = thisArray(I, self, "unshift");
    if (arr->elements.size() + (size_t) a.n > I.maxArray) I.throwRange("Invalid array length");
    I.ensureRoom((size_t) a.n * sizeof(JsVar));
    arr->elements.insert(arr->elements.begin(), a.v, a.v + a.n);
    arr->touch();
    return JsVar((double) arr->elements.size());
  });
  I.method(ap, "slice", 2, [](Interp& I, const JsVar& self, const Args& a) {
    ArrayObject* arr = thisArray(I, self, "slice");
    const size_t len = arr->elements.size();
    const size_t from = relativeIndex(argInteger(I, a[0]), len);
    const size_t to = a[1].isUndefined() ? len : relativeIndex(argInteger(I, a[1]), len);
    const auto& e = arr->elements;
    if (to <= from || from >= e.size()) return I.arrayOf({});
    I.ensureRoom(((std::min)(to, e.size()) - from) * sizeof(JsVar));
    return I.arrayOf(std::vector<JsVar>(e.begin() + (std::ptrdiff_t) from, e.begin() + (std::ptrdiff_t) (std::min)(to, e.size())));
  });
  I.method(ap, "splice", 2, [](Interp& I, const JsVar& self, const Args& a) {
    ArrayObject* arr = thisArray(I, self, "splice");
    const size_t len = arr->elements.size();
    const size_t start = relativeIndex(argInteger(I, a[0]), len);
    size_t count = 0;
    if (a.n == 0) count = 0;
    else if (a.n == 1) count = len - start;
    else count = clampIndex(argInteger(I, a[1]), len - start);
    const size_t inserting = a.n > 2 ? (size_t) a.n - 2 : 0;
    if (len - count + inserting > I.maxArray) I.throwRange("Invalid array length");
    auto& e = arr->elements;
    const size_t s = (std::min)(start, e.size());
    const size_t c = (std::min)(count, e.size() - s);
    I.ensureRoom((c + inserting) * sizeof(JsVar));
    std::vector<JsVar> removed(e.begin() + (std::ptrdiff_t) s, e.begin() + (std::ptrdiff_t) (s + c));
    e.erase(e.begin() + (std::ptrdiff_t) s, e.begin() + (std::ptrdiff_t) (s + c));
    if (inserting) e.insert(e.begin() + (std::ptrdiff_t) s, a.v + 2, a.v + a.n);
    arr->touch();
    return I.arrayOf(std::move(removed));
  });
  I.method(ap, "concat", 1, [](Interp& I, const JsVar& self, const Args& a) {
    ArrayObject* arr = thisArray(I, self, "concat");
    I.ensureRoom(arr->elements.size() * sizeof(JsVar));
    std::vector<JsVar> out = arr->elements;
    for (int i = 0; i < a.n; ++i) {
      if (a[i].isArray()) {
        const auto& e = static_cast<ArrayObject*>(Access::obj(a[i]))->elements;
        if (out.size() + e.size() > I.maxArray) I.throwRange("Invalid array length");
        I.ensureRoom(e.size() * sizeof(JsVar));
        out.insert(out.end(), e.begin(), e.end());
      } else {
        out.push_back(a[i]);
      }
    }
    return I.arrayOf(std::move(out));
  });
  I.method(ap, "join", 1, [](Interp& I, const JsVar& self, const Args& a) {
    thisArray(I, self, "join");
    return I.str(I.join(self, a[0].isUndefined() ? std::string(",") : I.toText(a[0])));
  });
  I.method(ap, "toString", 0, [](Interp& I, const JsVar& self, const Args&) {
    if (!self.isArray()) return I.str("[object Object]");
    return I.str(I.join(self, ","));
  });
  I.method(ap, "reverse", 0, [](Interp& I, const JsVar& self, const Args&) {
    ArrayObject* arr = thisArray(I, self, "reverse");
    std::reverse(arr->elements.begin(), arr->elements.end());
    return self;
  });
  I.method(ap, "indexOf", 1, [](Interp& I, const JsVar& self, const Args& a) {
    ArrayObject* arr = thisArray(I, self, "indexOf");
    const size_t from = relativeIndex(argInteger(I, a[1]), arr->elements.size());
    for (size_t i = from; i < arr->elements.size(); ++i)
      if (Interp::strictEquals(arr->elements[i], a[0])) return JsVar((double) i);
    return JsVar(-1);
  });
  I.method(ap, "lastIndexOf", 1, [](Interp& I, const JsVar& self, const Args& a) {
    ArrayObject* arr = thisArray(I, self, "lastIndexOf");
    const size_t len = arr->elements.size();
    if (!len) return JsVar(-1);
    double from = a.n > 1 ? argInteger(I, a[1]) : (double) len - 1;
    if (from < 0) from += (double) len;
    if (from < 0) return JsVar(-1);
    for (size_t i = (size_t) (std::min)(from, (double) len - 1) + 1; i-- > 0;)
      if (i < arr->elements.size() && Interp::strictEquals(arr->elements[i], a[0])) return JsVar((double) i);
    return JsVar(-1);
  });
  auto includes = [](Interp& I, const JsVar& self, const Args& a) {
    ArrayObject* arr = thisArray(I, self, "includes");
    const size_t from = relativeIndex(argInteger(I, a[1]), arr->elements.size());
    const bool nan = a[0].isNumber() && Access::num(a[0]) != Access::num(a[0]);
    for (size_t i = from; i < arr->elements.size(); ++i) {
      const JsVar& e = arr->elements[i];
      if (Interp::strictEquals(e, a[0]) || (nan && e.isNumber() && Access::num(e) != Access::num(e)))
        return JsVar(true);
    }
    return JsVar(false);
  };
  I.method(ap, "includes", 1, includes);
  I.method(ap, "contains", 1, includes); // the reference's name
  I.method(ap, "remove", 1, [](Interp& I, const JsVar& self, const Args& a) {
    // The reference's Array.remove: every element equal to the value goes.
    ArrayObject* arr = thisArray(I, self, "remove");
    auto& e = arr->elements;
    e.erase(std::remove_if(e.begin(), e.end(),
                           [&a](const JsVar& v) { return Interp::strictEquals(v, a[0]); }),
            e.end());
    return JsVar();
  });
  I.method(ap, "at", 1, [](Interp& I, const JsVar& self, const Args& a) {
    ArrayObject* arr = thisArray(I, self, "at");
    double k = argInteger(I, a[0]);
    if (k < 0) k += (double) arr->elements.size();
    if (k < 0 || k >= (double) arr->elements.size()) return JsVar();
    return arr->elements[(size_t) k];
  });
  I.method(ap, "fill", 1, [](Interp& I, const JsVar& self, const Args& a) {
    ArrayObject* arr = thisArray(I, self, "fill");
    const size_t len = arr->elements.size();
    const size_t from = relativeIndex(argInteger(I, a[1]), len);
    const size_t to = a[2].isUndefined() ? len : relativeIndex(argInteger(I, a[2]), len);
    for (size_t i = from; i < to && i < arr->elements.size(); ++i) arr->elements[i] = a[0];
    return self;
  });

  // Iteration with a callback: the length is read once, as the language
  // says, and every element is read afresh -- the callback may change the
  // array under the loop, and the loop must stay inside it.
  I.method(ap, "forEach", 1, [](Interp& I, const JsVar& self, const Args& a) {
    ArrayObject* arr = thisArray(I, self, "forEach");
    JsVar fn = callbackArg(I, a, "");
    const size_t len = arr->elements.size();
    for (size_t i = 0; i < len && i < arr->elements.size(); ++i) {
      JsVar argv[3] = {arr->elements[i], JsVar((double) i), self};
      I.call(fn, a[1], argv, 3);
    }
    return JsVar();
  });
  I.method(ap, "map", 1, [](Interp& I, const JsVar& self, const Args& a) {
    ArrayObject* arr = thisArray(I, self, "map");
    JsVar fn = callbackArg(I, a, "");
    const size_t len = arr->elements.size();
    I.ensureRoom(len * sizeof(JsVar));
    std::vector<JsVar> out(len);
    for (size_t i = 0; i < len && i < arr->elements.size(); ++i) {
      JsVar argv[3] = {arr->elements[i], JsVar((double) i), self};
      out[i] = I.call(fn, a[1], argv, 3);
    }
    return I.arrayOf(std::move(out));
  });
  I.method(ap, "filter", 1, [](Interp& I, const JsVar& self, const Args& a) {
    ArrayObject* arr = thisArray(I, self, "filter");
    JsVar fn = callbackArg(I, a, "");
    const size_t len = arr->elements.size();
    std::vector<JsVar> out;
    for (size_t i = 0; i < len && i < arr->elements.size(); ++i) {
      JsVar e = arr->elements[i];
      JsVar argv[3] = {e, JsVar((double) i), self};
      if (I.call(fn, a[1], argv, 3).toBool()) out.push_back(e);
    }
    return I.arrayOf(std::move(out));
  });
  struct Finder {
    static JsVar run(Interp& I, const JsVar& self, const Args& a, bool fromEnd, bool wantIndex,
                     const char* name) {
      ArrayObject* arr = thisArray(I, self, name);
      JsVar fn = callbackArg(I, a, "");
      const size_t len = arr->elements.size();
      for (size_t k = 0; k < len; ++k) {
        const size_t i = fromEnd ? len - 1 - k : k;
        JsVar e = i < arr->elements.size() ? arr->elements[i] : JsVar();
        JsVar argv[3] = {e, JsVar((double) i), self};
        if (I.call(fn, a[1], argv, 3).toBool()) return wantIndex ? JsVar((double) i) : e;
      }
      return wantIndex ? JsVar(-1) : JsVar();
    }
  };
  I.method(ap, "find", 1, [](Interp& I, const JsVar& self, const Args& a) {
    return Finder::run(I, self, a, false, false, "find");
  });
  I.method(ap, "findIndex", 1, [](Interp& I, const JsVar& self, const Args& a) {
    return Finder::run(I, self, a, false, true, "findIndex");
  });
  I.method(ap, "findLast", 1, [](Interp& I, const JsVar& self, const Args& a) {
    return Finder::run(I, self, a, true, false, "findLast");
  });
  I.method(ap, "findLastIndex", 1, [](Interp& I, const JsVar& self, const Args& a) {
    return Finder::run(I, self, a, true, true, "findLastIndex");
  });
  I.method(ap, "some", 1, [](Interp& I, const JsVar& self, const Args& a) {
    ArrayObject* arr = thisArray(I, self, "some");
    JsVar fn = callbackArg(I, a, "");
    const size_t len = arr->elements.size();
    for (size_t i = 0; i < len && i < arr->elements.size(); ++i) {
      JsVar argv[3] = {arr->elements[i], JsVar((double) i), self};
      if (I.call(fn, a[1], argv, 3).toBool()) return JsVar(true);
    }
    return JsVar(false);
  });
  I.method(ap, "every", 1, [](Interp& I, const JsVar& self, const Args& a) {
    ArrayObject* arr = thisArray(I, self, "every");
    JsVar fn = callbackArg(I, a, "");
    const size_t len = arr->elements.size();
    for (size_t i = 0; i < len && i < arr->elements.size(); ++i) {
      JsVar argv[3] = {arr->elements[i], JsVar((double) i), self};
      if (!I.call(fn, a[1], argv, 3).toBool()) return JsVar(false);
    }
    return JsVar(true);
  });
  struct Reducer {
    static JsVar run(Interp& I, const JsVar& self, const Args& a, bool fromEnd) {
      ArrayObject* arr = thisArray(I, self, fromEnd ? "reduceRight" : "reduce");
      JsVar fn = callbackArg(I, a, "");
      const size_t len = arr->elements.size();
      size_t k = 0;
      JsVar acc;
      if (a.n >= 2) {
        acc = a[1];
      } else {
        if (len == 0) I.throwType("Reduce of empty array with no initial value");
        acc = arr->elements[fromEnd ? len - 1 : 0];
        k = 1;
      }
      for (; k < len; ++k) {
        const size_t i = fromEnd ? len - 1 - k : k;
        if (i >= arr->elements.size()) continue;
        JsVar argv[4] = {acc, arr->elements[i], JsVar((double) i), self};
        acc = I.call(fn, JsVar(), argv, 4);
      }
      return acc;
    }
  };
  I.method(ap, "reduce", 1, [](Interp& I, const JsVar& self, const Args& a) {
    return Reducer::run(I, self, a, false);
  });
  I.method(ap, "reduceRight", 1, [](Interp& I, const JsVar& self, const Args& a) {
    return Reducer::run(I, self, a, true);
  });
  I.method(ap, "sort", 1, [](Interp& I, const JsVar& self, const Args& a) {
    ArrayObject* arr = thisArray(I, self, "sort");
    JsVar cmp = a[0];
    if (!cmp.isUndefined() && !cmp.isFunction())
      I.throwType("The comparison function must be either a function or undefined");
    I.ensureRoom(arr->elements.size() * sizeof(JsVar) * 3);
    std::vector<JsVar> items = arr->elements;
    // undefined sorts last and is never passed to the comparator.
    std::vector<JsVar> defined;
    size_t undefinedCount = 0;
    for (JsVar& v : items) {
      if (v.isUndefined()) ++undefinedCount;
      else defined.push_back(std::move(v));
    }
    if (cmp.isFunction()) {
      mergeSort(defined, [&I, &cmp](const JsVar& x, const JsVar& y) {
        JsVar argv[2] = {x, y};
        const double r = I.toNumber(I.call(cmp, JsVar(), argv, 2));
        return r < 0;
      });
    } else {
      // Compared as strings, each converted once.
      std::vector<JsVar> texts;
      for (const JsVar& v : defined) texts.push_back(I.toStr(v));
      std::vector<size_t> order(defined.size());
      for (size_t i = 0; i < order.size(); ++i) order[i] = i;
      mergeSort(order, [&texts](size_t x, size_t y) {
        return Interp::compareText(Access::str(texts[x]), Access::str(texts[y])) < 0;
      });
      std::vector<JsVar> sorted;
      sorted.reserve(order.size());
      for (size_t i : order) sorted.push_back(defined[i]);
      defined.swap(sorted);
    }
    defined.resize(defined.size() + undefinedCount);
    arr->elements.swap(defined);
    arr->touch();
    return self;
  });
  struct Flattener {
    static void run(Interp& I, const JsVar& v, double depth, std::vector<JsVar>& out) {
      I.checkStack();
      for (const JsVar& e : static_cast<ArrayObject*>(Access::obj(v))->elements) {
        if (e.isArray() && depth >= 1) {
          JsVar keep = e;
          run(I, keep, depth - 1, out);
        } else {
          if (out.size() >= I.maxArray) I.throwRange("Invalid array length");
          if ((out.size() & 0xFFFF) == 0xFFFF) I.ensureRoom(out.size() * sizeof(JsVar));
          out.push_back(e);
        }
      }
    }
  };
  I.method(ap, "flat", 0, [](Interp& I, const JsVar& self, const Args& a) {
    thisArray(I, self, "flat");
    const double depth = a[0].isUndefined() ? 1.0 : argInteger(I, a[0]);
    std::vector<JsVar> out;
    Flattener::run(I, self, depth, out);
    return I.arrayOf(std::move(out));
  });
  I.method(ap, "flatMap", 1, [](Interp& I, const JsVar& self, const Args& a) {
    ArrayObject* arr = thisArray(I, self, "flatMap");
    JsVar fn = callbackArg(I, a, "");
    std::vector<JsVar> out;
    const size_t len = arr->elements.size();
    for (size_t i = 0; i < len && i < arr->elements.size(); ++i) {
      JsVar argv[3] = {arr->elements[i], JsVar((double) i), self};
      JsVar r = I.call(fn, a[1], argv, 3);
      if (r.isArray()) Flattener::run(I, r, 0, out);
      else out.push_back(r);
      if (out.size() > I.maxArray) I.throwRange("Invalid array length");
    }
    return I.arrayOf(std::move(out));
  });
}

// ── String ──────────────────────────────────────────────────────────────────

inline void Interp::installStringBuiltins(Interp& I) {
  auto stringCtor = constructorFor(I, "String", 1, [](Interp& I, const JsVar&, const Args& a) {
    if (a.n == 0) return I.str(std::string());
    return I.toStr(a[0]);
  }, I.stringProto_);
  I.method(stringCtor, "fromCharCode", 1, [](Interp& I, const JsVar&, const Args& a) {
    std::u16string u;
    for (int i = 0; i < a.n; ++i) u += (char16_t) (toUint32(I.toNumber(a[i])) & 0xFFFF);
    I.checkLength(u.size() * 3);
    return I.strUnits(u.data(), u.size());
  });
  I.method(stringCtor, "fromCodePoint", 1, [](Interp& I, const JsVar&, const Args& a) {
    std::string out;
    for (int i = 0; i < a.n; ++i) {
      const double d = I.toNumber(a[i]);
      if (d != std::floor(d) || d < 0 || d > 0x10FFFF)
        I.throwRange("Invalid code point " + numberToString(d));
      appendCodePoint(out, (uint32_t) d);
    }
    return I.str(canonicalText(out));
  });

  auto& sp = I.stringProto_;
  auto stringSelf = [](Interp& I, const JsVar& self, const Args&) {
    if (!self.isString()) I.throwType("String.prototype.toString requires that 'this' be a String");
    return self;
  };
  I.method(sp, "toString", 0, stringSelf);
  I.method(sp, "valueOf", 0, stringSelf);
  I.method(sp, "charAt", 1, [](Interp& I, const JsVar& self, const Args& a) {
    JsVar keep;
    const Units u(thisString(I, self, "charAt", keep));
    const double i = argInteger(I, a[0]);
    if (i < 0 || i >= (double) u.size()) return I.str(std::string());
    return u.slice(I, (size_t) i, (size_t) i + 1);
  });
  I.method(sp, "charCodeAt", 1, [](Interp& I, const JsVar& self, const Args& a) {
    JsVar keep;
    const Units u(thisString(I, self, "charCodeAt", keep));
    const double i = argInteger(I, a[0]);
    if (i < 0 || i >= (double) u.size()) return JsVar(std::numeric_limits<double>::quiet_NaN());
    return JsVar((double) u.at((size_t) i));
  });
  I.method(sp, "codePointAt", 1, [](Interp& I, const JsVar& self, const Args& a) {
    JsVar keep;
    const Units u(thisString(I, self, "codePointAt", keep));
    const double d = argInteger(I, a[0]);
    if (d < 0 || d >= (double) u.size()) return JsVar();
    const size_t i = (size_t) d;
    const char16_t c = u.at(i);
    if (c >= 0xD800 && c <= 0xDBFF && i + 1 < u.size()) {
      const char16_t lo = u.at(i + 1);
      if (lo >= 0xDC00 && lo <= 0xDFFF) return JsVar((double) (0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00)));
    }
    return JsVar((double) c);
  });
  I.method(sp, "at", 1, [](Interp& I, const JsVar& self, const Args& a) {
    JsVar keep;
    const Units u(thisString(I, self, "at", keep));
    double k = argInteger(I, a[0]);
    if (k < 0) k += (double) u.size();
    if (k < 0 || k >= (double) u.size()) return JsVar();
    return u.slice(I, (size_t) k, (size_t) k + 1);
  });
  I.method(sp, "indexOf", 1, [](Interp& I, const JsVar& self, const Args& a) {
    JsVar keep, needle = I.toStr(a[0]);
    const Units u(thisString(I, self, "indexOf", keep));
    const Units n(Access::str(needle));
    const size_t from = clampIndex(argInteger(I, a[1]), u.size());
    const size_t at = u.find(I, n, from);
    return JsVar(at == std::string::npos ? -1.0 : (double) at);
  });
  I.method(sp, "lastIndexOf", 1, [](Interp& I, const JsVar& self, const Args& a) {
    JsVar keep, needle = I.toStr(a[0]);
    const Units u(thisString(I, self, "lastIndexOf", keep));
    const Units n(Access::str(needle));
    const double pos = I.toNumber(a[1]);
    const size_t from = pos != pos ? u.size() : clampIndex(toInteger(pos), u.size());
    const size_t at = u.rfind(I, n, from);
    return JsVar(at == std::string::npos ? -1.0 : (double) at);
  });
  I.method(sp, "includes", 1, [](Interp& I, const JsVar& self, const Args& a) {
    JsVar keep, needle = I.toStr(a[0]);
    const Units u(thisString(I, self, "includes", keep));
    const Units n(Access::str(needle));
    return JsVar(u.find(I, n, clampIndex(argInteger(I, a[1]), u.size())) != std::string::npos);
  });
  I.method(sp, "startsWith", 1, [](Interp& I, const JsVar& self, const Args& a) {
    JsVar keep, needle = I.toStr(a[0]);
    const Units u(thisString(I, self, "startsWith", keep));
    const Units n(Access::str(needle));
    const size_t from = clampIndex(argInteger(I, a[1]), u.size());
    if (from + n.size() > u.size()) return JsVar(false);
    for (size_t i = 0; i < n.size(); ++i)
      if (u.at(from + i) != n.at(i)) return JsVar(false);
    return JsVar(true);
  });
  I.method(sp, "endsWith", 1, [](Interp& I, const JsVar& self, const Args& a) {
    JsVar keep, needle = I.toStr(a[0]);
    const Units u(thisString(I, self, "endsWith", keep));
    const Units n(Access::str(needle));
    const size_t end = a[1].isUndefined() ? u.size() : clampIndex(argInteger(I, a[1]), u.size());
    if (n.size() > end) return JsVar(false);
    for (size_t i = 0; i < n.size(); ++i)
      if (u.at(end - n.size() + i) != n.at(i)) return JsVar(false);
    return JsVar(true);
  });
  I.method(sp, "slice", 2, [](Interp& I, const JsVar& self, const Args& a) {
    JsVar keep;
    const Units u(thisString(I, self, "slice", keep));
    const size_t from = relativeIndex(argInteger(I, a[0]), u.size());
    const size_t to = a[1].isUndefined() ? u.size() : relativeIndex(argInteger(I, a[1]), u.size());
    return u.slice(I, from, to);
  });
  I.method(sp, "substring", 2, [](Interp& I, const JsVar& self, const Args& a) {
    JsVar keep;
    const Units u(thisString(I, self, "substring", keep));
    size_t from = clampIndex(argInteger(I, a[0]), u.size());
    size_t to = a[1].isUndefined() ? u.size() : clampIndex(argInteger(I, a[1]), u.size());
    if (from > to) std::swap(from, to);
    return u.slice(I, from, to);
  });
  I.method(sp, "substr", 2, [](Interp& I, const JsVar& self, const Args& a) {
    JsVar keep;
    const Units u(thisString(I, self, "substr", keep));
    const size_t from = relativeIndex(argInteger(I, a[0]), u.size());
    const double len = a[1].isUndefined() ? (double) u.size() : argInteger(I, a[1]);
    const size_t count = clampIndex(len, u.size() - from);
    return u.slice(I, from, from + count);
  });
  I.method(sp, "toUpperCase", 0, [](Interp& I, const JsVar& self, const Args&) {
    JsVar keep;
    const StringRep* r = thisString(I, self, "toUpperCase", keep);
    I.checkLength(r->text.size() * 2);
    return I.str(changeCase(r->text, true));
  });
  I.method(sp, "toLowerCase", 0, [](Interp& I, const JsVar& self, const Args&) {
    JsVar keep;
    const StringRep* r = thisString(I, self, "toLowerCase", keep);
    return I.str(changeCase(r->text, false));
  });
  I.method(sp, "toLocaleUpperCase", 0, [](Interp& I, const JsVar& self, const Args&) {
    JsVar keep;
    const StringRep* r = thisString(I, self, "toLocaleUpperCase", keep);
    I.checkLength(r->text.size() * 2);
    return I.str(changeCase(r->text, true));
  });
  I.method(sp, "toLocaleLowerCase", 0, [](Interp& I, const JsVar& self, const Args&) {
    JsVar keep;
    const StringRep* r = thisString(I, self, "toLocaleLowerCase", keep);
    return I.str(changeCase(r->text, false));
  });
  struct Trim {
    static JsVar run(Interp& I, const JsVar& self, bool start, bool end, const char* name) {
      JsVar keep;
      const StringRep* r = thisString(I, self, name, keep);
      size_t from = 0, to = 0;
      trimBounds(r->text, start, end, from, to);
      return I.str(r->text.substr(from, to - from));
    }
  };
  I.method(sp, "trim", 0, [](Interp& I, const JsVar& self, const Args&) {
    return Trim::run(I, self, true, true, "trim");
  });
  I.method(sp, "trimStart", 0, [](Interp& I, const JsVar& self, const Args&) {
    return Trim::run(I, self, true, false, "trimStart");
  });
  I.method(sp, "trimEnd", 0, [](Interp& I, const JsVar& self, const Args&) {
    return Trim::run(I, self, false, true, "trimEnd");
  });
  struct Pad {
    static JsVar run(Interp& I, const JsVar& self, const Args& a, bool atStart) {
      JsVar keep;
      const StringRep* r = thisString(I, self, atStart ? "padStart" : "padEnd", keep);
      const double target = argInteger(I, a[0]);
      if (target <= (double) r->units) return keep;
      JsVar fillValue = a[1].isUndefined() ? I.str(" ") : I.toStr(a[1]);
      const Units fill(Access::str(fillValue));
      if (fill.size() == 0) return keep;
      const size_t need = (size_t) (std::min)(target, 4294967295.0) - r->units;
      I.checkLength(need * 3 + r->text.size());
      std::u16string pad;
      const std::u16string f = fill.units();
      while (pad.size() < need) pad += f;
      pad.resize(need);
      const std::string padText = fromUtf16(pad);
      std::string out;
      if (atStart) {
        out = padText;
        I.appendText(out, r->text);
      } else {
        out = r->text;
        I.appendText(out, padText);
      }
      return I.str(std::move(out));
    }
  };
  I.method(sp, "padStart", 2, [](Interp& I, const JsVar& self, const Args& a) {
    return Pad::run(I, self, a, true);
  });
  I.method(sp, "padEnd", 2, [](Interp& I, const JsVar& self, const Args& a) {
    return Pad::run(I, self, a, false);
  });
  I.method(sp, "repeat", 1, [](Interp& I, const JsVar& self, const Args& a) {
    JsVar keep;
    const StringRep* r = thisString(I, self, "repeat", keep);
    const double n = argInteger(I, a[0]);
    if (n < 0 || std::isinf(n)) I.throwRange("Invalid count value: " + numberToString(n));
    if (n == 0 || r->text.empty()) return I.str(std::string());
    if (n * (double) r->text.size() > (double) I.maxString) I.throwRange("Invalid string length");
    I.checkLength((size_t) n * r->text.size());
    std::string out;
    out.reserve((size_t) n * r->text.size());
    for (size_t i = 0; i < (size_t) n; ++i) I.appendText(out, r->text);
    return I.str(std::move(out));
  });
  I.method(sp, "concat", 1, [](Interp& I, const JsVar& self, const Args& a) {
    JsVar keep;
    thisString(I, self, "concat", keep);
    JsVar acc = keep;
    for (int i = 0; i < a.n; ++i) {
      JsVar piece = I.toStr(a[i]);
      acc = I.concat(acc, piece);
    }
    return acc;
  });
  I.method(sp, "localeCompare", 1, [](Interp& I, const JsVar& self, const Args& a) {
    JsVar keep, other = I.toStr(a[0]);
    const StringRep* r = thisString(I, self, "localeCompare", keep);
    return JsVar((double) Interp::compareText(r, Access::str(other)));
  });
  I.method(sp, "normalize", 0, [](Interp& I, const JsVar& self, const Args&) {
    JsVar keep;
    thisString(I, self, "normalize", keep);
    return keep;
  });
  I.method(sp, "split", 2, [](Interp& I, const JsVar& self, const Args& a) {
    JsVar keep;
    const Units u(thisString(I, self, "split", keep));
    const uint32_t limit = a[1].isUndefined() ? 4294967295u : toUint32(I.toNumber(a[1]));
    std::vector<JsVar> out;
    if (limit == 0) return I.arrayOf({});
    if (a[0].isUndefined()) {
      out.push_back(keep);
      return I.arrayOf(std::move(out));
    }
    JsVar sepValue = I.toStr(a[0]);
    const Units sep(Access::str(sepValue));
    if (sep.size() == 0) {
      if (u.size() > I.maxArray) I.throwRange("Invalid array length");
      for (size_t i = 0; i < u.size() && out.size() < limit; ++i) {
        out.push_back(u.slice(I, i, i + 1));
        if ((i & 0xFFF) == 0xFFF) I.interruptPoint();
      }
      return I.arrayOf(std::move(out));
    }
    size_t from = 0;
    for (;;) {
      const size_t at = u.find(I, sep, from);
      if (at == std::string::npos) break;
      if ((out.size() & 0xFFF) == 0xFFF) I.interruptPoint();
      out.push_back(u.slice(I, from, at));
      if (out.size() >= limit) return I.arrayOf(std::move(out));
      if (out.size() >= I.maxArray) I.throwRange("Invalid array length");
      from = at + sep.size();
    }
    out.push_back(u.slice(I, from, u.size()));
    return I.arrayOf(std::move(out));
  });
  struct Replace {
    /** GetSubstitution, for a pattern that is a string: $$, $&, $` and $'.
     *
     *  Each reference copies part of the subject, so a few kilobytes of
     *  "$`$`$`..." after a megabyte of text asked for gigabytes, made in full
     *  before the result's length was checked: 3 GB for one call under a
     *  64 MB limit. And a reference that copies nothing ($& of an empty
     *  match) still costs its step, once per match, with no clock read:
     *  replaceAll("", "$&" x 2,000,000) on 1,000 characters ran 24 s past a
     *  1 s limit. So the piece is held to the `room` the result has left, and
     *  `steps`, counted across every match, brings the time limit and stop()
     *  in as the replacement is walked. */
    static std::u16string substitute(Interp& I, const std::u16string& replacement, const std::u16string& subject,
                                     size_t at, size_t length, size_t room, size_t& steps) {
      std::u16string out;
      // `out` never passes `room`, so room - out.size() cannot wrap.
      const auto fits = [&](size_t count) {
        if (count > room - out.size()) I.throwRange("Invalid string length");
      };
      for (size_t i = 0; i < replacement.size(); ++i) {
        if ((++steps & 0xFFFF) == 0) I.interruptPoint();
        const char16_t c = replacement[i];
        if (c == u'$' && i + 1 < replacement.size()) {
          const char16_t n = replacement[i + 1];
          size_t from = 0, count = 0;
          bool reference = true;
          if (n == u'&') {
            from = at;
            count = length;
          } else if (n == u'`') {
            count = at;
          } else if (n == u'\'') {
            from = (std::min)(at + length, subject.size());
            count = subject.size() - from;
          } else {
            reference = false;
          }
          if (reference) {
            fits(count);
            out.append(subject, from, count);
            ++i;
            continue;
          }
          if (n == u'$') ++i; // "$$" is one '$'
        }
        fits(1);
        out += c;
      }
      return out;
    }
    static JsVar run(Interp& I, const JsVar& self, const Args& a, bool all) {
      JsVar keep;
      const StringRep* r = thisString(I, self, all ? "replaceAll" : "replace", keep);
      JsVar patternValue = I.toStr(a[0]);
      const std::u16string subject = Units(r).units();
      const std::u16string pattern = Units(Access::str(patternValue)).units();
      const bool functional = a[1].isFunction();
      std::u16string replacement;
      if (!functional) {
        JsVar rv = I.toStr(a[1]);
        replacement = Units(Access::str(rv)).units();
      }
      std::vector<size_t> positions;
      size_t from = 0;
      for (;;) {
        const size_t at =
            searchUnits<char16_t>(I, subject.data(), subject.size(), pattern.data(), pattern.size(), from, false);
        if (at == std::u16string::npos || at > subject.size()) break;
        positions.push_back(at);
        if (!all) break;
        if ((positions.size() & 0xFFF) == 0xFFF) {
          I.interruptPoint();
          I.ensureRoom(positions.size() * sizeof(size_t));
        }
        from = at + (pattern.empty() ? 1 : pattern.size());
        if (from > subject.size()) break;
      }
      if (positions.empty()) return keep;
      std::u16string out;
      size_t last = 0, steps = 0;
      const size_t most = I.maxString / 3; // units; the check below, as a count
      for (size_t at : positions) {
        out.append(subject, last, at - last);
        std::u16string piece;
        if (functional) {
          JsVar argv[3] = {I.strUnits(pattern.data(), pattern.size()), JsVar((double) at), keep};
          JsVar rv = I.toStr(I.call(a[1], JsVar(), argv, 3));
          piece = Units(Access::str(rv)).units();
        } else {
          piece = substitute(I, replacement, subject, at, pattern.size(),
                             most > out.size() ? most - out.size() : 0, steps);
        }
        if ((out.size() + piece.size()) * 3 > I.maxString) I.throwRange("Invalid string length");
        out += piece;
        last = at + pattern.size();
      }
      out.append(subject, (std::min)(last, subject.size()), std::u16string::npos);
      I.checkLength(out.size() * 3);
      return I.strUnits(out.data(), out.size());
    }
  };
  I.method(sp, "replace", 2, [](Interp& I, const JsVar& self, const Args& a) {
    return Replace::run(I, self, a, false);
  });
  I.method(sp, "replaceAll", 2, [](Interp& I, const JsVar& self, const Args& a) {
    return Replace::run(I, self, a, true);
  });
}

// ── Number ──────────────────────────────────────────────────────────────────

inline double thisNumber(Interp& I, const JsVar& self, const char* name) {
  if (!self.isNumber())
    I.throwType(std::string("Number.prototype.") + name + " requires that 'this' be a Number");
  return Access::num(self);
}

inline void Interp::installNumberBuiltins(Interp& I) {
  auto numberCtor = constructorFor(I, "Number", 1, [](Interp& I, const JsVar&, const Args& a) {
    return JsVar(a.n == 0 ? 0.0 : I.toNumber(a[0]));
  }, I.numberProto_);
  numberCtor->put("MAX_SAFE_INTEGER", JsVar(9007199254740991.0), DynamicObject::kReadOnly);
  numberCtor->put("MIN_SAFE_INTEGER", JsVar(-9007199254740991.0), DynamicObject::kReadOnly);
  numberCtor->put("MAX_VALUE", JsVar((std::numeric_limits<double>::max)()), DynamicObject::kReadOnly);
  numberCtor->put("MIN_VALUE", JsVar(std::numeric_limits<double>::denorm_min()), DynamicObject::kReadOnly);
  numberCtor->put("EPSILON", JsVar(std::numeric_limits<double>::epsilon()), DynamicObject::kReadOnly);
  numberCtor->put("POSITIVE_INFINITY", JsVar(std::numeric_limits<double>::infinity()), DynamicObject::kReadOnly);
  numberCtor->put("NEGATIVE_INFINITY", JsVar(-std::numeric_limits<double>::infinity()), DynamicObject::kReadOnly);
  numberCtor->put("NaN", JsVar(std::numeric_limits<double>::quiet_NaN()), DynamicObject::kReadOnly);
  numberCtor->put("parseFloat", I.global_->getProperty("parseFloat"), 0);
  numberCtor->put("parseInt", I.global_->getProperty("parseInt"), 0);
  I.method(numberCtor, "isInteger", 1, [](Interp&, const JsVar&, const Args& a) {
    if (!a[0].isNumber()) return JsVar(false);
    const double d = Access::num(a[0]);
    return JsVar(std::isfinite(d) && d == std::floor(d));
  });
  I.method(numberCtor, "isSafeInteger", 1, [](Interp&, const JsVar&, const Args& a) {
    if (!a[0].isNumber()) return JsVar(false);
    const double d = Access::num(a[0]);
    return JsVar(std::isfinite(d) && d == std::floor(d) && std::fabs(d) <= 9007199254740991.0);
  });
  I.method(numberCtor, "isFinite", 1, [](Interp&, const JsVar&, const Args& a) {
    return JsVar(a[0].isNumber() && std::isfinite(Access::num(a[0])));
  });
  I.method(numberCtor, "isNaN", 1, [](Interp&, const JsVar&, const Args& a) {
    return JsVar(a[0].isNumber() && Access::num(a[0]) != Access::num(a[0]));
  });

  auto& np = I.numberProto_;
  I.method(np, "toString", 1, [](Interp& I, const JsVar& self, const Args& a) {
    const double v = thisNumber(I, self, "toString");
    double radix = 10;
    if (!a[0].isUndefined()) radix = argInteger(I, a[0]);
    if (radix < 2 || radix > 36) I.throwRange("toString() radix must be between 2 and 36");
    return I.str(numberToRadix(v, (int) radix));
  });
  I.method(np, "toLocaleString", 0, [](Interp& I, const JsVar& self, const Args&) {
    return I.str(numberToString(thisNumber(I, self, "toLocaleString")));
  });
  I.method(np, "valueOf", 0, [](Interp& I, const JsVar& self, const Args&) {
    return JsVar(thisNumber(I, self, "valueOf"));
  });
  I.method(np, "toFixed", 1, [](Interp& I, const JsVar& self, const Args& a) {
    const double v = thisNumber(I, self, "toFixed");
    const double digits = argInteger(I, a[0]);
    if (digits < 0 || digits > 100) I.throwRange("toFixed() digits argument must be between 0 and 100");
    return I.str(numberToFixed(v, (int) digits));
  });
  I.method(np, "toPrecision", 1, [](Interp& I, const JsVar& self, const Args& a) {
    const double v = thisNumber(I, self, "toPrecision");
    if (a[0].isUndefined()) return I.str(numberToString(v));
    const double p = argInteger(I, a[0]);
    if (!std::isfinite(v)) return I.str(numberToString(v));
    if (p < 1 || p > 100) I.throwRange("toPrecision() argument must be between 1 and 100");
    return I.str(numberToPrecision(v, (int) p));
  });
  I.method(np, "toExponential", 1, [](Interp& I, const JsVar& self, const Args& a) {
    const double v = thisNumber(I, self, "toExponential");
    const double f = argInteger(I, a[0]);
    if (!std::isfinite(v)) return I.str(numberToString(v));
    if (f < 0 || f > 100) I.throwRange("toExponential() argument must be between 0 and 100");
    return I.str(numberToExponential(v, (int) f, a[0].isUndefined()));
  });
}

// ── Math ────────────────────────────────────────────────────────────────────

/** Math.cbrt. glibc's cbrt misses some exact cubes by an ulp (27 gave
 *  3.0000000000000004) where V8's and MSVC's do not; of the result and its
 *  two neighbours, the one whose cube is nearest x, the cube measured to
 *  well past a double's precision with fma. */
inline double cubeRoot(double x) noexcept {
  const double r = std::cbrt(x);
  if (!std::isfinite(r) || r == 0.0) return r;
  auto miss = [x](double y) {
    const double y2 = y * y, y2lo = std::fma(y, y, -y2);
    const double y3 = y2 * y, y3lo = std::fma(y2, y, -y3) + y2lo * y;
    return std::fabs((y3 - x) + y3lo);
  };
  double best = r, bestMiss = miss(r);
  for (const double y : {std::nextafter(r, -HUGE_VAL), std::nextafter(r, HUGE_VAL)}) {
    const double m = miss(y);
    if (m < bestMiss) {
      best = y;
      bestMiss = m;
    }
  }
  return best;
}

inline void Interp::installMathBuiltins(Interp& I) {
  auto math = I.newObject();
  I.global_->put("Math", Access::fromObject(math), 0);
  const struct {
    const char* name;
    double value;
  } constants[] = {{"PI", 3.141592653589793},      {"E", 2.718281828459045},
                   {"LN2", 0.6931471805599453},     {"LN10", 2.302585092994046},
                   {"LOG2E", 1.4426950408889634},   {"LOG10E", 0.4342944819032518},
                   {"SQRT2", 1.4142135623730951},   {"SQRT1_2", 0.7071067811865476}};
  for (const auto& c : constants) math->put(c.name, JsVar(c.value), DynamicObject::kReadOnly);

#define SONORE_JS_MATH1(NAME, EXPR)                                                   \
  I.method(math, NAME, 1, [](Interp& I, const JsVar&, const Args& a) {                \
    const double x = I.toNumber(a[0]);                                                \
    return JsVar((double) (EXPR));                                                    \
  })
  SONORE_JS_MATH1("abs", std::fabs(x));
  SONORE_JS_MATH1("acos", std::acos(x));
  SONORE_JS_MATH1("acosh", std::acosh(x));
  SONORE_JS_MATH1("asin", std::asin(x));
  SONORE_JS_MATH1("asinh", std::asinh(x));
  SONORE_JS_MATH1("atan", std::atan(x));
  SONORE_JS_MATH1("atanh", std::atanh(x));
  SONORE_JS_MATH1("cbrt", cubeRoot(x));
  SONORE_JS_MATH1("ceil", std::ceil(x));
  SONORE_JS_MATH1("cos", std::cos(x));
  SONORE_JS_MATH1("cosh", std::cosh(x));
  SONORE_JS_MATH1("exp", std::exp(x));
  SONORE_JS_MATH1("expm1", std::expm1(x));
  SONORE_JS_MATH1("floor", std::floor(x));
  SONORE_JS_MATH1("fround", (double) (float) x);
  SONORE_JS_MATH1("log", std::log(x));
  SONORE_JS_MATH1("log10", std::log10(x));
  SONORE_JS_MATH1("log1p", std::log1p(x));
  SONORE_JS_MATH1("log2", std::log2(x));
  SONORE_JS_MATH1("sin", std::sin(x));
  SONORE_JS_MATH1("sinh", std::sinh(x));
  SONORE_JS_MATH1("sqrt", std::sqrt(x));
  SONORE_JS_MATH1("tan", std::tan(x));
  SONORE_JS_MATH1("tanh", std::tanh(x));
  SONORE_JS_MATH1("trunc", std::trunc(x));
  SONORE_JS_MATH1("sign", (x != x) ? x : (x > 0 ? 1.0 : x < 0 ? -1.0 : x));
  // The reference's additions.
  SONORE_JS_MATH1("sqr", x * x);
  SONORE_JS_MATH1("toDegrees", x * (180.0 / 3.141592653589793));
  SONORE_JS_MATH1("toRadians", x * (3.141592653589793 / 180.0));
#undef SONORE_JS_MATH1

  I.method(math, "round", 1, [](Interp& I, const JsVar&, const Args& a) {
    const double x = I.toNumber(a[0]);
    if (!std::isfinite(x) || x == 0) return JsVar(x);
    // floor(x + 0.5), but without the rounding error x + 0.5 itself makes:
    // 0.49999999999999994 + 0.5 is 1 in doubles, and Math.round of it is 0.
    double r = std::floor(x);
    if (x - r >= 0.5) r += 1.0;
    if (r == 0 && x < 0) return JsVar(-0.0);
    return JsVar(r);
  });
  I.method(math, "clz32", 1, [](Interp& I, const JsVar&, const Args& a) {
    uint32_t v = toUint32(I.toNumber(a[0]));
    int n = 0;
    if (v == 0) return JsVar(32);
    while (!(v & 0x80000000u)) {
      v <<= 1;
      ++n;
    }
    return JsVar(n);
  });
  I.method(math, "atan2", 2, [](Interp& I, const JsVar&, const Args& a) {
    const double y = I.toNumber(a[0]);
    const double x = I.toNumber(a[1]);
    return JsVar(std::atan2(y, x));
  });
  I.method(math, "pow", 2, [](Interp& I, const JsVar&, const Args& a) {
    const double x = I.toNumber(a[0]);
    const double y = I.toNumber(a[1]);
    return JsVar(Interp::jsPow(x, y));
  });
  I.method(math, "imul", 2, [](Interp& I, const JsVar&, const Args& a) {
    const uint32_t x = toUint32(I.toNumber(a[0]));
    const uint32_t y = toUint32(I.toNumber(a[1]));
    return JsVar((double) (int32_t) (x * y));
  });
  I.method(math, "max", 2, [](Interp& I, const JsVar&, const Args& a) {
    std::vector<double> v;
    for (int i = 0; i < a.n; ++i) v.push_back(I.toNumber(a[i]));
    double r = -std::numeric_limits<double>::infinity();
    for (double d : v) {
      if (d != d) return JsVar(d);
      if (d > r || (d == 0 && r == 0 && !std::signbit(d))) r = d;
    }
    return JsVar(r);
  });
  I.method(math, "min", 2, [](Interp& I, const JsVar&, const Args& a) {
    std::vector<double> v;
    for (int i = 0; i < a.n; ++i) v.push_back(I.toNumber(a[i]));
    double r = std::numeric_limits<double>::infinity();
    for (double d : v) {
      if (d != d) return JsVar(d);
      if (d < r || (d == 0 && r == 0 && std::signbit(d))) r = d;
    }
    return JsVar(r);
  });
  I.method(math, "hypot", 2, [](Interp& I, const JsVar&, const Args& a) {
    std::vector<double> v;
    for (int i = 0; i < a.n; ++i) v.push_back(I.toNumber(a[i]));
    bool nan = false;
    double biggest = 0;
    for (double d : v) {
      if (std::isinf(d)) return JsVar(std::numeric_limits<double>::infinity());
      if (d != d) nan = true;
      else biggest = (std::max)(biggest, std::fabs(d));
    }
    if (nan) return JsVar(std::numeric_limits<double>::quiet_NaN());
    if (biggest == 0) return JsVar(0);
    double sum = 0;
    for (double d : v) sum += (d / biggest) * (d / biggest);
    return JsVar(std::sqrt(sum) * biggest);
  });
  I.method(math, "random", 0, [](Interp& I, const JsVar&, const Args&) {
    return JsVar(I.random());
  });
  I.method(math, "randInt", 2, [](Interp& I, const JsVar&, const Args& a) {
    // The reference's: an integer in [a, b).
    const double lo = std::floor(I.toNumber(a[0]));
    const double hi = std::floor(I.toNumber(a[1]));
    if (!(hi > lo)) return JsVar(lo);
    return JsVar(lo + std::floor(I.random() * (hi - lo)));
  });
  I.method(math, "range", 3, [](Interp& I, const JsVar&, const Args& a) {
    // The reference's: the first argument limited to [second, third].
    const double v = I.toNumber(a[0]);
    const double lo = I.toNumber(a[1]);
    const double hi = I.toNumber(a[2]);
    return JsVar(v < lo ? lo : v > hi ? hi : v);
  });
}

} // namespace js
} // namespace detail

// ── DynamicObject ───────────────────────────────────────────────────────────

inline DynamicObject::~DynamicObject() {
  std::vector<std::shared_ptr<detail::js::GcNode>> stack;
  for (Property& p : props_) detail::js::Access::dropRef(p.value, stack);
  props_.clear();
  if (proto_) stack.push_back(std::move(proto_));
  detail::js::releaseIteratively(stack);
  if (meter_) meter_->bytes -= charged_;
}

inline void DynamicObject::gcVisit(std::vector<const detail::js::GcNode*>& out) const {
  for (const Property& p : props_) detail::js::Access::visit(p.value, out);
  if (proto_) out.push_back(proto_.get());
}

inline void DynamicObject::gcDrop(std::vector<std::shared_ptr<detail::js::GcNode>>& out) {
  for (Property& p : props_) detail::js::Access::dropRef(p.value, out);
  props_.clear();
  index_.reset();
  keyBytes_ = 0;
  if (proto_) out.push_back(std::move(proto_));
  proto_.reset();
}

inline void DynamicObject::recharge() {
  if (!meter_) return;
  const long long now =
      (long long) (sizeof(DynamicObject) + 64 + props_.capacity() * sizeof(Property)) + keyBytes_ +
      (index_ ? (long long) props_.size() * 48 + keyBytes_ : 0) + extraBytes();
  meter_->bytes += now - charged_;
  charged_ = now;
}

inline int DynamicObject::findIndex(const std::string& name) const {
  if (index_) {
    auto it = index_->find(name);
    return it == index_->end() ? -1 : (int) it->second;
  }
  for (size_t i = 0; i < props_.size(); ++i)
    if (props_[i].key.size() == name.size() && props_[i].key == name) return (int) i;
  return -1;
}

inline DynamicObject::Property* DynamicObject::find(const std::string& name) {
  const int i = findIndex(name);
  return i < 0 ? nullptr : &props_[(size_t) i];
}

inline const DynamicObject::Property* DynamicObject::find(const std::string& name) const {
  const int i = findIndex(name);
  return i < 0 ? nullptr : &props_[(size_t) i];
}

inline void DynamicObject::rebuildIndex() {
  if (props_.size() <= 8) {
    index_.reset();
    return;
  }
  if (!index_) index_.reset(new std::unordered_map<std::string, uint32_t>());
  index_->clear();
  index_->reserve(props_.size());
  for (size_t i = 0; i < props_.size(); ++i) (*index_)[props_[i].key] = (uint32_t) i;
}

inline void DynamicObject::put(const std::string& name, const JsVar& value, uint8_t flags) {
  const int i = findIndex(name);
  if (i >= 0) {
    props_[(size_t) i].value = value;
    props_[(size_t) i].flags = flags;
    return;
  }
  props_.push_back(Property{name, value, flags});
  keyBytes_ += (long long) name.size();
  if (index_) (*index_)[name] = (uint32_t) (props_.size() - 1);
  else if (props_.size() > 8) rebuildIndex();
  recharge();
}

inline std::vector<std::string> DynamicObject::keysInOrder(bool enumerableOnly) const {
  // ECMAScript's own-key order: integer keys ascending, then the rest in the
  // order they were added.
  std::vector<std::pair<uint32_t, size_t>> indices;
  std::vector<std::string> keys;
  keys.reserve(props_.size());
  for (size_t i = 0; i < props_.size(); ++i) {
    if (enumerableOnly && !(props_[i].flags & kEnumerable)) continue;
    uint32_t n = 0;
    if (detail::js::isIndexText(props_[i].key, &n)) indices.push_back({n, i});
  }
  std::sort(indices.begin(), indices.end());
  for (const auto& p : indices) keys.push_back(props_[p.second].key);
  for (size_t i = 0; i < props_.size(); ++i) {
    if (enumerableOnly && !(props_[i].flags & kEnumerable)) continue;
    if (!detail::js::isIndexText(props_[i].key, nullptr)) keys.push_back(props_[i].key);
  }
  return keys;
}

inline bool DynamicObject::hasProperty(const std::string& name) const {
  return findIndex(name) >= 0;
}

inline JsVar DynamicObject::getProperty(const std::string& name) const {
  const Property* p = find(name);
  return p ? p->value : JsVar();
}

inline void DynamicObject::setProperty(const std::string& name, const JsVar& value) {
  if (Property* p = find(name)) {
    p->value = value;
    return;
  }
  put(name, value, kEnumerable);
}

inline bool DynamicObject::removeProperty(const std::string& name) {
  const int i = findIndex(name);
  if (i < 0) return false;
  keyBytes_ -= (long long) props_[(size_t) i].key.size();
  JsVar dying = std::move(props_[(size_t) i].value);
  props_.erase(props_.begin() + i);
  if (index_) rebuildIndex();
  recharge();
  return true;
}

inline bool DynamicObject::hasMethod(const std::string& name) const {
  const Property* p = find(name);
  return p && p->value.isFunction();
}

inline void DynamicObject::setMethod(const std::string& name, JsVar::NativeFunction function) {
  auto f = std::make_shared<detail::js::FunctionObject>();
  f->type = detail::js::FunctionObject::Type::Native;
  f->native = std::move(function);
  f->name = name;
  put(name, JsVar(std::shared_ptr<DynamicObject>(std::move(f))), 0);
}

inline JsVar DynamicObject::invokeMethod(const JsVar& thisObject, const std::string& name,
                                         const std::vector<JsVar>& args) const {
  const Property* p = find(name);
  if (!p || !p->value.isMethod()) return JsVar();
  JsVar fn = p->value; // kept alive while it runs
  auto* f = static_cast<detail::js::FunctionObject*>(detail::js::Access::obj(fn));
  JsVar::NativeFunction native = f->native;
  if (!native) return JsVar();
  return native(JsVar::NativeFunctionArgs(thisObject, args.data(), (int) args.size()));
}

inline void DynamicObject::clear() {
  std::vector<std::shared_ptr<detail::js::GcNode>> stack;
  for (Property& p : props_) detail::js::Access::dropRef(p.value, stack);
  props_.clear();
  index_.reset();
  keyBytes_ = 0;
  recharge();
  detail::js::releaseIteratively(stack);
}

inline std::vector<std::string> DynamicObject::getPropertyNames() const { return keysInOrder(false); }

inline DynamicObject::Ptr DynamicObject::clone() const {
  // A function's clone is a plain object with its properties: a function's
  // code is shared, never copied.
  Ptr copy = cellKind_ == JsVar::Kind::Array
                 ? Ptr(std::make_shared<detail::js::ArrayObject>())
                 : std::make_shared<DynamicObject>();
  detail::js::Access::cloneValue(JsVar(), this, copy);
  return copy;
}

namespace detail {
namespace js {

/** A deep copy without recursion: a worklist of (original, copy) pairs, and
 *  a memo so shared structure stays shared and a cycle stays a cycle. With a
 *  seed, the copy of `seedSource` is `seedCopy` (DynamicObject::clone, which
 *  has no JsVar of itself to start from). */
inline JsVar Access::cloneValue(const JsVar& root, const DynamicObject* seedSource,
                                const std::shared_ptr<DynamicObject>& seedCopy) {
  using Kind = JsVar::Kind;
  std::unordered_map<const DynamicObject*, JsVar> memo;
  std::vector<std::pair<const DynamicObject*, DynamicObject*>> work;
  auto copyOf = [&](const JsVar& v) -> JsVar {
    const Kind k = Access::kind(v);
    if (k != Kind::Array && k != Kind::Object) return v;
    const DynamicObject* src = Access::obj(v);
    auto it = memo.find(src);
    if (it != memo.end()) return it->second;
    std::shared_ptr<DynamicObject> dst;
    if (k == Kind::Array) dst = std::make_shared<ArrayObject>();
    else dst = std::make_shared<DynamicObject>();
    JsVar result(dst);
    memo[src] = result;
    work.push_back({src, dst.get()});
    return result;
  };
  JsVar out;
  if (seedSource && seedCopy) {
    out = JsVar(seedCopy);
    memo[seedSource] = out;
    work.push_back({seedSource, seedCopy.get()});
  } else {
    out = copyOf(root);
  }
  while (!work.empty()) {
    const DynamicObject* src = work.back().first;
    DynamicObject* dst = work.back().second;
    work.pop_back();
    if (src->cellKind_ == Kind::Array) {
      const auto& from = static_cast<const ArrayObject*>(src)->elements;
      auto& to = static_cast<ArrayObject*>(dst)->elements;
      to.resize(from.size());
      for (size_t i = 0; i < from.size(); ++i) to[i] = copyOf(from[i]);
    }
    for (const auto& p : src->props_) dst->put(p.key, copyOf(p.value), p.flags);
    dst->explicitProto_ = src->explicitProto_;
    dst->proto_ = src->proto_;
  }
  return out;
}

struct PlainBudget {
  size_t visits = kPlainTextLimit;
  bool cut = false;
};

/** Appends `piece` to `out` up to kPlainTextLimit bytes, the cut falling on a
 *  character boundary. */
inline void appendPlainPiece(std::string& out, const std::string& piece, PlainBudget& budget) {
  if (budget.cut) return;
  if (out.size() + piece.size() <= kPlainTextLimit) {
    Interp::appendText(out, piece);
    return;
  }
  size_t take = kPlainTextLimit - (std::min)(out.size(), kPlainTextLimit);
  while (take > 0 && ((unsigned char) piece[take] & 0xC0) == 0x80) --take;
  out.append(piece, 0, take);
  budget.cut = true;
}

/** ToString without an engine, appended to `out`: no script is called, an
 *  object is "[object Object]", and a cycle joins as empty. */
inline void appendPlainText(const JsVar& v, std::vector<const DynamicObject*>& joining, std::string& out,
                            PlainBudget& budget) {
  using Kind = JsVar::Kind;
  if (budget.cut) return;
  switch (Access::kind(v)) {
    case Kind::String: appendPlainPiece(out, Access::str(v)->text, budget); return;
    case Kind::Number: appendPlainPiece(out, numberToString(Access::num(v)), budget); return;
    case Kind::Boolean: appendPlainPiece(out, Access::num(v) != 0 ? "true" : "false", budget); return;
    case Kind::Undefined: appendPlainPiece(out, "undefined", budget); return;
    case Kind::Null: appendPlainPiece(out, "null", budget); return;
    case Kind::Function:
      appendPlainPiece(out, functionSource(static_cast<const FunctionObject*>(Access::obj(v))), budget);
      return;
    case Kind::Object: appendPlainPiece(out, "[object Object]", budget); return;
    case Kind::Array: break;
  }
  const ArrayObject* a = static_cast<const ArrayObject*>(Access::obj(v));
  if (std::find(joining.begin(), joining.end(), a) != joining.end() || joining.size() > 200) return;
  // A million references to one long chain of one-element arrays write only
  // commas, but visit hundreds of millions of arrays: visits are counted too.
  if (budget.visits == 0) {
    budget.cut = true;
    return;
  }
  --budget.visits;
  joining.push_back(a);
  for (size_t i = 0; i < a->elements.size() && !budget.cut; ++i) {
    if (i) appendPlainPiece(out, ",", budget);
    if (!a->elements[i].isVoid()) appendPlainText(a->elements[i], joining, out, budget);
  }
  joining.pop_back();
}

inline std::string plainText(const JsVar& v, std::vector<const DynamicObject*>& joining) {
  if (Access::kind(v) == JsVar::Kind::String) return Access::str(v)->text; // whole, as it was made
  std::string out;
  PlainBudget budget;
  appendPlainText(v, joining, out, budget);
  if (budget.cut) out += "...";
  return out;
}

/** `budget` is kPlainTextLimit to start with: each value costs 16 bytes, a
 *  string or a key its length too, and once it is spent the rest is null --
 *  a JsonValue cannot share, so shared structure is copied every time. */
inline JsonValue Access::toJsonValue(const JsVar& v, std::vector<const DynamicObject*>& stack,
                                     size_t& budget) {
  using Kind = JsVar::Kind;
  const size_t own = 16 + (Access::kind(v) == Kind::String ? Access::str(v)->text.size() : 0);
  if (own > budget) {
    budget = 0;
    return JsonValue();
  }
  budget -= own;
  switch (Access::kind(v)) {
    case Kind::Boolean: return JsonValue(v.toBool());
    case Kind::Number: {
      const double d = Access::num(v);
      return std::isfinite(d) ? JsonValue(d) : JsonValue();
    }
    case Kind::String: return JsonValue(Access::str(v)->text);
    case Kind::Array:
    case Kind::Object: break;
    default: return JsonValue();
  }
  const DynamicObject* o = Access::obj(v);
  if (std::find(stack.begin(), stack.end(), o) != stack.end() ||
      stack.size() >= (size_t) JsonValue::kMaxDepth)
    return JsonValue();
  stack.push_back(o);
  JsonValue out;
  if (Access::kind(v) == Kind::Array) {
    out = JsonValue::array();
    for (const JsVar& e : static_cast<const ArrayObject*>(o)->elements)
      out.append(toJsonValue(e, stack, budget));
  } else {
    out = JsonValue::object();
    for (const std::string& k : o->keysInOrder(true)) {
      const JsVar& value = o->find(k)->value;
      if (value.isUndefined() || value.isFunction()) continue; // as JSON.stringify leaves them out
      budget -= (std::min)(budget, k.size());
      out.set(k, toJsonValue(value, stack, budget));
    }
  }
  stack.pop_back();
  return out;
}

inline JsVar fromJsonValue(const JsonValue& j, int depth) {
  // Asked with isBool() and friends rather than by switching on the kind:
  // Xlib.h defines Bool as a macro, and this header must not be the one that
  // trips over it.
  if (j.isNull()) return JsVar(nullptr);
  if (j.isBool()) return JsVar(j.asBool());
  if (j.isNumber()) return JsVar(j.asNumber());
  if (j.isString()) return JsVar(j.asString());
  if (depth > 256) return JsVar(nullptr);
  if (j.isArray()) {
    std::vector<JsVar> items;
    items.reserve(j.size());
    for (size_t i = 0; i < j.size(); ++i) items.push_back(fromJsonValue(j.at(i), depth + 1));
    return JsVar::array(std::move(items));
  }
  auto o = std::make_shared<DynamicObject>();
  for (const auto& m : j.members()) o->setProperty(m.first, fromJsonValue(m.second, depth + 1));
  return JsVar(o);
}

} // namespace js
} // namespace detail

// ── JsVar ───────────────────────────────────────────────────────────────────

inline JsVar::JsVar(const char* text) : kind_(Kind::String) {
  ref_ = detail::js::makeRep(detail::js::canonicalText(text ? text : ""), nullptr);
}

inline JsVar::JsVar(const std::string& text) : kind_(Kind::String) {
  ref_ = detail::js::makeRep(detail::js::canonicalText(text), nullptr);
}

inline JsVar::JsVar(std::shared_ptr<DynamicObject> object) noexcept {
  if (!object) {
    kind_ = Kind::Null;
    return;
  }
  kind_ = object->cellKind_;
  ref_ = std::static_pointer_cast<void>(object);
}

inline JsVar::JsVar(NativeFunction function, int) : kind_(Kind::Function) {
  auto f = std::make_shared<detail::js::FunctionObject>();
  f->type = detail::js::FunctionObject::Type::Native;
  f->native = std::move(function);
  ref_ = std::static_pointer_cast<void>(std::shared_ptr<DynamicObject>(std::move(f)));
}

inline JsVar JsVar::array(std::vector<JsVar> elements) {
  auto a = std::make_shared<detail::js::ArrayObject>();
  a->elements = std::move(elements);
  return JsVar(std::shared_ptr<DynamicObject>(std::move(a)));
}

inline JsVar JsVar::object() { return JsVar(std::make_shared<DynamicObject>()); }

inline const JsVar& JsVar::NativeFunctionArgs::operator[](int index) const noexcept {
  static const JsVar undef;
  return (index >= 0 && index < numArguments) ? arguments[index] : undef;
}

inline bool JsVar::isMethod() const noexcept {
  if (kind_ != Kind::Function) return false;
  return static_cast<const detail::js::FunctionObject*>(
             static_cast<const DynamicObject*>(ref_.get()))
             ->type == detail::js::FunctionObject::Type::Native;
}

inline bool JsVar::toBool() const noexcept {
  switch (kind_) {
    case Kind::Undefined:
    case Kind::Null: return false;
    case Kind::Boolean: return number_ != 0;
    case Kind::Number: return !(number_ == 0 || number_ != number_);
    case Kind::String: return !detail::js::Access::str(*this)->text.empty();
    default: return true;
  }
}

inline double JsVar::toDouble() const {
  switch (kind_) {
    case Kind::Undefined: return std::numeric_limits<double>::quiet_NaN();
    case Kind::Null: return 0.0;
    case Kind::Boolean:
    case Kind::Number: return number_;
    case Kind::String: return detail::js::stringToNumber(detail::js::Access::str(*this)->text);
    case Kind::Array: return detail::js::stringToNumber(toString());
    default: return std::numeric_limits<double>::quiet_NaN();
  }
}

inline int JsVar::toInt() const {
  const double d = toDouble();
  if (d != d) return 0;
  if (d >= 2147483647.0) return 2147483647;
  if (d <= -2147483648.0) return -2147483647 - 1;
  return (int) d;
}

inline int64_t JsVar::toInt64() const { return (int64_t) detail::js::clampToLong(toDouble()); }

inline std::string JsVar::toString() const {
  std::vector<const DynamicObject*> joining;
  return detail::js::plainText(*this, joining);
}

inline const char* JsVar::typeName() const noexcept {
  switch (kind_) {
    case Kind::Undefined: return "undefined";
    case Kind::Boolean: return "boolean";
    case Kind::Number: return "number";
    case Kind::String: return "string";
    case Kind::Function: return "function";
    default: return "object";
  }
}

inline int JsVar::size() const noexcept {
  const std::vector<JsVar>* a = getArray();
  return a ? (int) (std::min)(a->size(), (size_t) 2147483647) : 0;
}

inline JsVar JsVar::operator[](int index) const {
  const std::vector<JsVar>* a = getArray();
  if (!a || index < 0 || (size_t) index >= a->size()) return JsVar();
  return (*a)[(size_t) index];
}

inline std::vector<JsVar>* JsVar::getArray() const noexcept {
  if (kind_ != Kind::Array) return nullptr;
  return &static_cast<detail::js::ArrayObject*>(static_cast<DynamicObject*>(ref_.get()))->elements;
}

inline bool JsVar::append(const JsVar& value) {
  if (kind_ != Kind::Array) return false;
  auto* a = static_cast<detail::js::ArrayObject*>(static_cast<DynamicObject*>(ref_.get()));
  a->elements.push_back(value);
  a->touch();
  return true;
}

inline DynamicObject* JsVar::getDynamicObject() const noexcept {
  return kind_ >= Kind::Array ? static_cast<DynamicObject*>(ref_.get()) : nullptr;
}

inline std::shared_ptr<DynamicObject> JsVar::getObject() const noexcept {
  if (kind_ < Kind::Array) return nullptr;
  return std::static_pointer_cast<DynamicObject>(ref_);
}

inline JsVar JsVar::getProperty(const std::string& name, const JsVar& fallback) const {
  if (kind_ == Kind::Array) {
    const std::vector<JsVar>* a = getArray();
    if (name == "length") return JsVar((double) a->size());
    uint32_t i = 0;
    if (detail::js::isIndexText(name, &i)) return i < a->size() ? (*a)[i] : fallback;
  }
  const DynamicObject* o = getDynamicObject();
  if (!o || !o->hasProperty(name)) return fallback;
  return o->getProperty(name);
}

inline JsVar JsVar::operator[](const char* name) const { return getProperty(name ? name : ""); }
inline JsVar JsVar::operator[](const std::string& name) const { return getProperty(name); }

inline bool JsVar::hasProperty(const std::string& name) const {
  if (kind_ == Kind::Array) {
    uint32_t i = 0;
    if (name == "length") return true;
    if (detail::js::isIndexText(name, &i)) return i < getArray()->size();
  }
  const DynamicObject* o = getDynamicObject();
  return o && o->hasProperty(name);
}

inline bool JsVar::operator==(const JsVar& other) const noexcept {
  return detail::js::Interp::strictEquals(*this, other);
}

inline bool JsVar::looselyEquals(const JsVar& other) const {
  const bool objA = kind_ >= Kind::Array, objB = other.kind_ >= Kind::Array;
  if (kind_ == other.kind_ || (objA && objB)) return *this == other;
  if (isVoid() || other.isVoid()) return isVoid() && other.isVoid();
  if (objA) return JsVar(toString()).looselyEquals(other);
  if (objB) return looselyEquals(JsVar(other.toString()));
  if (isString() && other.isString()) return toString() == other.toString();
  return toDouble() == other.toDouble(); // number, string and boolean meet as numbers
}

inline JsVar JsVar::clone() const { return detail::js::Access::cloneValue(*this); }

inline JsonValue JsVar::toJson() const {
  std::vector<const DynamicObject*> stack;
  size_t budget = detail::js::kPlainTextLimit;
  return detail::js::Access::toJsonValue(*this, stack, budget);
}

inline JsVar JsVar::fromJson(const JsonValue& json) { return detail::js::fromJsonValue(json, 0); }

inline std::string JsVar::toJsonString(int indent) const {
  std::string out;
  try {
    detail::js::JsonWriter writer(nullptr, JsVar(),
                                  std::string((size_t) (std::max)(0, (std::min)(indent, 10)), ' '));
    if (!writer.write(*this, out)) return std::string();
  } catch (const std::exception&) {
    return std::string(); // a cycle, or nesting too deep to write
  }
  return out;
}

inline JsVar JsVar::parseJson(const std::string& text, std::string* error) {
  try {
    detail::js::JsonReader reader(text, nullptr);
    JsVar v = reader.read();
    if (error) error->clear();
    return v;
  } catch (const detail::js::SyntaxErr& e) {
    if (error) *error = e.message;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
  }
  return JsVar();
}

// ── The engine ──────────────────────────────────────────────────────────────

/**
 * Runs scripts: a small JavaScript interpreter with a global scope of its own.
 *
 *   JavascriptEngine js;
 *   js.registerNativeObject("plugin", pluginObject);
 *   js.execute("function curve(x) { return Math.pow(x, 2.5); }");
 *   JsVar y = js.callFunction("curve", {0.5});
 *
 * Every entry point reports failure in a Result -- a syntax error, an
 * uncaught throw, a limit -- and never throws. Globals persist between calls:
 * what one execute() defines, the next can use.
 *
 * The limits below are read at the start of each outermost call; change them
 * between calls, not during one.
 */
class JavascriptEngine {
public:
  /** What happened: success, or an error message with where it happened. */
  class Result {
  public:
    /** Success. */
    Result() = default;
    /** Success, by name. */
    static Result success() { return Result(); }
    /** A failure with its message and, when known, its 1-based place. */
    static Result failure(std::string message, int line = 0, int column = 0) {
      Result r;
      r.ok_ = false;
      r.message_ = std::move(message);
      r.line_ = line;
      r.column_ = column;
      return r;
    }
    /** True on success. */
    bool wasOk() const noexcept { return ok_; }
    /** True on failure. */
    bool failed() const noexcept { return !ok_; }
    /** wasOk(), so `if (js.execute(code))` reads as success. */
    explicit operator bool() const noexcept { return ok_; }
    /** "TypeError: x is not a function", "SyntaxError: Unexpected end of
     *  input", "Execution timed-out", ... -- empty on success. */
    const std::string& getErrorMessage() const noexcept { return message_; }
    /** 1-based; 0 when there is no place to point at. */
    int getLine() const noexcept { return line_; }
    /** 1-based, like getLine(); 0 when there is no place to point at. */
    int getColumn() const noexcept { return column_; }
    /** "Line 3, column 7: TypeError: ..." */
    std::string getDescription() const {
      if (ok_) return std::string();
      if (line_ <= 0) return message_;
      return "Line " + std::to_string(line_) + ", column " + std::to_string(column_) + ": " +
             message_;
    }

  private:
    bool ok_ = true;
    std::string message_;
    int line_ = 0;
    int column_ = 0;
  };

  /** An engine with an empty global scope and the default limits. */
  JavascriptEngine() : interp_(new detail::js::Interp()) {}
  /** Frees the global scope and what only it held. */
  ~JavascriptEngine() = default;
  ///@{
  /** Not copyable: a global scope has one owner. */
  JavascriptEngine(const JavascriptEngine&) = delete;
  JavascriptEngine& operator=(const JavascriptEngine&) = delete;
  ///@}

  // ── Limits ───────────────────────────────────────────────────────────────

  /** Seconds a call may run before it fails with "Execution timed-out" (the
   *  reference's default, 15). Zero or less means no limit. */
  double maximumExecutionTime = 15.0;
  /** Script function calls nested at once. */
  int maximumRecursionDepth = 1000;
  /** Native stack the interpreter may use, measured from where it was
   *  entered -- and never more than the calling thread actually has left,
   *  less 64 KB, which is asked of the OS. Every recursive path checks it
   *  (the parser, the evaluator, JSON, string conversion), so the depth a
   *  script reaches is whichever limit comes first: a script call costs about
   *  1.7 KB of native stack in an optimised build, several times that in a
   *  debug or sanitizer build. */
  size_t maximumStackBytes = (size_t) 4 << 20;
  /** Everything the engine's scripts have allocated and still hold. */
  size_t maximumMemoryBytes = (size_t) 64 << 20;
  /** The longest string a script may make, in UTF-8 bytes. */
  size_t maximumStringLength = (size_t) 8 << 20;
  /** The longest array a script may make. Arrays are dense: a[1e9] = 1 asks
   *  for a billion elements, and is refused. */
  size_t maximumArrayLength = (size_t) 1 << 20;
  /** Where console.log, console.warn ... and trace() write, a line at a time.
   *  Unset, they write nowhere: a plugin's stdout is the host's. */
  std::function<void(const std::string&)> consoleOutput;

  // ── Scripts ──────────────────────────────────────────────────────────────

  /** Makes `object` a global called `name`. */
  void registerNativeObject(const std::string& name, DynamicObject::Ptr object) {
    interp_->global_->setProperty(name, JsVar(std::move(object)));
  }

  /** Makes a C++ function a global called `name`. */
  void registerNativeFunction(const std::string& name, JsVar::NativeFunction function) {
    auto f = std::make_shared<detail::js::FunctionObject>();
    f->type = detail::js::FunctionObject::Type::Native;
    f->native = std::move(function);
    f->name = name;
    interp_->global_->setProperty(name, JsVar(std::shared_ptr<DynamicObject>(std::move(f))));
  }

  /** Evaluates `code` and returns its value. An expression gives its value
   *  ("1 + 2", "{ a: 1 }"); a program gives the value of the last expression
   *  statement it ran, as eval() does. Undefined on failure. */
  JsVar evaluate(const std::string& code, Result* result = nullptr) {
    return run([&code](detail::js::Interp& I) { return I.evaluateCode(code, true); }, result);
  }

  /** Runs `code` as a program. */
  Result execute(const std::string& code) {
    Result r;
    run([&code](detail::js::Interp& I) { return I.evaluateCode(code, false); }, &r);
    return r;
  }

  /** Calls a global function -- one declared by a script, or registered. */
  JsVar callFunction(const std::string& name, const std::vector<JsVar>& args = {},
                     Result* result = nullptr) {
    return run(
        [&name, &args](detail::js::Interp& I) {
          JsVar fn;
          detail::js::Binding* b = I.globalScope_->find(name);
          if (b && !(b->flags & detail::js::Binding::kUninitialised)) fn = b->value;
          else fn = I.global_->getProperty(name);
          if (!fn.isFunction()) I.throwType(name + " is not a function");
          return I.call(fn, JsVar(), args.data(), args.size());
        },
        result);
  }

  /** Calls a function value -- a callback a script handed to a native
   *  function, say -- with `thisObject` as `this`. */
  JsVar callFunctionObject(const JsVar& thisObject, const JsVar& function,
                           const std::vector<JsVar>& args = {}, Result* result = nullptr) {
    return run(
        [&thisObject, &function, &args](detail::js::Interp& I) {
          if (!function.isFunction()) I.throwType(function.toString() + " is not a function");
          return I.call(function, thisObject, args.data(), args.size());
        },
        result);
  }

  /** Stops the script running now, from any thread: it fails with
   *  "Interrupted". Nothing running, nothing happens. */
  void stop() noexcept { interp_->stopFlag.store(true); }

  /** The global object: what scripts' globals live on. */
  DynamicObject::Ptr getRootObject() const { return interp_->global_; }

  /** Bytes the engine's scripts have allocated and still hold. */
  size_t getMemoryUsage() const noexcept {
    const long long b = interp_->meter_->bytes.load();
    return b > 0 ? (size_t) b : 0;
  }

  /** Frees garbage cycles now. The engine does this by itself when memory
   *  runs short and as its heap grows; this is for a plugin that wants it
   *  done at a moment of its choosing. */
  void collectGarbage() {
    if (interp_->running()) return;
    try {
      interp_->collect();
    } catch (...) {
    }
  }

private:
  std::unique_ptr<detail::js::Interp> interp_;

  static std::string describeThrow(detail::js::Interp& I, const JsVar& value) {
    using detail::js::Access;
    const DynamicObject* o = value.getDynamicObject();
    if (o) {
      for (const DynamicObject* p = I.protoOf(o); p; p = I.protoOf(p)) {
        if (p == I.errorProto_.get()) {
          const std::string name = I.chainLookup(o, "name").toString();
          const std::string message = I.chainLookup(o, "message").toString();
          return message.empty() ? name : name + ": " + message;
        }
      }
    }
    return "Uncaught " + value.toString();
  }

  template <typename Body>
  JsVar run(Body&& body, Result* result) {
    detail::js::Interp& I = *interp_;
    if (!I.running()) {
      I.timeLimit = maximumExecutionTime;
      I.maxDepth = maximumRecursionDepth > 0 ? maximumRecursionDepth : 1;
      I.maxStack = maximumStackBytes;
      I.maxMemory = (long long) (std::min)(maximumMemoryBytes, (size_t) 1 << 62);
      I.maxString = maximumStringLength;
      I.maxArray = maximumArrayLength;
      I.consoleHook = consoleOutput;
    }
    detail::js::Interp::Entry entry(I);
    Result r;
    JsVar value;
    try {
      value = body(I);
    } catch (const detail::js::Throw& t) {
      try {
        r = Result::failure(describeThrow(I, t.value), t.line, t.col);
      } catch (...) {
        r = Result::failure("Uncaught exception", t.line, t.col);
      }
    } catch (const detail::js::Abort& a) {
      r = Result::failure(a.message, a.line, a.col);
    } catch (const detail::js::SyntaxErr& e) {
      r = Result::failure("SyntaxError: " + e.message, e.line, e.col);
    } catch (const detail::js::ShortCircuit&) {
      value = JsVar();
    } catch (const std::bad_alloc&) {
      r = Result::failure("Out of memory", I.line_, I.col_);
    } catch (const std::exception& e) {
      r = Result::failure(e.what(), I.line_, I.col_);
    } catch (...) {
      r = Result::failure("unknown error", I.line_, I.col_);
    }
    if (result) *result = r;
    return r.failed() ? JsVar() : value;
  }
};

} // namespace sonore

#undef SONORE_JS_NOINLINE
