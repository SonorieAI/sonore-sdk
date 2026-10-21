// SPDX-License-Identifier: Apache-2.0
//
// JSON, and Base64, for state a plugin has to write down.
//
// ── Why the SDK needs a real one ────────────────────────────────────────────
//
// The webview bridge builds JSON by concatenating strings and parses its own
// messages by hand. That is fine for a fixed message set with four fields and
// useless the moment a plugin has structured state to save: a sampler's key
// map, a modulation matrix, a list of user presets. StateBag holds strings, so
// today the only way to store a tree is to invent a format per plugin.
//
// ── Where this stops ────────────────────────────────────────────────────────
//
// It parses and writes RFC 8259 JSON, with two deliberate limits stated rather
// than discovered: a nesting depth cap, because a plugin's state file may come
// from anywhere and a thousand open brackets is a stack overflow rather than a
// document; and numbers are all doubles, because JSON has no integer type and
// pretending otherwise is where every JSON library's edge cases live.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "number_text.h"

namespace sonore {

/**
 * A JSON value.
 *
 * Objects keep their keys SORTED, because std::map does. That is a deliberate
 * consequence rather than an accident: a plugin's state written twice from the
 * same values produces byte-identical output, so a host can tell whether
 * anything actually changed and a user can diff two preset files.
 */
class JsonValue {
public:
  /** What a value holds: the six JSON types. */
  enum class Kind {
    Null,   ///< `null`, and what a missing key or a failed parse gives.
    Bool,   ///< `true` or `false`.
    Number, ///< Any number, held as a double.
    String, ///< A UTF-8 string.
    Array,  ///< An ordered list of values.
    Object  ///< String keys to values, kept sorted by key.
  };

  /** A null value. */
  JsonValue() = default;
  /** A Bool. */
  JsonValue(bool v) : kind_(Kind::Bool), bool_(v) {}
  /** A Number. NaN and infinity are kept, but toString() writes them as
   *  `null`, the only thing JSON has for them. */
  JsonValue(double v) : kind_(Kind::Number), number_(v) {}
  /** A Number: the int is held as a double, which every int fits exactly. */
  JsonValue(int v) : kind_(Kind::Number), number_((double) v) {}
  /** A String, copied. A null pointer gives an empty string, not a null
   *  value. */
  JsonValue(const char* v) : kind_(Kind::String), string_(v ? v : "") {}
  /** A String. The bytes are kept as given and are expected to be UTF-8. */
  JsonValue(std::string v) : kind_(Kind::String), string_(std::move(v)) {}

  /** A deep copy: a JsonValue owns its whole tree. */
  JsonValue(const JsonValue&) = default;
  /** Takes the tree; `other` is left valid but unspecified. */
  JsonValue(JsonValue&&) = default;
  /** Takes the tree; `other` is left valid but unspecified. */
  JsonValue& operator=(JsonValue&&) = default;
  /** Copies `other` BEFORE anything of this value is freed: `other` may be
   *  part of it. Walking down a path is `node = node["key"]`, and the
   *  memberwise assignment cleared the map that held the child while it was
   *  still copying from it -- freed memory read, and the result null or
   *  garbage. */
  JsonValue& operator=(const JsonValue& other) {
    if (this != &other) {
      JsonValue copy(other);
      *this = std::move(copy);
    }
    return *this;
  }

  /** An empty Array, which writes as `[]`. A default value becomes an Array
   *  only on its first append(), so an empty list built from one writes
   *  `null`. */
  static JsonValue array() {
    JsonValue v;
    v.kind_ = Kind::Array;
    return v;
  }
  /** An empty Object, which writes as `{}`. A default value becomes an
   *  Object only on its first set(), so an empty one built from it writes
   *  `null`. */
  static JsonValue object() {
    JsonValue v;
    v.kind_ = Kind::Object;
    return v;
  }

