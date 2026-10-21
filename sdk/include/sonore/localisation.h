// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: the interface's text in the user's language, looked up by the
// English it was written in.
//
// ── Why it is here after all ────────────────────────────────────────────────
//
// The first answer was that translation belongs upstream: a plugin's
// parameter names come from its descriptor, and a generated plugin can be
// generated in any language. That covers parameter names and nothing else.
// An editor also says "Save preset", "Sample not found", "Click to learn",
// and every one of those is a string literal in the code. Switching language
// at runtime, and a translator sending a file rather than a pull request, both
// need the table at runtime.
//
// ── The file format ─────────────────────────────────────────────────────────
//
// The reference's, so translation files people already have load unchanged:
//
//     language: French
//     countries: fr be mc ch lu
//
//     "Save preset" = "Enregistrer le preset"
//     "Sample \"%s\" not found" = "Échantillon \"%s\" introuvable"
//
// UTF-8, with or without a byte-order mark, or UTF-16 with one (either byte
// order). One mapping per line, both sides double-quoted. Inside the quotes \" \' \\
// \n \t and \r are escapes; any other backslash is kept as written. A line
// that is not a mapping, a language or a countries line is ignored -- which
// is how comments work, whatever they start with. A mapping that cannot be
// read (a missing quote, no '=') is skipped and its line number recorded, so
// a translator can be told where the file is wrong rather than wondering why
// one string stayed English. (The one difference from the reference: it does
// not treat \\ as an escape, so a string ending in a backslash could not be
// written at all.)
//
// ── Threads ─────────────────────────────────────────────────────────────────
//
// The current mapping is an IMMUTABLE snapshot held by a shared_ptr.
// translate() takes a short lock only to copy that pointer, then looks up
// without it; setCurrentMappings() swaps the pointer under the same lock. A
// lookup that started against the old table finishes against it, and the old
// table is freed by whichever side lets go last. So the editor can translate
// from any thread while the user changes language -- but not from the audio
// thread: a lookup returns a std::string, which allocates.
#pragma once

#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "path_io.h"

namespace sonore {

/**
 * A table of translations, keyed by the original (English) text, with an
 * optional fallback table consulted for anything this one lacks.
 */
class LocalisedStrings {
public:
  /** An empty table: every lookup misses. */
  LocalisedStrings() = default;

  /** Parses the contents of a translation file (see the header comment).
   *  `ignoreCase` matches originals with ASCII case folded. */
  explicit LocalisedStrings(const std::string& fileContents, bool ignoreCase = false) {
    loadFromText(fileContents, ignoreCase);
  }

  /** A deep copy, fallbacks included. */
  LocalisedStrings(const LocalisedStrings& other) : LocalisedStrings() {
    copyOwnFields(other);
    LocalisedStrings* tail = this;
    for (const LocalisedStrings* f = other.fallback_.get(); f; f = f->fallback_.get()) {
      tail->fallback_.reset(new LocalisedStrings());
      tail = tail->fallback_.get();
      tail->copyOwnFields(*f);
    }
  }
  /** Replaces this table, fallbacks included, with a deep copy of
   *  `other`. */
  LocalisedStrings& operator=(const LocalisedStrings& other) {
    if (this != &other) {
      LocalisedStrings copy(other);
      *this = std::move(copy);
    }
    return *this;
  }
  /** Takes the table and its fallbacks; `other` is left valid but
   *  unspecified. */
  LocalisedStrings(LocalisedStrings&&) = default;
  /** Takes the table and its fallbacks, releasing this one's. */
  LocalisedStrings& operator=(LocalisedStrings&&) = default;

  /** A chain of fallbacks is released link by link, not recursively. */
  ~LocalisedStrings() {
    std::unique_ptr<LocalisedStrings> next = std::move(fallback_);
    while (next) {
      std::unique_ptr<LocalisedStrings> after = std::move(next->fallback_);
      next = std::move(after);
    }
  }

  /** Reads a translation file by UTF-8 path. nullptr, with `errorOut` set,
   *  if it cannot be read; a file that reads but has malformed lines loads
   *  what it can (see getMalformedLines). */
  static std::unique_ptr<LocalisedStrings> fromFile(const std::string& path, bool ignoreCase = false,
                                                    std::string* errorOut = nullptr) {
    std::FILE* f = pathio::open(path.c_str(), "rb");
    if (!f) {
      if (errorOut) *errorOut = "cannot open " + path;
      return nullptr;
    }
    std::string text;
    char buffer[16384];
    bool tooLarge = false;
    for (;;) {
      const size_t got = std::fread(buffer, 1, sizeof(buffer), f);
      if (got == 0) break;
      // A translation file is kilobytes. Sixteen megabytes is not one.
      if (text.size() + got > ((size_t) 16 << 20)) {
        tooLarge = true;
        break;
      }
      text.append(buffer, got);
    }
    const bool failed = std::ferror(f) != 0;
    std::fclose(f);
    if (failed || tooLarge) {
      if (errorOut) *errorOut = tooLarge ? path + " is too large to be a translation file"
                                         : "error reading " + path;
      return nullptr;
    }
    if (errorOut) errorOut->clear();
    return std::unique_ptr<LocalisedStrings>(new LocalisedStrings(text, ignoreCase));
  }

