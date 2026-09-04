// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: state a plugin has beyond its parameters.
//
// The versioned SNRS blob every format shares stores the parameter values and
// the bypass flag. That is all a filter or a saturator has. But the moment a
// plugin loads a FILE, a sampler, a convolution reverb, an IR-based cabinet,
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
#include <string>
#include <vector>

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
  void clear() { entries_.clear(); }
  size_t size() const { return entries_.size(); }
  bool empty() const { return entries_.empty(); }

  // ── Walking it ────────────────────────────────────────────
  //
  // Everything else here is by key, because that is how a DSP uses it. An
  // editor needs the other direction: it has to show what is in the bag
  // without knowing in advance what a particular plugin put there.

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
    for (uint8_t c : entries_[index].value)
      if (c < 0x20 && c != '\t') return false; // control bytes: not text
    return true;
  }

  // ── Writing ────────────────────────────────────────────────────────────────

  void setString(const char* key, const std::string& value) {
    setBytes(key, (const uint8_t*) value.data(), value.size());
  }

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

  void setInt(const char* key, int64_t value) {
    // Text rather than raw bytes: an integer written on one machine and read
    // on another must not depend on either one's byte order, and a session
    // file travels between machines all the time.
    setString(key, std::to_string(value));
  }

  void setDouble(const char* key, double value) {
    // Seventeen digits round-trips every double exactly, and snprintf with %.17g
    // is locale-sensitive: hence the manual, locale-immune formatting used
    // everywhere else in this SDK.
    char text[48];
    formatDouble(text, sizeof(text), value);
    setString(key, text);
  }

  void setBool(const char* key, bool value) { setInt(key, value ? 1 : 0); }

  // ── Reading ────────────────────────────────────────────────────────────────

  bool has(const char* key) const { return findConst(key) != nullptr; }

  std::string getString(const char* key, const std::string& fallback = std::string()) const {
    const Entry* entry = findConst(key);
    if (!entry) return fallback;
    return std::string((const char*) entry->value.data(), entry->value.size());
  }

  const std::vector<uint8_t>* getBytes(const char* key) const {
    const Entry* entry = findConst(key);
    return entry ? &entry->value : nullptr;
  }

  int64_t getInt(const char* key, int64_t fallback = 0) const {
    const Entry* entry = findConst(key);
    if (!entry || entry->value.empty()) return fallback;
    const std::string text((const char*) entry->value.data(), entry->value.size());
    char* end = nullptr;
    const long long v = std::strtoll(text.c_str(), &end, 10);
    return (end && end != text.c_str()) ? (int64_t) v : fallback;
  }

  double getDouble(const char* key, double fallback = 0.0) const {
    const Entry* entry = findConst(key);
    if (!entry || entry->value.empty()) return fallback;
    const std::string text((const char*) entry->value.data(), entry->value.size());
    return parseDouble(text.c_str(), fallback);
  }

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
   *  reopened with some settings missing. */
  bool deserialise(const uint8_t* data, size_t size, size_t* consumed = nullptr) {
    clear();
    size_t pos = 0;
    uint32_t count = 0;
    if (!readU32(data, size, pos, &count)) return false;
    // A corrupt count must not spin the loop allocating: no plugin has a
    // million settings.
    if (count > 65536) return false;
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t keyLength = 0, valueLength = 0;
      if (!readU32(data, size, pos, &keyLength)) return false;
      if (pos + keyLength > size) return false;
      Entry entry;
      entry.key.assign((const char*) data + pos, keyLength);
      pos += keyLength;
      if (!readU32(data, size, pos, &valueLength)) return false;
      if (pos + valueLength > size) return false;
      entry.value.assign(data + pos, data + pos + valueLength);
      pos += valueLength;
      entries_.push_back(std::move(entry));
    }
    if (consumed) *consumed = pos;
    return true;
  }

private:
  struct Entry {
    std::string key;
    std::vector<uint8_t> value;
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
    std::snprintf(out, capacity, "%.17g", value);
    for (size_t i = 0; out[i]; ++i)
      if (out[i] == ',') out[i] = '.';
  }

  static double parseDouble(const char* text, double fallback) {
    // strtod is locale-sensitive too, so the fractional part is assembled by
    // hand rather than trusted to it.
    if (!text || !*text) return fallback;
    const char* p = text;
    bool negative = false;
    if (*p == '-') {
      negative = true;
      ++p;
    } else if (*p == '+') {
      ++p;
    }
    double whole = 0.0;
    bool anyDigits = false;
    while (*p >= '0' && *p <= '9') {
      whole = whole * 10.0 + (double) (*p - '0');
      ++p;
      anyDigits = true;
    }
    if (*p == '.' || *p == ',') {
      ++p;
      double scale = 0.1;
      while (*p >= '0' && *p <= '9') {
        whole += (double) (*p - '0') * scale;
        scale *= 0.1;
        ++p;
        anyDigits = true;
      }
    }
    if (!anyDigits) return fallback;
    if (*p == 'e' || *p == 'E') {
      ++p;
      bool negativeExponent = false;
      if (*p == '-') {
        negativeExponent = true;
        ++p;
      } else if (*p == '+') {
        ++p;
      }
      int exponent = 0;
      while (*p >= '0' && *p <= '9') exponent = exponent * 10 + (*p++ - '0');
      double factor = 1.0;
      for (int i = 0; i < exponent && i < 308; ++i) factor *= 10.0;
      whole = negativeExponent ? whole / factor : whole * factor;
    }
    return negative ? -whole : whole;
  }

  std::vector<Entry> entries_;
};

} // namespace sonore