  /** Which of the six JSON types this value holds. */
  Kind kind() const { return kind_; }
  ///@{
  /** True when kind() is that type. The as...() readers do not convert
   *  between types, so test first when the type itself matters. */
  bool isNull() const { return kind_ == Kind::Null; }
  bool isBool() const { return kind_ == Kind::Bool; }
  bool isNumber() const { return kind_ == Kind::Number; }
  bool isString() const { return kind_ == Kind::String; }
  bool isArray() const { return kind_ == Kind::Array; }
  bool isObject() const { return kind_ == Kind::Object; }
  ///@}

  /**
   * Readers that take a fallback rather than throwing or asserting.
   *
   * A plugin reading a state file written by an older version of itself is the
   * normal case, not the error case: a field it does not recognise, or one that
   * is missing, must give a sensible default and let the plugin load. A reader
   * that threw would turn every forward-compatibility question into a crash.
   */
  bool asBool(bool fallback = false) const {
    return kind_ == Kind::Bool ? bool_ : fallback;
  }
  /** The number, or `fallback` when this is not a Number. A string such as
   *  "0.5" is not converted. */
  double asNumber(double fallback = 0.0) const {
    return kind_ == Kind::Number ? number_ : fallback;
  }
  /** asNumber() narrowed to float, or `fallback` when this is not a
   *  Number. SATURATED, as asInt() is: a finite number beyond float's range
   *  reads as the largest float of its sign (1e300 is valid JSON, and
   *  narrowing a double outside float's range is undefined behaviour --
   *  MSVC gave infinity). An infinity or a NaN is kept. */
  float asFloat(float fallback = 0.0f) const {
    if (kind_ != Kind::Number) return fallback;
    const double kMaxFloat = 3.4028234663852886e38;
    if (number_ > kMaxFloat && number_ <= 1.7976931348623157e308) return 3.40282347e38f;
    if (number_ < -kMaxFloat && number_ >= -1.7976931348623157e308) return -3.40282347e38f;
    return (float) number_;
  }
  /** Truncated toward zero, and SATURATED: a settings file is user-editable,
   *  1e300 is valid JSON, and converting a double outside int's range is
   *  undefined behaviour -- MSVC happened to give INT_MIN for 1e300, a huge
   *  number read back as the most negative one. NaN has no int and gives the
   *  fallback. */
  int asInt(int fallback = 0) const {
    if (kind_ != Kind::Number || number_ != number_) return fallback;
    if (number_ >= 2147483647.0) return 2147483647;
    if (number_ <= -2147483648.0) return -2147483647 - 1;
    return (int) number_;
  }
  /** A copy of the string, or `fallback` when this is not a String. A
   *  Number is not formatted: use toString() for that. */
  std::string asString(const std::string& fallback = std::string()) const {
    return kind_ == Kind::String ? string_ : fallback;
  }

  // ── Arrays ───────────────────────────────────────────────────────────────

  /** The elements of an Array or the members of an Object; 0 for any other
   *  kind. */
  size_t size() const {
    if (kind_ == Kind::Array) return array_.size();
    if (kind_ == Kind::Object) return object_.size();
    return 0;
  }

  /** Adds `v` at the end. A value that is not an Array becomes one first,
   *  so a null value appended to is a one-element array; what it held as
   *  another kind is no longer written. */
  void append(JsonValue v) {
    kind_ = Kind::Array;
    array_.push_back(std::move(v));
  }

  /** Out of range gives a null value rather than reading past the end. */
  const JsonValue& at(size_t index) const {
    static const JsonValue null;
    if (kind_ != Kind::Array || index >= array_.size()) return null;
    return array_[index];
  }

  // ── Objects ──────────────────────────────────────────────────────────────

  /** Sets `key` to `v`, replacing any value it had. A value that is not an
   *  Object becomes one first; what it held as another kind is no longer
   *  written. */
  void set(const std::string& key, JsonValue v) {
    kind_ = Kind::Object;
    object_[key] = std::move(v);
  }

  /** True when this is an Object with `key`, even if the key's value is
   *  null. */
  bool has(const std::string& key) const {
    return kind_ == Kind::Object && object_.find(key) != object_.end();
  }