  /**
   * Adds the mappings, language and countries in `fileContents` to this
   * table; a mapping for an original already present replaces it. Changing
   * `ignoreCase` re-keys what is already loaded.
   */
  void loadFromText(const std::string& text, bool ignoreCase) {
    setIgnoresCase(ignoreCase);
    // A file saved as UTF-16 -- Notepad's "Unicode", which a translator on
    // Windows reaches for -- is read by its byte-order mark, as the reference
    // reads it. Taken as bytes, every other byte was a NUL, no line began
    // with a quote, and the file loaded nothing and reported nothing wrong.
    const std::string fileContents = utf8FromMarkedUtf16(text);
    size_t lineStart = 0;
    int lineNumber = 0;
    const size_t n = fileContents.size();
    while (lineStart <= n) {
      size_t lineEnd = lineStart;
      while (lineEnd < n && fileContents[lineEnd] != '\n' && fileContents[lineEnd] != '\r') ++lineEnd;
      ++lineNumber;
      std::string line = trimmed(fileContents.substr(lineStart, lineEnd - lineStart));
      if (lineNumber == 1 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line = trimmed(line.substr(3));
      readLine(line, lineNumber);
      if (lineEnd >= n) break;
      // \r\n is one line end, not two.
      lineStart = lineEnd + ((fileContents[lineEnd] == '\r' && lineEnd + 1 < n &&
                              fileContents[lineEnd + 1] == '\n') ? 2 : 1);
    }
  }

  /** The translation of `text`, from this table or its fallbacks -- or
   *  `text` itself, so an untranslated string still says something. */
  std::string translate(const std::string& text) const {
    const std::string* found = find(text);
    return found ? *found : text;
  }
  /** The translation, or `resultIfNotFound`. */
  std::string translate(const std::string& text, const std::string& resultIfNotFound) const {
    const std::string* found = find(text);
    return found ? *found : resultIfNotFound;
  }

  /** Sets one mapping directly, as a file line would. An empty original is
   *  ignored; an empty translation removes the mapping. */
  void setTranslation(const std::string& original, const std::string& translated) {
    if (original.empty()) return;
    if (translated.empty()) {
      table_.erase(keyOf(original));
      return;
    }
    table_[keyOf(original)] = Entry{original, translated};
  }

  /** Copies every mapping from `other` (not its fallbacks), replacing any
   *  this table already had for the same original. Its countries are added;
   *  its language is taken only if this table has none. */
  void addStrings(const LocalisedStrings& other) {
    for (const auto& kv : other.table_) setTranslation(kv.second.original, kv.second.translated);
    for (const std::string& c : other.countries_) addCountry(c);
    if (language_.empty()) language_ = other.language_;
  }

  /** A table consulted for anything this one lacks -- typically a regional
   *  table ("fr-CA") falling back on the general one ("fr"). Owned. */
  void setFallback(std::unique_ptr<LocalisedStrings> fallback) { fallback_ = std::move(fallback); }
  /** The fallback table, owned by this one, or null. */
  const LocalisedStrings* getFallback() const { return fallback_.get(); }

  /** The file's "language:" line, trimmed -- "French" -- or "" when it had
   *  none. */
  const std::string& getLanguageName() const { return language_; }
  /** The codes on the "countries:" line, as written and without repeats:
   *  {"fr", "be", "mc"}. */
  const std::vector<std::string>& getCountryCodes() const { return countries_; }
  /** True when originals are matched with ASCII case folded. */
  bool isIgnoringCase() const { return ignoreCase_; }
  /** Number of mappings in this table (fallbacks not counted). */
  size_t size() const { return table_.size(); }

  /** Every mapping in this table, original to translation. */
  std::map<std::string, std::string> getMappings() const {
    std::map<std::string, std::string> out;
    for (const auto& kv : table_) out[kv.second.original] = kv.second.translated;
    return out;
  }

  /** 1-based line numbers of mappings that could not be read. */
  const std::vector<int>& getMalformedLines() const { return malformed_; }

  // ── The process-wide current table ───────────────────────────────────────

  /** Installs the table translate() reads, taking ownership; nullptr goes
   *  back to no translation at all. Safe against concurrent translate(). */
  static void setCurrentMappings(std::unique_ptr<LocalisedStrings> newTranslations) {
    std::shared_ptr<const LocalisedStrings> next(std::move(newTranslations));
    Current& c = current();
    {
      std::lock_guard<std::mutex> hold(c.lock);
      c.strings.swap(next);
    }
    // The previous table, if this was the last holder, is freed here --
    // outside the lock, so a large table's destruction does not stall readers.
  }

  /** The table translate() is reading, or null. Held by the caller, it stays
   *  valid even if the language changes meanwhile. */
  static std::shared_ptr<const LocalisedStrings> getCurrentMappings() {
    Current& c = current();
    std::lock_guard<std::mutex> hold(c.lock);
    return c.strings;
  }

  /** `text` translated by the current table, or `text` itself when there is
   *  none or it lacks the string. Any thread but the audio thread: it takes
   *  a short lock and allocates. */
  static std::string translateWithCurrentMappings(const std::string& text) {
    const std::shared_ptr<const LocalisedStrings> table = getCurrentMappings();
    return table ? table->translate(text) : text;
  }
  /** The same for a C string; a null pointer is "". */
  static std::string translateWithCurrentMappings(const char* text) {
    return translateWithCurrentMappings(std::string(text ? text : ""));
  }

private:
  /** One mapping. The table is keyed by the original folded to lower case
   *  when the file asks to ignore case, so the original's own spelling is
   *  kept here for getMappings() and for rekeying. */
  struct Entry {
    std::string original;   ///< as written in the file
    std::string translated; ///< never empty: an empty one is no mapping
  };
  /** The process-wide current table, behind a mutex. Readers copy the
   *  shared_ptr out under the lock and translate without it, so a table
   *  being replaced stays alive until the last of them is done. */
  struct Current {
    std::mutex lock;                                 ///< held only to copy or swap `strings`
    std::shared_ptr<const LocalisedStrings> strings; ///< null when nothing is installed
  };
  static Current& current() {
    static Current c;
    return c;
  }

  void copyOwnFields(const LocalisedStrings& other) {
    language_ = other.language_;
    countries_ = other.countries_;
    table_ = other.table_;
    ignoreCase_ = other.ignoreCase_;
    malformed_ = other.malformed_;
  }

  static bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
  static std::string trimmed(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && isSpace(s[b])) ++b;
    while (e > b && isSpace(s[e - 1])) --e;
    return s.substr(b, e - b);
  }
  /** `text` as UTF-8 when it starts with a UTF-16 byte-order mark, in either
   *  byte order; anything else as it is (no UTF-8 text starts with FF FE or
   *  FE FF, so nothing else is mistaken for it). An unpaired surrogate
   *  becomes U+FFFD, and an odd last byte is dropped. */
  static std::string utf8FromMarkedUtf16(const std::string& text) {
    if (text.size() < 2) return text;
    const unsigned char first = (unsigned char) text[0], second = (unsigned char) text[1];
    const bool little = first == 0xFF && second == 0xFE;
    if (!little && !(first == 0xFE && second == 0xFF)) return text;
    auto unit = [&text, little](size_t at) {
      const uint32_t a = (unsigned char) text[at], b = (unsigned char) text[at + 1];
      return little ? (a | (b << 8)) : ((a << 8) | b);
    };
    std::string out;
    out.reserve(text.size());
    for (size_t at = 2; at + 1 < text.size(); at += 2) {
      uint32_t cp = unit(at);
      if (cp >= 0xD800 && cp <= 0xDBFF && at + 3 < text.size() && unit(at + 2) >= 0xDC00 &&
          unit(at + 2) <= 0xDFFF) {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (unit(at + 2) - 0xDC00);
        at += 2;
      } else if (cp >= 0xD800 && cp <= 0xDFFF) {
        cp = 0xFFFD;
      }
      if (cp < 0x80) {
        out.push_back((char) cp);
      } else if (cp < 0x800) {
        out.push_back((char) (0xC0 | (cp >> 6)));
        out.push_back((char) (0x80 | (cp & 0x3F)));
      } else if (cp < 0x10000) {
        out.push_back((char) (0xE0 | (cp >> 12)));
        out.push_back((char) (0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char) (0x80 | (cp & 0x3F)));
      } else {
        out.push_back((char) (0xF0 | (cp >> 18)));
        out.push_back((char) (0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char) (0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char) (0x80 | (cp & 0x3F)));
      }
    }
    return out;
  }

