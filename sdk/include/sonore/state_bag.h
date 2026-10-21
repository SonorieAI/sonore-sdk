// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: state a plugin has beyond its parameters.
//
// The versioned SNRS blob every format shares stores the parameter values and
// the bypass flag. That is all a filter or a saturator has. But the moment a
// plugin loads a FILE (a sampler, a convolution reverb, an IR-based cabinet),
// its state includes something no float array can express: which file, at what
// offset, with which loop points, plus whatever curve the user drew.
//
// A plugin whose session reopens with an empty sample slot is a plugin the
// user does not trust twice, so this is not a nicety.
//
// The design is deliberately dull: an ordered list of key/value pairs where
// values are bytes. Not JSON, not a tree. A tree invites a schema, a schema
// invites migrations, and the one thing this must never do is fail to load a
// session saved by a slightly older build.

#pragma once

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "number_text.h"

namespace sonore {

/**
 * Key/value state, serialised as part of the plugin's blob.
 *
 * Reading a key that was never written returns the fallback rather than
 * failing: a plugin gaining a new setting must still open every session saved
 * before that setting existed, and the fallback IS the migration.
 */
class StateBag {
public:
  /** Remove every key. */
  void clear() { entries_.clear(); }
  /** How many keys the bag holds. */
  size_t size() const { return entries_.size(); }
  /** Whether it holds no key at all. */
  bool empty() const { return entries_.empty(); }

  // ── Walking it ────────────────────────────────────────────
  //
  // Everything else here is by key, because that is how a DSP uses it. An
  // editor needs the other direction: it has to show what is in the bag
  // without knowing in advance what a particular plugin put there.

  /** The key at `index`, 0-based, in the order keys were first written; an
   *  empty string past the end. The reference is the bag's own, valid until
   *  the bag next changes. */
  const std::string& keyAt(size_t index) const {
    static const std::string empty;
    return index < entries_.size() ? entries_[index].key : empty;
  }

  /** The value as text. Every setter stores text -- numbers included, so a
   *  session moves between machines of different byte order -- so this is
   *  lossless for everything except a deliberate blob. */
  std::string valueAt(size_t index) const {
    if (index >= entries_.size()) return std::string();
    const Entry& e = entries_[index];
    return std::string((const char*) e.value.data(), e.value.size());
  }

  /** Whether the value is text at all. setBytes() takes arbitrary data -- a
   *  wavetable, an impulse response -- and handing that to a UI as if it were
   *  a string produces a screenful of rubbish at best. A caller showing the
   *  bag to somebody asks this first. */
  bool valueIsText(size_t index) const {
    if (index >= entries_.size()) return false;
    return isUtf8Text(entries_[index].value.data(), entries_[index].value.size());
  }

  /** Text means well-formed UTF-8 with no control bytes but tab. Control
   *  bytes alone was the old test, and a blob is rarely without a byte of
   *  0x80 or more that starts no valid character: "m\xE7\xFB=" passed as text,
   *  and the page's state arrived as invalid UTF-8. Overlong forms,
   *  surrogates and anything past U+10FFFF are not text either. */
  static bool isUtf8Text(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n;) {
      const uint8_t c = p[i];
      if (c < 0x80) {
        if (c < 0x20 && c != '\t') return false;
        ++i;
        continue;
      }
      size_t len = 0;
      uint32_t cp = 0, least = 0;
      if (c >= 0xC2 && c <= 0xDF) { len = 2; cp = c & 0x1Fu; least = 0x80; }
      else if (c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0Fu; least = 0x800; }
      else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07u; least = 0x10000; }
      else return false; // a continuation byte first, C0/C1, or F5 and up
      if (i + len > n) return false;
      for (size_t k = 1; k < len; ++k) {
        if ((p[i + k] & 0xC0u) != 0x80u) return false;
        cp = (cp << 6) | (p[i + k] & 0x3Fu);
      }
      if (cp < least || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
      i += len;
    }
    return true;
  }

  // ── Writing ────────────────────────────────────────────────────────────────
  //
  // Every setter copies, replaces the key's value in place if it was written
  // before (keeping its position), and ignores a null or empty key. They
  // allocate: call them from saveState(), not from process().

  /** Store the string's bytes under `key`. */
  void setString(const char* key, const std::string& value) {
    setBytes(key, (const uint8_t*) value.data(), value.size());
  }