  /** A missing key gives a null value, so `state["gain"].asFloat(0.5f)` reads
   *  correctly against a file that never had a gain in it. */
  const JsonValue& operator[](const std::string& key) const {
    static const JsonValue null;
    if (kind_ != Kind::Object) return null;
    auto it = object_.find(key);
    return it == object_.end() ? null : it->second;
  }

  /** The members of an Object in key order, for iterating over keys a
   *  reader does not know in advance. Empty for a value that was never an
   *  Object. The reference is valid until this value changes or is
   *  destroyed. */
  const std::map<std::string, JsonValue>& members() const { return object_; }

  // ── Writing ──────────────────────────────────────────────────────────────

  /** `indent` of 0 is compact, which is what goes in a state chunk. Anything
   *  else is for a file a person will open. */
  std::string toString(int indent = 0) const {
    std::string out;
    write(out, indent, 0);
    return out;
  }

  // ── Reading ──────────────────────────────────────────────────────────────

  /** Depth is capped because a state file may come from anywhere, and a
   *  thousand open brackets is a stack overflow rather than a document. */
  static constexpr int kMaxDepth = 64;

  /** Parses, or returns null with `errorOut` set. */
  static JsonValue parse(const std::string& text, std::string* errorOut = nullptr) {
    Parser parser(text);
    JsonValue value = parser.parseValue(0);
    if (!parser.error.empty()) {
      if (errorOut) *errorOut = parser.error;
      return JsonValue();
    }
    parser.skipSpace();
    if (parser.at < text.size()) {
      if (errorOut) *errorOut = "trailing characters after the JSON value";
      return JsonValue();
    }
    if (errorOut) errorOut->clear();
    return value;
  }

private:
  void write(std::string& out, int indent, int depth) const {
    const bool pretty = indent > 0;
    switch (kind_) {
      case Kind::Null: out += "null"; return;
      case Kind::Bool: out += bool_ ? "true" : "false"; return;
      case Kind::Number: writeNumber(out); return;
      case Kind::String: writeString(out, string_); return;
      case Kind::Array: {
        if (array_.empty()) {
          out += "[]";
          return;
        }
        out += '[';
        for (size_t i = 0; i < array_.size(); ++i) {
          if (i) out += ',';
          if (pretty) newline(out, indent, depth + 1);
          array_[i].write(out, indent, depth + 1);
        }
        if (pretty) newline(out, indent, depth);
        out += ']';
        return;
      }
      case Kind::Object: {
        if (object_.empty()) {
          out += "{}";
          return;
        }
        out += '{';
        bool first = true;
        for (const auto& entry : object_) {
          if (!first) out += ',';
          first = false;
          if (pretty) newline(out, indent, depth + 1);
          writeString(out, entry.first);
          out += ':';
          if (pretty) out += ' ';
          entry.second.write(out, indent, depth + 1);
        }
        if (pretty) newline(out, indent, depth);
        out += '}';
        return;
      }
    }
  }

  static void newline(std::string& out, int indent, int depth) {
    out += '\n';
    out.append((size_t) (indent * depth), ' ');
  }

  void writeNumber(std::string& out) const {
    // %.17g round-trips a double exactly, which is what a state file needs: a
    // parameter written and read back must be the same value, or reloading a
    // preset moves every control by a fraction. The trailing ".0" is dropped
    // for whole numbers because JSON has no integer type and a reader would
    // rather see 3 than 3.0000000000000000.
    if (number_ != number_ || number_ == number_ * 0.5 * 2.0 + 1e308) {
      // NaN or infinity, neither of which JSON can express. Null is the only
      // honest answer -- writing `nan` produces a file no other parser reads.
      out += "null";
      return;
    }
    char buffer[40];
    // The range FIRST: converting a double of 2^63 or more to long long is
    // undefined behaviour, and the comparison used to do exactly that before
    // it looked at the range.
    if (number_ < 1e15 && number_ > -1e15 && number_ == (double) (long long) number_) {
      std::snprintf(buffer, sizeof(buffer), "%lld", (long long) number_);
    } else {
      // Not printf alone: a host that set a comma locale would make this
      // [0,5] -- two numbers to every other reader of the file.
      numtext::format(buffer, sizeof(buffer), number_);
    }
    out += buffer;
  }