  static std::string asciiLower(std::string s) {
    for (char& c : s)
      if (c >= 'A' && c <= 'Z') c = (char) (c - 'A' + 'a');
    return s;
  }
  static bool startsWithIgnoringCase(const std::string& s, const char* prefix) {
    size_t i = 0;
    for (; prefix[i]; ++i) {
      if (i >= s.size()) return false;
      char a = s[i], b = prefix[i];
      if (a >= 'A' && a <= 'Z') a = (char) (a - 'A' + 'a');
      if (b >= 'A' && b <= 'Z') b = (char) (b - 'A' + 'a');
      if (a != b) return false;
    }
    return true;
  }

  std::string keyOf(const std::string& s) const { return ignoreCase_ ? asciiLower(s) : s; }

  void setIgnoresCase(bool ignoreCase) {
    if (ignoreCase == ignoreCase_) return;
    ignoreCase_ = ignoreCase;
    std::map<std::string, Entry> rekeyed;
    for (auto& kv : table_) rekeyed[keyOf(kv.second.original)] = std::move(kv.second);
    table_ = std::move(rekeyed);
  }

  void addCountry(const std::string& code) {
    for (const std::string& c : countries_)
      if (c == code) return;
    countries_.push_back(code);
  }

  // A double-quoted string starting just after its opening quote at `at`;
  // `at` ends just after the closing quote. False if it never closes.
  static bool readQuoted(const std::string& line, size_t& at, std::string& out) {
    while (at < line.size()) {
      const char c = line[at];
      if (c == '"') {
        ++at;
        return true;
      }
      if (c == '\\' && at + 1 < line.size()) {
        const char e = line[at + 1];
        switch (e) {
          case '"': out += '"'; break;
          case '\'': out += '\''; break;
          case '\\': out += '\\'; break;
          case 'n': out += '\n'; break;
          case 't': out += '\t'; break;
          case 'r': out += '\r'; break;
          default:
            out += '\\';
            out += e;
        }
        at += 2;
        continue;
      }
      out += c;
      ++at;
    }
    return false;
  }