  /** Store `size` bytes from `data` under `key` -- a wavetable, an impulse
   *  response, anything; nothing is interpreted. `data` may be null only when
   *  `size` is 0. */
  void setBytes(const char* key, const uint8_t* data, size_t size) {
    if (!key || !key[0]) return;
    Entry* existing = find(key);
    if (existing) {
      existing->value.assign(data, data + size);
      return;
    }
    Entry entry;
    entry.key = key;
    entry.value.assign(data, data + size);
    entries_.push_back(std::move(entry));
  }

  /** Store an integer, as decimal text. */
  void setInt(const char* key, int64_t value) {
    // Text rather than raw bytes: an integer written on one machine and read
    // on another must not depend on either one's byte order, and a session
    // file travels between machines all the time.
    setString(key, std::to_string(value));
  }

  /** Store a double as text with '.' as the point whatever the locale, with
   *  enough digits that getDouble() returns exactly this value. Infinities
   *  and NaN are stored as such and read back as such. */
  void setDouble(const char* key, double value) {
    // Seventeen digits round-trips every double exactly, and snprintf with %.17g
    // is locale-sensitive, hence the manual, locale-immune formatting used
    // everywhere else in this SDK.
    char text[48];
    formatDouble(text, sizeof(text), value);
    setString(key, text);
  }

  /** Store a bool, as the integer 1 or 0. */
  void setBool(const char* key, bool value) { setInt(key, value ? 1 : 0); }

  // ── Reading ────────────────────────────────────────────────────────────────

  /** Whether `key` was written, even with an empty value. False for null. */
  bool has(const char* key) const { return findConst(key) != nullptr; }

  /** The value's bytes as a string, unchanged, or `fallback` when the key is
   *  absent. */
  std::string getString(const char* key, const std::string& fallback = std::string()) const {
    const Entry* entry = findConst(key);
    if (!entry) return fallback;
    return std::string((const char*) entry->value.data(), entry->value.size());
  }

  /** The value's bytes, or null when the key is absent. The vector is the
   *  bag's own: valid until the bag next changes, and not to be kept. */
  const std::vector<uint8_t>* getBytes(const char* key) const {
    const Entry* entry = findConst(key);
    return entry ? &entry->value : nullptr;
  }

  /** The value read as a decimal integer, or `fallback` when the key is
   *  absent or empty or its text does not start with a number. Text after the
   *  leading number is ignored. */
  int64_t getInt(const char* key, int64_t fallback = 0) const {
    const Entry* entry = findConst(key);
    if (!entry || entry->value.empty()) return fallback;
    const std::string text((const char*) entry->value.data(), entry->value.size());
    char* end = nullptr;
    const long long v = std::strtoll(text.c_str(), &end, 10);
    return (end && end != text.c_str()) ? (int64_t) v : fallback;
  }

  /** The value read as a double, with '.' or ',' as the point whatever the
   *  locale, or `fallback` when the key is absent or empty or its text does
   *  not start with a number. "inf", "-inf" and "nan" read as themselves. */
  double getDouble(const char* key, double fallback = 0.0) const {
    const Entry* entry = findConst(key);
    if (!entry || entry->value.empty()) return fallback;
    const std::string text((const char*) entry->value.data(), entry->value.size());
    return parseDouble(text.c_str(), fallback);
  }

  /** True for any integer other than 0; `fallback` as getInt() gives it. */
  bool getBool(const char* key, bool fallback = false) const {
    return getInt(key, fallback ? 1 : 0) != 0;
  }

  // ── Serialisation ──────────────────────────────────────────────────────────

  /** Append to a byte stream: count, then key/value pairs with explicit
   *  lengths. Lengths rather than terminators, so a value may contain a null
   *  and a binary blob needs no escaping. */
  void serialise(std::vector<uint8_t>& out) const {
    writeU32(out, (uint32_t) entries_.size());
    for (const Entry& entry : entries_) {
      writeU32(out, (uint32_t) entry.key.size());
      out.insert(out.end(), entry.key.begin(), entry.key.end());
      writeU32(out, (uint32_t) entry.value.size());
      out.insert(out.end(), entry.value.begin(), entry.value.end());
    }
  }