  static void writeString(std::string& out, const std::string& text) {
    out += '"';
    for (unsigned char c : text) {
      switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
          if (c < 0x20) {
            // Control characters MUST be escaped; anything else is passed
            // through as-is, which keeps UTF-8 intact rather than expanding it
            // into \u escapes nobody needs.
            char buffer[8];
            std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
            out += buffer;
          } else {
            out += (char) c;
          }
      }
    }
    out += '"';
  }

  /** One parse() call's state: the text, the position and the first error.
   *  Recursive descent, one call per level of nesting, refused past
   *  kMaxDepth. Holds a reference to the text, so it lives inside parse()
   *  and nowhere else. */
  struct Parser {
    explicit Parser(const std::string& t) : text(t) {}

    const std::string& text;
    size_t at = 0;
    std::string error;

    void skipSpace() {
      while (at < text.size() &&
             (text[at] == ' ' || text[at] == '\t' || text[at] == '\n' || text[at] == '\r'))
        ++at;
    }

    JsonValue parseValue(int depth) {
      if (depth > kMaxDepth) {
        error = "JSON nested too deeply";
        return JsonValue();
      }
      skipSpace();
      if (at >= text.size()) {
        error = "unexpected end of JSON";
        return JsonValue();
      }
      const char c = text[at];
      if (c == '{') return parseObject(depth);
      if (c == '[') return parseArray(depth);
      if (c == '"') return JsonValue(parseString());
      if (c == 't' || c == 'f') return parseBool();
      if (c == 'n') {
        if (text.compare(at, 4, "null") != 0) {
          error = "expected null";
          return JsonValue();
        }
        at += 4;
        return JsonValue();
      }
      return parseNumber();
    }

    JsonValue parseObject(int depth) {
      JsonValue out = JsonValue::object();
      ++at; // {
      skipSpace();
      if (at < text.size() && text[at] == '}') {
        ++at;
        return out;
      }
      for (;;) {
        skipSpace();
        if (at >= text.size() || text[at] != '"') {
          error = "expected a key in a JSON object";
          return JsonValue();
        }
        const std::string key = parseString();
        if (!error.empty()) return JsonValue();
        skipSpace();
        if (at >= text.size() || text[at] != ':') {
          error = "expected ':' after a JSON key";
          return JsonValue();
        }
        ++at;
        JsonValue value = parseValue(depth + 1);
        if (!error.empty()) return JsonValue();
        out.set(key, std::move(value));
        skipSpace();
        if (at < text.size() && text[at] == ',') {
          ++at;
          continue;
        }
        if (at < text.size() && text[at] == '}') {
          ++at;
          return out;
        }
        error = "expected ',' or '}' in a JSON object";
        return JsonValue();
      }
    }

    JsonValue parseArray(int depth) {
      JsonValue out = JsonValue::array();
      ++at; // [
      skipSpace();
      if (at < text.size() && text[at] == ']') {
        ++at;
        return out;
      }
      for (;;) {
        JsonValue value = parseValue(depth + 1);
        if (!error.empty()) return JsonValue();
        out.append(std::move(value));
        skipSpace();
        if (at < text.size() && text[at] == ',') {
          ++at;
          continue;
        }
        if (at < text.size() && text[at] == ']') {
          ++at;
          return out;
        }
        error = "expected ',' or ']' in a JSON array";
        return JsonValue();
      }
    }

    JsonValue parseBool() {
      if (text.compare(at, 4, "true") == 0) {
        at += 4;
        return JsonValue(true);
      }
      if (text.compare(at, 5, "false") == 0) {
        at += 5;
        return JsonValue(false);
      }
      error = "expected true or false";
      return JsonValue();
    }

