// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: enough Turtle to read an LV2 bundle.
//
// LV2 describes its plugins in RDF, serialised as Turtle. A host cannot even
// learn a plugin's NAME without parsing it, which is why hosting LV2 needs
// this and hosting CLAP or VST3 does not.
//
// This is a SUBSET, and the boundary is stated rather than discovered:
//
//   Supported: @prefix and @base, absolute and prefixed names, `a` as
//   rdf:type, predicate lists with `;`, object lists with `,`, blank nodes
//   both anonymous and labelled, single- and triple-quoted strings with
//   escapes, language tags, datatype suffixes, numbers, booleans, comments,
//   and collections.
//
//   Not supported: nothing else. In particular there is no attempt at
//   named graphs or TriG, and a document using them is REFUSED rather than
//   half-read. A parser that quietly skips what it does not understand hands
//   its caller a plugin description with a hole in it, and the caller has no
//   way to tell.
//
// The output is triples with blank nodes given generated labels, which is all
// a bundle reader needs: it looks up subjects by type and follows predicates.

#pragma once

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace sonore {
namespace turtle {

struct Triple {
  std::string subject;
  std::string predicate;
  std::string object;
  /** True when the object was a literal rather than a URI or blank node. The
   *  difference matters: <http://example/5> and "5" are not the same thing,
   *  and a reader that treated them alike would follow a string as a link. */
  bool objectIsLiteral = false;
  /** The literal's datatype URI, or empty. Kept because lv2:minimum 0 and
   *  lv2:minimum "0" mean different things to a strict reader. */
  std::string datatype;
};

/** One document's triples, with the prefixes it declared. */
struct Document {
  std::vector<Triple> triples;
  std::map<std::string, std::string> prefixes;

  /** Every object of (subject, predicate). Returns an empty vector rather
   *  than failing: a predicate a bundle never stated is a normal thing, not
   *  an error. */
  std::vector<std::string> objects(const std::string& subject,
                                   const std::string& predicate) const {
    std::vector<std::string> found;
    for (const Triple& t : triples)
      if (t.subject == subject && t.predicate == predicate) found.push_back(t.object);
    return found;
  }

  /** The first object of (subject, predicate), or an empty string. */
  std::string object(const std::string& subject, const std::string& predicate) const {
    for (const Triple& t : triples)
      if (t.subject == subject && t.predicate == predicate) return t.object;
    return std::string();
  }

  /** Every subject of a given rdf:type. */
  std::vector<std::string> subjectsOfType(const std::string& type) const {
    std::vector<std::string> found;
    for (const Triple& t : triples)
      if (t.predicate == "http://www.w3.org/1999/02/22-rdf-syntax-ns#type" && t.object == type)
        found.push_back(t.subject);
    return found;
  }

  bool hasType(const std::string& subject, const std::string& type) const {
    for (const Triple& t : triples)
      if (t.subject == subject && t.object == type &&
          t.predicate == "http://www.w3.org/1999/02/22-rdf-syntax-ns#type")
        return true;
    return false;
  }
};

namespace detail {

inline bool isNameStart(char c) {
  return std::isalpha((unsigned char) c) || c == '_' || (unsigned char) c >= 0x80;
}
inline bool isNameChar(char c) {
  return isNameStart(c) || std::isdigit((unsigned char) c) || c == '-' || c == '.' || c == '%';
}

/** One pass over one document. A struct rather than a pile of free functions
 *  because every step needs the cursor, the prefixes and the blank-node
 *  counter, and threading three references through everything reads worse
 *  than holding them. */
struct Parser {
  const std::string& text;
  size_t pos = 0;
  std::string base;
  Document* out = nullptr;
  int blankCounter = 0;

  Parser(const std::string& t, const std::string& baseUri, Document* d)
      : text(t), base(baseUri), out(d) {}

  bool atEnd() const { return pos >= text.size(); }
  char peek() const { return pos < text.size() ? text[pos] : '\0'; }

  void skipSpace() {
    for (;;) {
      while (pos < text.size() && std::isspace((unsigned char) text[pos])) ++pos;
      if (pos < text.size() && text[pos] == '#') {
        while (pos < text.size() && text[pos] != '\n') ++pos;
        continue;
      }
      return;
    }
  }

  bool literalAhead(const char* word) {
    const size_t n = std::strlen(word);
    if (text.compare(pos, n, word) != 0) return false;
    // A keyword has to end: "@prefixed" is not "@prefix".
    const size_t after = pos + n;
    if (after < text.size() && isNameChar(text[after])) return false;
    return true;
  }

  std::string freshBlank() { return "_:sonore" + std::to_string(++blankCounter); }

  /** Resolve a relative reference against the base, the way a bundle's own
   *  file references are written: <plugin.ttl> beside the manifest. Only the
   *  cases an LV2 bundle produces -- absolute URIs pass through, everything
   *  else is appended to the base directory. */
  std::string resolve(const std::string& ref) const {
    if (ref.find("://") != std::string::npos) return ref;
    if (!ref.empty() && ref.find(':') != std::string::npos && ref[0] != '.' && ref[0] != '/')
      return ref; // urn:, mailto:, and anything else already absolute
    if (base.empty()) return ref;
    if (ref.empty()) return base;
    std::string prefix = base;
    if (!prefix.empty() && prefix.back() != '/' && prefix.back() != '\\') prefix += '/';
    return prefix + ref;
  }

  bool parseUriRef(std::string* value) {
    if (peek() != '<') return false;
    ++pos;
    std::string raw;
    while (pos < text.size() && text[pos] != '>') {
      if (text[pos] == '\\' && pos + 1 < text.size()) {
        // \uXXXX inside a URI. Rare, and skipping it silently would build a
        // URI that does not match the one the plugin publishes.
        ++pos;
        if (text[pos] == 'u' && pos + 4 < text.size()) {
          raw += (char) std::strtol(text.substr(pos + 1, 4).c_str(), nullptr, 16);
          pos += 5;
          continue;
        }
      }
      raw += text[pos++];
    }
    if (pos >= text.size()) return false;
    ++pos; // '>'
    *value = resolve(raw);
    return true;
  }

  bool parsePrefixedName(std::string* value) {
    const size_t start = pos;
    std::string prefix;
    while (pos < text.size() && text[pos] != ':' && isNameChar(text[pos])) ++pos;
    if (pos >= text.size() || text[pos] != ':') {
      pos = start;
      return false;
    }
    prefix = text.substr(start, pos - start);
    ++pos; // ':'
    std::string local;
    while (pos < text.size() && isNameChar(text[pos])) local += text[pos++];
    // A trailing dot belongs to the statement, not the name.
    while (!local.empty() && local.back() == '.') {
      local.pop_back();
      --pos;
    }
    auto found = out->prefixes.find(prefix);
    if (found == out->prefixes.end()) return false; // undeclared: a real error
    *value = found->second + local;
    return true;
  }

  bool parseString(std::string* value) {
    const char quote = peek();
    if (quote != '"' && quote != '\'') return false;
    const bool triple = text.compare(pos, 3, std::string(3, quote)) == 0;
    pos += triple ? 3 : 1;
    std::string result;
    for (;;) {
      if (pos >= text.size()) return false;
      if (triple && text.compare(pos, 3, std::string(3, quote)) == 0) {
        pos += 3;
        break;
      }
      if (!triple && text[pos] == quote) {
        ++pos;
        break;
      }
      if (text[pos] == '\\' && pos + 1 < text.size()) {
        ++pos;
        switch (text[pos]) {
          case 'n': result += '\n'; break;
          case 'r': result += '\r'; break;
          case 't': result += '\t'; break;
          case 'b': result += '\b'; break;
          case 'f': result += '\f'; break;
          case '\\': result += '\\'; break;
          case '"': result += '"'; break;
          case '\'': result += '\''; break;
          case 'u':
            if (pos + 4 < text.size()) {
              result += (char) std::strtol(text.substr(pos + 1, 4).c_str(), nullptr, 16);
              pos += 4;
            }
            break;
          default: result += text[pos]; break;
        }
        ++pos;
        continue;
      }
      result += text[pos++];
    }
    *value = result;
    return true;
  }

  /** A number or a boolean, which Turtle writes bare. */
  bool parseBareLiteral(std::string* value, std::string* datatype) {
    const size_t start = pos;
    if (literalAhead("true") || literalAhead("false")) {
      const size_t n = literalAhead("true") ? 4u : 5u;
      *value = text.substr(pos, n);
      *datatype = "http://www.w3.org/2001/XMLSchema#boolean";
      pos += n;
      return true;
    }
    if (peek() != '+' && peek() != '-' && peek() != '.' && !std::isdigit((unsigned char) peek()))
      return false;
    if (peek() == '+' || peek() == '-') ++pos;
    bool digits = false, dot = false, exponent = false;
    while (pos < text.size()) {
      const char c = text[pos];
      if (std::isdigit((unsigned char) c)) {
        digits = true;
        ++pos;
      } else if (c == '.' && !dot && !exponent) {
        // A dot ENDS the statement unless a digit follows it, which is the
        // one genuinely ambiguous character in the grammar.
        if (pos + 1 >= text.size() || !std::isdigit((unsigned char) text[pos + 1])) break;
        dot = true;
        ++pos;
      } else if ((c == 'e' || c == 'E') && digits && !exponent) {
        exponent = true;
        ++pos;
        if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) ++pos;
      } else {
        break;
      }
    }
    if (!digits) {
      pos = start;
      return false;
    }
    *value = text.substr(start, pos - start);
    *datatype = exponent ? "http://www.w3.org/2001/XMLSchema#double"
                : dot    ? "http://www.w3.org/2001/XMLSchema#decimal"
                         : "http://www.w3.org/2001/XMLSchema#integer";
    return true;
  }

  /** Any term in object position. Blank nodes and collections recurse, which
   *  is why this emits triples as it goes rather than returning a tree. */
  bool parseObject(std::string* value, bool* isLiteral, std::string* datatype) {
    skipSpace();
    *isLiteral = false;
    datatype->clear();

    if (peek() == '<') return parseUriRef(value);

    if (peek() == '[') {
      ++pos;
      *value = freshBlank();
      skipSpace();
      if (peek() == ']') {
        ++pos;
        return true;
      }
      if (!parsePredicateList(*value)) return false;
      skipSpace();
      if (peek() != ']') return false;
      ++pos;
      return true;
    }

    if (peek() == '(') {
      // An RDF collection: a chain of rdf:first / rdf:rest ending in rdf:nil.
      ++pos;
      const std::string rdf = "http://www.w3.org/1999/02/22-rdf-syntax-ns#";
      std::vector<std::string> items;
      std::vector<bool> literals;
      std::vector<std::string> types;
      for (;;) {
        skipSpace();
        if (peek() == ')') {
          ++pos;
          break;
        }
        if (atEnd()) return false;
        std::string item, type;
        bool lit = false;
        if (!parseObject(&item, &lit, &type)) return false;
        items.push_back(item);
        literals.push_back(lit);
        types.push_back(type);
      }
      if (items.empty()) {
        *value = rdf + "nil";
        return true;
      }
      std::string head = freshBlank();
      *value = head;
      for (size_t i = 0; i < items.size(); ++i) {
        out->triples.push_back({head, rdf + "first", items[i], literals[i], types[i]});
        const std::string tail = (i + 1 < items.size()) ? freshBlank() : rdf + "nil";
        out->triples.push_back({head, rdf + "rest", tail, false, ""});
        head = tail;
      }
      return true;
    }

    if (text.compare(pos, 2, "_:") == 0) {
      pos += 2;
      std::string label;
      while (pos < text.size() && isNameChar(text[pos])) label += text[pos++];
      *value = "_:" + label;
      return true;
    }

    if (peek() == '"' || peek() == '\'') {
      if (!parseString(value)) return false;
      *isLiteral = true;
      if (peek() == '@') {
        ++pos;
        while (pos < text.size() && (std::isalnum((unsigned char) text[pos]) || text[pos] == '-'))
          ++pos;
      } else if (text.compare(pos, 2, "^^") == 0) {
        pos += 2;
        std::string type;
        if (peek() == '<') {
          if (!parseUriRef(&type)) return false;
        } else if (!parsePrefixedName(&type)) {
          return false;
        }
        *datatype = type;
      }
      return true;
    }

    std::string bare, bareType;
    if (parseBareLiteral(&bare, &bareType)) {
      *value = bare;
      *isLiteral = true;
      *datatype = bareType;
      return true;
    }
    return parsePrefixedName(value);
  }

  bool parseVerb(std::string* value) {
    skipSpace();
    if (literalAhead("a")) {
      pos += 1;
      *value = "http://www.w3.org/1999/02/22-rdf-syntax-ns#type";
      return true;
    }
    if (peek() == '<') return parseUriRef(value);
    return parsePrefixedName(value);
  }

  bool parsePredicateList(const std::string& subject) {
    for (;;) {
      skipSpace();
      if (peek() == ']' || peek() == '.' || atEnd()) return true;

      std::string predicate;
      if (!parseVerb(&predicate)) return false;

      for (;;) {
        std::string object, datatype;
        bool isLiteral = false;
        if (!parseObject(&object, &isLiteral, &datatype)) return false;
        out->triples.push_back({subject, predicate, object, isLiteral, datatype});
        skipSpace();
        if (peek() == ',') {
          ++pos;
          continue;
        }
        break;
      }

      skipSpace();
      if (peek() == ';') {
        ++pos;
        // "a ; b ; ." is legal: an empty predicate slot just ends the list.
        continue;
      }
      return true;
    }
  }

  bool parseDirective() {
    if (literalAhead("@prefix")) {
      pos += 7;
      skipSpace();
      std::string prefix;
      while (pos < text.size() && text[pos] != ':') prefix += text[pos++];
      if (pos >= text.size()) return false;
      ++pos; // ':'
      skipSpace();
      std::string uri;
      if (!parseUriRef(&uri)) return false;
      out->prefixes[prefix] = uri;
      skipSpace();
      if (peek() != '.') return false;
      ++pos;
      return true;
    }
    if (literalAhead("@base")) {
      pos += 5;
      skipSpace();
      std::string uri;
      if (!parseUriRef(&uri)) return false;
      base = uri;
      skipSpace();
      if (peek() != '.') return false;
      ++pos;
      return true;
    }
    return false;
  }

  bool run() {
    for (;;) {
      skipSpace();
      if (atEnd()) return true;

      if (peek() == '@') {
        if (!parseDirective()) return false;
        continue;
      }
      // SPARQL-style PREFIX/BASE, which some tools emit. Refused rather than
      // silently ignored: a document whose prefixes were skipped parses into
      // triples with the wrong URIs, which is worse than not parsing.
      if (literalAhead("PREFIX") || literalAhead("BASE") || literalAhead("GRAPH")) return false;

      std::string subject, datatype;
      bool isLiteral = false;
      if (!parseObject(&subject, &isLiteral, &datatype)) return false;
      if (isLiteral) return false; // a literal cannot be a subject

      if (!parsePredicateList(subject)) return false;
      skipSpace();
      if (peek() != '.') return false;
      ++pos;
    }
  }
};

} // namespace detail

/** Parse a Turtle document. `baseUri` resolves relative references -- for a
 *  bundle, the directory the file lives in. */
inline bool parse(const std::string& text, const std::string& baseUri, Document* out) {
  if (!out) return false;
  out->triples.clear();
  out->prefixes.clear();
  detail::Parser parser(text, baseUri, out);
  return parser.run();
}

} // namespace turtle
} // namespace sonore