  void readLine(const std::string& line, int lineNumber) {
    if (line.empty()) return;
    if (line[0] == '"') {
      size_t at = 1;
      std::string original, translated;
      bool ok = readQuoted(line, at, original);
      while (ok && at < line.size() && isSpace(line[at])) ++at;
      ok = ok && at < line.size() && line[at] == '=';
      ++at;
      while (ok && at < line.size() && isSpace(line[at])) ++at;
      ok = ok && at < line.size() && line[at] == '"';
      ++at;
      ok = ok && readQuoted(line, at, translated);
      if (!ok || original.empty()) {
        malformed_.push_back(lineNumber);
        return;
      }
      // An empty translation is a line somebody has not translated yet: the
      // original stands, as it does in the reference.
      if (!translated.empty()) table_[keyOf(original)] = Entry{original, translated};
      return;
    }
    if (startsWithIgnoringCase(line, "language:")) {
      language_ = trimmed(line.substr(9));
      return;
    }
    if (startsWithIgnoringCase(line, "countries:")) {
      std::string token;
      const std::string rest = line.substr(10) + " ";
      for (char c : rest) {
        if (isSpace(c) || c == ',') {
          if (!token.empty()) addCountry(token);
          token.clear();
        } else {
          token += c;
        }
      }
    }
  }

  // This table, then each fallback in turn -- a loop, not recursion.
  const std::string* find(const std::string& text) const {
    for (const LocalisedStrings* t = this; t; t = t->fallback_.get()) {
      const auto it = t->table_.find(t->keyOf(text));
      if (it != t->table_.end()) return &it->second.translated;
    }
    return nullptr;
  }

  std::string language_;
  std::vector<std::string> countries_;
  std::map<std::string, Entry> table_;
  bool ignoreCase_ = false;
  std::unique_ptr<LocalisedStrings> fallback_;
  std::vector<int> malformed_;
};

/** `text` translated by the current mappings, or `text` itself if there are
 *  none or they lack it. What SONORE_TRANS expands to. */
inline std::string translate(const std::string& text) {
  return LocalisedStrings::translateWithCurrentMappings(text);
}
/** The same for a C string; a null pointer is "". */
inline std::string translate(const char* text) {
  return LocalisedStrings::translateWithCurrentMappings(text);
}
/** `text` translated, or `resultIfNotFound` when no current table has it. */
inline std::string translate(const std::string& text, const std::string& resultIfNotFound) {
  const std::shared_ptr<const LocalisedStrings> table = LocalisedStrings::getCurrentMappings();
  return table ? table->translate(text, resultIfNotFound) : resultIfNotFound;
}

} // namespace sonore

/** Marks a literal as interface text and translates it. A script that scans
 *  sources for SONORE_TRANS("...") finds every string a translator needs. */
#define SONORE_TRANS(stringLiteral) ::sonore::translate(stringLiteral)

/** Marks a literal for translation WITHOUT translating it here -- for text
 *  stored now and translated when shown (a table of menu entries, say). */
#define SONORE_NEEDS_TRANS(stringLiteral) (stringLiteral)