    JsonValue parseNumber() {
      const size_t from = at;
      if (at < text.size() && (text[at] == '-' || text[at] == '+')) ++at;
      while (at < text.size() &&
             ((text[at] >= '0' && text[at] <= '9') || text[at] == '.' || text[at] == 'e' ||
              text[at] == 'E' || text[at] == '+' || text[at] == '-'))
        ++at;
      if (at == from) {
        error = "expected a JSON value";
        return JsonValue();
      }
      // Not atof: under a host's comma locale it stops at the '.', and every
      // fraction in a settings file read back truncated.
      //
      // ALL of the span, or it is not a number. The scan above is generous
      // and the conversion stops where the number does, so the rest was
      // dropped without a word: [1-2] read as [1], {"gain":0.5.7} as 0.5.
      // And finite: 1e999 is +inf, which no JSON writer produces and no
      // setting means.
      double value = 0.0;
      if (numtext::parse(text.data() + from, at - from, &value) != at - from ||
          !std::isfinite(value)) {
        error = "malformed JSON number";
        return JsonValue();
      }
      return JsonValue(value);
    }

    /** Exactly four hex digits at `pos`, or false. strtoul took any prefix --
     *  "0x41", " 041", "12zz", "-001" -- and read what it liked of it. */
    bool hex4(size_t pos, uint32_t* code) const {
      if (pos + 4 > text.size()) return false;
      uint32_t v = 0;
      for (size_t i = pos; i < pos + 4; ++i) {
        const char c = text[i];
        uint32_t d;
        if (c >= '0' && c <= '9') d = (uint32_t) (c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t) (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint32_t) (c - 'A' + 10);
        else return false;
        v = v * 16 + d;
      }
      *code = v;
      return true;
    }

    std::string parseString() {
      std::string out;
      ++at; // opening quote
      while (at < text.size()) {
        const char c = text[at];
        if (c == '"') {
          ++at;
          return out;
        }
        if (c != '\\') {
          out += c;
          ++at;
          continue;
        }
        ++at;
        if (at >= text.size()) break;
        const char escape = text[at++];
        switch (escape) {
          case '"': out += '"'; break;
          case '\\': out += '\\'; break;
          case '/': out += '/'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'n': out += '\n'; break;
          case 'r': out += '\r'; break;
          case 't': out += '\t'; break;
          case 'u': {
            uint32_t code = 0;
            if (!hex4(at, &code)) {
              error = at + 4 > text.size() ? "truncated \\u escape" : "malformed \\u escape";
              return out;
            }
            at += 4;
            // A surrogate PAIR is two escapes and one character. Emitting each
            // half separately produces bytes no UTF-8 reader accepts, which is
            // how an emoji in a preset name becomes two question marks.
            uint32_t low = 0;
            if (code >= 0xd800 && code <= 0xdbff && at + 6 <= text.size() &&
                text[at] == '\\' && text[at + 1] == 'u' && hex4(at + 2, &low) &&
                low >= 0xdc00 && low <= 0xdfff) {
              code = 0x10000u + ((code - 0xd800u) << 10) + (low - 0xdc00u);
              at += 6;
            } else if (code >= 0xd800 && code <= 0xdfff) {
              // Half a pair is no character at all. Encoded as it stood it was
              // ED A0 80, which is not UTF-8; the replacement character is
              // what every reader shows for it anyway.
              code = 0xFFFD;
            }
            appendUtf8(out, code);
            break;
          }
          default:
            error = "unknown escape in a JSON string";
            return out;
        }
      }
      error = "unterminated JSON string";
      return out;
    }

    static void appendUtf8(std::string& out, uint32_t code) {
      if (code < 0x80) {
        out += (char) code;
      } else if (code < 0x800) {
        out += (char) (0xc0 | (code >> 6));
        out += (char) (0x80 | (code & 0x3f));
      } else if (code < 0x10000) {
        out += (char) (0xe0 | (code >> 12));
        out += (char) (0x80 | ((code >> 6) & 0x3f));
        out += (char) (0x80 | (code & 0x3f));
      } else {
        out += (char) (0xf0 | (code >> 18));
        out += (char) (0x80 | ((code >> 12) & 0x3f));
        out += (char) (0x80 | ((code >> 6) & 0x3f));
        out += (char) (0x80 | (code & 0x3f));
      }
    }
  };