  /** Read back. Returns false on anything malformed rather than producing a
   *  half-filled bag: a truncated session is better reported than silently
   *  reopened with some settings missing. On false the bag is left exactly as
   *  it was -- it used to be emptied first and then filled as far as the
   *  damage, so a caller that kept using it saw a mixture of neither. */
  bool deserialise(const uint8_t* data, size_t size, size_t* consumed = nullptr) {
    std::vector<Entry> parsed;
    size_t pos = 0;
    uint32_t count = 0;
    if (!readU32(data, size, pos, &count)) return false;
    // A corrupt count must not spin the loop allocating: no plugin has a
    // million settings.
    if (count > 65536) return false;
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t keyLength = 0, valueLength = 0;
      if (!readU32(data, size, pos, &keyLength)) return false;
      // Compared as what is LEFT, never as pos + length: on a 32-bit build
      // that sum wraps for a length near 4 GB, passes, and the assign below
      // reads gigabytes past the blob. pos <= size holds after every read.
      if (keyLength > size - pos) return false;
      Entry entry;
      entry.key.assign((const char*) data + pos, keyLength);
      pos += keyLength;
      if (!readU32(data, size, pos, &valueLength)) return false;
      if (valueLength > size - pos) return false;
      entry.value.assign(data + pos, data + pos + valueLength);
      pos += valueLength;
      parsed.push_back(std::move(entry));
    }
    entries_.swap(parsed);
    if (consumed) *consumed = pos;
    return true;
  }

private:
  /** One key and its value, kept in the order keys were first written --
   *  the order serialise() writes and keyAt() walks. Writing a key again
   *  replaces the value in place. The value is bytes: text for everything
   *  the typed setters store, anything at all from setBytes(). */
  struct Entry {
    std::string key;            ///< never empty
    std::vector<uint8_t> value; ///< the bytes as stored
  };

  Entry* find(const char* key) {
    for (Entry& entry : entries_)
      if (entry.key == key) return &entry;
    return nullptr;
  }
  const Entry* findConst(const char* key) const {
    if (!key) return nullptr;
    for (const Entry& entry : entries_)
      if (entry.key == key) return &entry;
    return nullptr;
  }

  static void writeU32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back((uint8_t) (v & 0xff));
    out.push_back((uint8_t) ((v >> 8) & 0xff));
    out.push_back((uint8_t) ((v >> 16) & 0xff));
    out.push_back((uint8_t) ((v >> 24) & 0xff));
  }

  static bool readU32(const uint8_t* data, size_t size, size_t& pos, uint32_t* out) {
    if (!data || pos + 4 > size) return false;
    *out = (uint32_t) data[pos] | ((uint32_t) data[pos + 1] << 8) |
           ((uint32_t) data[pos + 2] << 16) | ((uint32_t) data[pos + 3] << 24);
    pos += 4;
    return true;
  }

  /** Locale-immune double formatting, the same rule the parameter bridge
   *  follows: a host that has called setlocale to a comma locale would
   *  otherwise write "0,5" and read back 0. */
  static void formatDouble(char* out, size_t capacity, double value) {
    numtext::format(out, capacity, value);
  }

  /** The C library's conversion, correctly rounded, with only the decimal
   *  point translated. The hand-assembled digit loop this replaces summed
   *  powers of a tenth, which is not exact: 0.3 came back as
   *  0.30000000000000010 and most doubles a few units in the last place off,
   *  so a value saved and restored was not the value saved. A comma is still
   *  read as the point, for blobs a comma-locale build wrote long ago. */
  static double parseDouble(const char* text, double fallback) {
    if (!text || !*text) return fallback;
    std::string copy(text);
    for (char& c : copy) {
      if (c == ',') c = '.';
      if (c >= 'A' && c <= 'Z') c = (char) (c - 'A' + 'a');
    }
    // What setDouble writes for the values that are not numbers. They read
    // back as the FALLBACK, so a fader saved at -inf dB -- the bottom of its
    // travel -- reopened wherever the fallback put it.
    if (copy == "inf" || copy == "+inf" || copy == "infinity")
      return std::numeric_limits<double>::infinity();
    if (copy == "-inf" || copy == "-infinity") return -std::numeric_limits<double>::infinity();
    if (copy == "nan" || copy == "-nan" || copy == "+nan")
      return std::numeric_limits<double>::quiet_NaN();
    return numtext::parseOr(copy, fallback);
  }

  std::vector<Entry> entries_;
};

} // namespace sonore