  Kind kind_ = Kind::Null;
  bool bool_ = false;
  double number_ = 0.0;
  std::string string_;
  std::vector<JsonValue> array_;
  std::map<std::string, JsonValue> object_;
};

/**
 * Base64, so binary state can live in a string.
 *
 * StateBag holds strings and a host's state chunk is bytes, so a plugin with an
 * impulse response or a wavetable to save has nowhere to put it. This is the
 * standard alphabet with padding -- not the URL-safe variant, because nothing
 * here puts state in a URL and two alphabets is two things to get wrong.
 */
struct Base64 {
  /** `size` bytes at `data` as padded Base64, 4 characters per 3 bytes and
   *  no line breaks. `data` may be null when `size` is 0. */
  static std::string encode(const uint8_t* data, size_t size) {
    static const char* kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < size; i += 3) {
      const uint32_t triple = ((uint32_t) data[i] << 16) | ((uint32_t) data[i + 1] << 8) |
                              (uint32_t) data[i + 2];
      out += kAlphabet[(triple >> 18) & 0x3f];
      out += kAlphabet[(triple >> 12) & 0x3f];
      out += kAlphabet[(triple >> 6) & 0x3f];
      out += kAlphabet[triple & 0x3f];
    }
    // The tail, padded. Without padding the length is ambiguous and a decoder
    // cannot tell one trailing byte from two.
    if (i < size) {
      const uint32_t a = data[i];
      const uint32_t b = (i + 1 < size) ? data[i + 1] : 0u;
      const uint32_t triple = (a << 16) | (b << 8);
      out += kAlphabet[(triple >> 18) & 0x3f];
      out += kAlphabet[(triple >> 12) & 0x3f];
      out += (i + 1 < size) ? kAlphabet[(triple >> 6) & 0x3f] : '=';
      out += '=';
    }
    return out;
  }

  /** The same, for a whole vector. */
  static std::string encode(const std::vector<uint8_t>& bytes) {
    return encode(bytes.data(), bytes.size());
  }

  /** Returns false rather than producing partial output: a state chunk that
   *  decoded halfway would be applied halfway, which is worse than not loading. */
  static bool decode(const std::string& text, std::vector<uint8_t>& out) {
    out.clear();
    out.reserve(text.size() / 4 * 3);
    uint32_t buffer = 0;
    int bits = 0;
    // Padding counted, not skipped: it was ignored wherever it fell, so a
    // truncated quad ("AAECA") or data after the padding ("QQ==QUJD") decoded
    // to true and a few bytes -- the partial output this promises never to
    // give.
    size_t symbols = 0, padding = 0;
    auto refuse = [&out]() {
      out.clear();
      return false;
    };
    for (char c : text) {
      if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
      if (c == '=') {
        if (++padding > 2) return refuse();
        continue;
      }
      if (padding) return refuse(); // data after the padding
      const int value = valueOf(c);
      if (value < 0) return refuse();
      ++symbols;
      buffer = (buffer << 6) | (uint32_t) value;
      bits += 6;
      if (bits >= 8) {
        bits -= 8;
        out.push_back((uint8_t) ((buffer >> bits) & 0xff));
      }
    }
    // Whole quads when padded; unpadded, a last group of two or three
    // symbols is unambiguous, one is not. And the bits left over are the
    // encoder's zero fill -- anything else is a different, damaged input.
    if (padding ? (symbols + padding) % 4 != 0 : symbols % 4 == 1) return refuse();
    if (bits > 0 && (buffer & ((1u << bits) - 1u)) != 0) return refuse();
    return true;
  }

private:
  static int valueOf(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  }
};

} // namespace sonore
