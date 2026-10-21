// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: XML: a tree of elements, read from text and written back.
//
// ── Why there is an XML parser here after all ───────────────────────────────
//
// For a long time the answer was no: plugin state is a StateBag, and json.h
// covers the one place a human-readable format was wanted. What changed is
// what a plugin has to READ. A VST3 preset's metadata chunk is XML. An
// .aupreset is an XML property list. A DAWproject file is a zip of XML. A
// plugin ported from the reference brings years of users' presets saved as
// that framework's XML state, and "your old presets do not open" is not a
// migration story anybody accepts. None of those formats is going to become
// JSON because this SDK would prefer it.
//
// The API is the reference's shape -- XmlElement with typed attribute getters,
// child lookup by name and by attribute, text elements; XmlDocument with
// getLastParseError -- so code moving across reads the same.
//
// ── What it is safe against ─────────────────────────────────────────────────
//
// The input is a file somebody downloaded, so it is treated as hostile:
//
//   - No external anything. A DOCTYPE is read past and its contents are
//     ignored; nothing is ever fetched or opened, so there is no XXE.
//   - No entity expansion beyond the five predefined entities and numeric
//     character references. That is what makes "billion laughs" impossible
//     rather than merely limited: an entity declared in an internal subset is
//     not expanded, and referencing it is an error that names it.
//   - Limits on input size, nesting depth and node count (attributes count
//     as nodes), all checked while parsing -- a refusal, not a stack overflow
//     or an out-of-memory.
//   - Nothing recursive. Parsing, writing, copying, comparing and destroying
//     all walk the tree with an explicit stack, so even a tree built by hand
//     a million levels deep cannot exhaust the thread's stack -- which on a
//     host's background thread may be 512 KB.
//   - Duplicate attributes are found by sorting, not by comparing every pair:
//     an element with a hundred thousand attributes is a refusal in
//     milliseconds rather than a hang.
//
// ── What it does not do, and the leniencies, named ──────────────────────────
//
// UTF-8 in, UTF-8 out. A declared encoding of ASCII or ISO-8859-1 is accepted
// (Latin-1 is transcoded first, byte for code point); UTF-16 and anything
// else are refused by name rather than misread. No validation against a DTD
// or schema, no namespace processing beyond prefix helpers.
//
// Strict where it matters -- invalid UTF-8, raw control characters, unknown
// entities, mismatched tags, '<' inside an attribute value are all errors --
// and lenient in three places where real files break the letter of the spec
// and nothing is at stake: "--" inside a comment, "]]>" in text, and numeric
// references to control characters (&#1;), which the writer itself emits so a
// string containing one survives the round trip. Names follow XML's ASCII
// rules and accept any non-ASCII character.
//
// Whitespace-only text between elements is dropped by default (the
// reference's default too); comments and processing instructions are read
// and dropped unless comments are asked for.
//
// There is no converter to JsonValue: attributes beside children, children
// whose order matters, and text mixed between elements have no one JSON
// shape, and a converter would pick one silently. A plugin that keeps XML in
// its state stores toString(TextFormat().singleLine()) as a StateBag string
// and parses it back on load -- the reference's copyXmlToBinary, in one line.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "number_text.h"
#include "path_io.h"

namespace sonore {

class XmlDocument;

/**
 * One node of an XML tree: an element with a tag name, attributes and
 * children -- or, with an empty tag name, a text node or a comment.
 *
 * Text is held in child nodes, as the reference does it, so mixed content
 * (`<p>one <b>two</b> three</p>`) keeps its order. Children are owned; a
 * pointer returned by any of the child functions stays valid until that child
 * is removed or its parent destroyed.
 */
class XmlElement {
public:
  /** An element called `tagName`, with no attributes or children. The name
   *  is not checked: isValidXmlName() does that for a name from a user. */
  explicit XmlElement(const std::string& tagName) : tag_(tagName) {}
  /** The same; a null pointer gives an empty tag name. */
  explicit XmlElement(const char* tagName) : tag_(tagName ? tagName : "") {}

  /** A deep copy: every child is copied, not shared. */
  XmlElement(const XmlElement& other)
      : kind_(other.kind_), tag_(other.tag_), text_(other.text_), attributes_(other.attributes_) {
    copyChildrenFrom(other);
  }
  /** Replaces this node and everything below it with a deep copy of
   *  `other`. The copy is made before the old tree is freed, so `other` may
   *  be a node inside this one. */
  XmlElement& operator=(const XmlElement& other) {
    if (this == &other) return *this;
    XmlElement copy(other);
    swapWith(copy);
    return *this;
  }
  /** Takes `other`'s tag, attributes and children; `other` is left valid
   *  but unspecified. Pointers to the children stay valid. */
  XmlElement(XmlElement&&) noexcept = default;
  /** Takes `other`'s tree, which may be a node inside this one
   *  (`*parent = std::move(*child)`). Pointers into `other`'s children stay
   *  valid; pointers into this node's old children do not. */
  XmlElement& operator=(XmlElement&& other) {
    if (this != &other) {
      // Take everything out of `other` BEFORE our own children are freed:
      // `other` may be one of them (`*parent = std::move(*child)`), and
      // reading it after releaseChildren() would be a use-after-free. The
      // moved-from fields are swapped into `moved`, which frees the old tree
      // iteratively when it dies.
      XmlElement moved(std::move(other));
      swapWith(moved);
    }
    return *this;
  }

  /** Iterative: a chain of a million nested elements is taken apart without
   *  a million nested destructor calls. */
  ~XmlElement() { releaseChildren(); }

  /** A text node, the way XmlDocument makes one for character data. */
  static std::unique_ptr<XmlElement> createTextElement(const std::string& text) {
    std::unique_ptr<XmlElement> e(new XmlElement(NodeKind::Text, text));
    return e;
  }
  /** A comment. Written as `<!--text-->`; a "--" inside is written as "- -",
   *  because XML has no way to escape it. */
  static std::unique_ptr<XmlElement> createCommentElement(const std::string& text) {
    std::unique_ptr<XmlElement> e(new XmlElement(NodeKind::Comment, text));
    return e;
  }

  // ── The node itself ──────────────────────────────────────────────────────

  /** The tag name, prefix included ("svg:rect"); "" for a text node or a
   *  comment. */
  const std::string& getTagName() const { return tag_; }
  /** Renames the element. Not checked, and written exactly as given. */
  void setTagName(const std::string& tagName) { tag_ = tagName; }
  /** True for an element (never a text node or a comment) whose whole tag
   *  name, prefix included, is `name`. Case-sensitive, as XML is. */
  bool hasTagName(const std::string& name) const { return kind_ == NodeKind::Element && tag_ == name; }

  /** "svg:rect" gives "rect"; a name with no prefix is returned whole. */
  std::string getTagNameWithoutNamespace() const {
    const size_t colon = tag_.rfind(':');
    return colon == std::string::npos ? tag_ : tag_.substr(colon + 1);
  }
  /** "svg:rect" gives "svg"; a name with no prefix gives "". */
  std::string getNamespace() const {
    const size_t colon = tag_.find(':');
    return colon == std::string::npos ? std::string() : tag_.substr(0, colon);
  }
  /** True for an element whose name without its prefix is `name`: "rect"
   *  matches both `<rect>` and `<svg:rect>`. */
  bool hasTagNameIgnoringNamespace(const std::string& name) const {
    return kind_ == NodeKind::Element && getTagNameWithoutNamespace() == name;
  }

  /** True for a text node: character data, from the parser or from
   *  createTextElement() and addTextElement(). */
  bool isTextElement() const { return kind_ == NodeKind::Text; }
  /** True for a comment: from createCommentElement(), or from the parser
   *  when XmlDocument::setCommentsKept(true) was called. */
  bool isCommentElement() const { return kind_ == NodeKind::Comment; }

  /** A text node's or a comment's text; "" for an element (an element's text
   *  is in its children -- see getAllSubText). */
  const std::string& getText() const { return text_; }

  /** On a text node or a comment, replaces its text. On an element, replaces
   *  all of its CHILDREN with one text node holding `text`, which is what
   *  `<name>text</name>` means. */
  void setText(const std::string& text) {
    if (kind_ != NodeKind::Element) {
      text_ = text;
      return;
    }
    // The new node first: `text` may belong to one of the children about to
    // be deleted -- e.setText(e.getFirstChildElement()->getText()) -- and was
    // read after it was freed.
    std::unique_ptr<XmlElement> node = createTextElement(text);
    deleteAllChildElements();
    addChildElement(std::move(node));
  }

  /** Every text node below this one, concatenated in document order. Comments
   *  are not text and are skipped. */
  std::string getAllSubText() const {
    if (kind_ == NodeKind::Text) return text_;
    std::string out;
    std::vector<const XmlElement*> pending;
    for (size_t i = children_.size(); i-- > 0;) pending.push_back(children_[i].get());
    while (!pending.empty()) {
      const XmlElement* e = pending.back();
      pending.pop_back();
      if (e->kind_ == NodeKind::Text) out += e->text_;
      for (size_t i = e->children_.size(); i-- > 0;) pending.push_back(e->children_[i].get());
    }
    return out;
  }

  /** The text of the first child called `childTagName`, or `defaultReturnValue`
   *  if there is no such child. */
  std::string getChildElementAllSubText(const std::string& childTagName,
                                        const std::string& defaultReturnValue) const {
    const XmlElement* child = getChildByName(childTagName);
    return child ? child->getAllSubText() : defaultReturnValue;
  }

  // ── Attributes ───────────────────────────────────────────────────────────
  //
  // Kept in the order they were set or read, so a file written back keeps
  // its attributes where a person put them.

  /** How many attributes this element has. */
  int getNumAttributes() const { return (int) attributes_.size(); }
  /** The name of attribute `index` (0-based, in the order above), or "" when
   *  the index is out of range. */
  const std::string& getAttributeName(int index) const {
    return index >= 0 && (size_t) index < attributes_.size() ? attributes_[(size_t) index].name
                                                              : emptyString();
  }
  /** The value of attribute `index`, entities already resolved, or "" when
   *  the index is out of range. */
  const std::string& getAttributeValue(int index) const {
    return index >= 0 && (size_t) index < attributes_.size() ? attributes_[(size_t) index].value
                                                              : emptyString();
  }

  /** True if the attribute is present, even with an empty value. The name
   *  is matched exactly, prefix included. */
  bool hasAttribute(const std::string& name) const { return findAttribute(name) != nullptr; }

  /** The value, or "" when there is no such attribute. */
  const std::string& getStringAttribute(const std::string& name) const {
    const Attribute* a = findAttribute(name);
    return a ? a->value : emptyString();
  }
  /** The value, or `defaultValue` when there is no such attribute. An
   *  attribute that is present but empty gives "", not the default. */
  std::string getStringAttribute(const std::string& name, const std::string& defaultValue) const {
    const Attribute* a = findAttribute(name);
    return a ? a->value : defaultValue;
  }

  /**
   * The value as an int, or `defaultValue` when the attribute is missing or is
   * not a number. Whole-value: "12" is 12, "12px" is the default, not 12 --
   * reading the leading digits of anything is how a corrupted file loads as
   * plausible numbers. A number with a fraction or an exponent ("2.7",
   * "1e3") is truncated toward zero; anything past int's range saturates
   * rather than wrapping, because converting such a double is undefined.
   */
  int getIntAttribute(const std::string& name, int defaultValue = 0) const {
    const Attribute* a = findAttribute(name);
    if (!a) return defaultValue;
    const std::string v = trimmed(a->value);
    if (v.empty()) return defaultValue;
    // Integer text first, exactly: a long run of digits read through double
    // would lose the low digits before saturating.
    size_t i = (v[0] == '-' || v[0] == '+') ? 1u : 0u;
    if (i < v.size() && v.find_first_not_of("0123456789", i) == std::string::npos) {
      const bool negative = v[0] == '-';
      long long magnitude = 0;
      for (; i < v.size(); ++i) {
        magnitude = magnitude * 10 + (v[i] - '0');
        if (magnitude > 2147483648LL) break;
      }
      if (negative) return magnitude >= 2147483648LL ? (std::numeric_limits<int>::min)()
                                                     : (int) -magnitude;
      return magnitude >= 2147483647LL ? (std::numeric_limits<int>::max)() : (int) magnitude;
    }
    double d = 0.0;
    if (!parseWholeNumber(v, d) || d != d) return defaultValue;
    if (d >= 2147483647.0) return (std::numeric_limits<int>::max)();
    if (d <= -2147483648.0) return (std::numeric_limits<int>::min)();
    return (int) d;
  }

  /** The value as a double, or `defaultValue`. Read with '.' as the point
   *  whatever locale the host set, and whole-value like getIntAttribute.
   *  "inf", "-inf" and "nan" read back what setAttribute writes for them. */
  double getDoubleAttribute(const std::string& name, double defaultValue = 0.0) const {
    const Attribute* a = findAttribute(name);
    if (!a) return defaultValue;
    double d = 0.0;
    return parseWholeNumber(trimmed(a->value), d) ? d : defaultValue;
  }

  /**
   * The value as a bool: "true", "yes", "on", "y" and any non-zero number
   * are true; "false", "no", "off", "n" and zero are false (letters in any
   * case). Anything else gives `defaultValue` -- an attribute that says
   * "maybe" is not quietly false.
   */
  bool getBoolAttribute(const std::string& name, bool defaultValue = false) const {
    const Attribute* a = findAttribute(name);
    if (!a) return defaultValue;
    const std::string v = asciiLower(trimmed(a->value));
    if (v == "true" || v == "yes" || v == "on" || v == "y") return true;
    if (v == "false" || v == "no" || v == "off" || v == "n") return false;
    double d = 0.0;
    if (parseWholeNumber(v, d) && d == d) return d != 0.0;
    return defaultValue;
  }

  /** True if the attribute exists and equals `value` (ASCII case folded when
   *  asked; full Unicode case folding is not attempted). */
  bool compareAttribute(const std::string& name, const std::string& value,
                        bool ignoreCase = false) const {
    const Attribute* a = findAttribute(name);
    if (!a) return false;
    return ignoreCase ? asciiLower(a->value) == asciiLower(value) : a->value == value;
  }

  /** Sets, replacing any existing value in place (so its position is kept). */
  void setAttribute(const std::string& name, const std::string& value) {
    if (Attribute* a = findAttribute(name)) {
      a->value = value;
      return;
    }
    attributes_.push_back(Attribute{name, value});
  }
  /** The same for a C string; a null pointer sets "". */
  void setAttribute(const std::string& name, const char* value) {
    setAttribute(name, std::string(value ? value : ""));
  }
  /** Written as "true" or "false". */
  void setAttribute(const std::string& name, bool value) {
    setAttribute(name, std::string(value ? "true" : "false"));
  }
  /** Any integer type, so `setAttribute("n", list.size())` is not an
   *  ambiguous call between int and double. */
  template <typename Integer,
            typename std::enable_if<std::is_integral<Integer>::value &&
                                        !std::is_same<Integer, bool>::value,
                                    int>::type = 0>
  void setAttribute(const std::string& name, Integer value) {
    char buffer[32];
    if constexpr (std::is_signed<Integer>::value)
      std::snprintf(buffer, sizeof(buffer), "%lld", (long long) value);
    else
      std::snprintf(buffer, sizeof(buffer), "%llu", (unsigned long long) value);
    setAttribute(name, std::string(buffer));
  }
  /** The shortest text that reads back as exactly the same double, with '.'
   *  as the point whatever the host's locale: 0.1 is "0.1", not
   *  "0.10000000000000001". Non-finite values are "inf", "-inf", "nan". */
  void setAttribute(const std::string& name, double value) {
    setAttribute(name, formatDouble(value));
  }
  /** Widened to double and written as that double: exact, but 0.1f takes
   *  more digits than 0.1 does. */
  void setAttribute(const std::string& name, float value) { setAttribute(name, (double) value); }

  /** Removes the attribute if there is one; the rest keep their order. */
  void removeAttribute(const std::string& name) {
    for (size_t i = 0; i < attributes_.size(); ++i)
      if (attributes_[i].name == name) {
        attributes_.erase(attributes_.begin() + (std::ptrdiff_t) i);
        return;
      }
  }
  /** Removes every attribute. Children are untouched. */
  void removeAllAttributes() { attributes_.clear(); }

  // ── Children ─────────────────────────────────────────────────────────────
  //
  // "Child elements" includes text nodes and comments, as in the reference:
  // getNumChildElements counts them, and isTextElement tells them apart. The
  // lookups by name and attribute only ever match elements.

  /** How many direct children, text nodes and comments included. */
  int getNumChildElements() const { return (int) children_.size(); }
  /** Direct child `index` (0-based) of any kind, or nullptr when the index
   *  is out of range. Owned by this element. */
  XmlElement* getChildElement(int index) const {
    return index >= 0 && (size_t) index < children_.size() ? children_[(size_t) index].get()
                                                            : nullptr;
  }
  /** The first direct child of any kind -- possibly a text node -- or
   *  nullptr when there are none. Owned by this element. */
  XmlElement* getFirstChildElement() const {
    return children_.empty() ? nullptr : children_.front().get();
  }
  /** The first child ELEMENT with this tag name, or nullptr. */
  XmlElement* getChildByName(const std::string& tagName) const {
    for (const auto& c : children_)
      if (c->kind_ == NodeKind::Element && c->tag_ == tagName) return c.get();
    return nullptr;
  }
  /** The first child element whose attribute `attributeName` is exactly
   *  `attributeValue`, or nullptr. */
  XmlElement* getChildByAttribute(const std::string& attributeName,
                                  const std::string& attributeValue) const {
    for (const auto& c : children_) {
      if (c->kind_ != NodeKind::Element) continue;
      const Attribute* a = c->findAttribute(attributeName);
      if (a && a->value == attributeValue) return c.get();
    }
    return nullptr;
  }
  /** Position among this node's children, or -1. */
  int getIndexOfChildElement(const XmlElement* child) const {
    for (size_t i = 0; i < children_.size(); ++i)
      if (children_[i].get() == child) return (int) i;
    return -1;
  }
  /** Is `child` one of this node's DIRECT children? */
  bool containsChildElement(const XmlElement* child) const {
    return getIndexOfChildElement(child) >= 0;
  }
  /** The element anywhere below this one whose child `descendant` is, or
   *  nullptr if it is not in this tree. */
  XmlElement* findParentElementOf(const XmlElement* descendant) {
    if (!descendant || descendant == this) return nullptr;
    std::vector<XmlElement*> pending{this};
    while (!pending.empty()) {
      XmlElement* e = pending.back();
      pending.pop_back();
      for (const auto& c : e->children_) {
        if (c.get() == descendant) return e;
        pending.push_back(c.get());
      }
    }
    return nullptr;
  }

  /** Adds a new element at the end and returns it (owned by this one). */
  XmlElement* createNewChildElement(const std::string& tagName) {
    children_.push_back(std::unique_ptr<XmlElement>(new XmlElement(tagName)));
    return children_.back().get();
  }
  /** Takes ownership and appends. Returns the child, or nullptr for nullptr. */
  XmlElement* addChildElement(std::unique_ptr<XmlElement> child) {
    if (!child) return nullptr;
    children_.push_back(std::move(child));
    return children_.back().get();
  }
  /** Takes ownership and inserts before `index`; an index out of range
   *  appends. */
  XmlElement* insertChildElement(std::unique_ptr<XmlElement> child, int index) {
    if (!child) return nullptr;
    XmlElement* raw = child.get();
    if (index < 0 || (size_t) index >= children_.size())
      children_.push_back(std::move(child));
    else
      children_.insert(children_.begin() + index, std::move(child));
    return raw;
  }
  /** Takes ownership and inserts at the front. Returns the child, or
   *  nullptr for nullptr. */
  XmlElement* prependChildElement(std::unique_ptr<XmlElement> child) {
    return insertChildElement(std::move(child), 0);
  }
  /** Appends a text node. */
  XmlElement* addTextElement(const std::string& text) {
    return addChildElement(createTextElement(text));
  }

  /** Detaches a direct child and hands it back -- let the pointer go to
   *  delete it. nullptr if `child` is not a direct child. */
  std::unique_ptr<XmlElement> removeChildElement(XmlElement* child) {
    for (size_t i = 0; i < children_.size(); ++i)
      if (children_[i].get() == child) {
        std::unique_ptr<XmlElement> out = std::move(children_[i]);
        children_.erase(children_.begin() + (std::ptrdiff_t) i);
        return out;
      }
    return nullptr;
  }
  /** Replaces a direct child in place; false (and `replacement` discarded)
   *  if `current` is not a direct child. */
  bool replaceChildElement(XmlElement* current, std::unique_ptr<XmlElement> replacement) {
    if (!replacement) return false;
    for (auto& c : children_)
      if (c.get() == current) {
        c = std::move(replacement);
        return true;
      }
    return false;
  }
  /** Deletes every child, text nodes and comments included. Pointers to
   *  any of them, or to anything below them, dangle afterwards. */
  void deleteAllChildElements() {
    std::vector<std::unique_ptr<XmlElement>> doomed = std::move(children_);
    children_.clear();
    releaseList(doomed);
  }
  /** Deletes the direct child elements called `tagName` (exact match,
   *  prefix included); every other child keeps its place. */
  void deleteAllChildElementsWithTagName(const std::string& tagName) {
    std::vector<std::unique_ptr<XmlElement>> doomed;
    std::vector<std::unique_ptr<XmlElement>> kept;
    for (auto& c : children_)
      (c->kind_ == NodeKind::Element && c->tag_ == tagName ? doomed : kept).push_back(std::move(c));
    children_ = std::move(kept);
    releaseList(doomed);
  }
  /** Deletes the direct children that are text nodes. Comments, elements
   *  and the text inside child elements are kept. */
  void deleteAllTextElements() {
    std::vector<std::unique_ptr<XmlElement>> kept;
    for (auto& c : children_)
      if (c->kind_ != NodeKind::Text) kept.push_back(std::move(c));
    children_ = std::move(kept);
  }

  /** `for (auto* child : e.getChildIterator())` -- every child, text nodes and
   *  comments included. */
  class ChildRange {
  public:
    /** A forward iterator over the children, yielding `XmlElement*`; when
     *  the range is filtered by tag it steps over everything else. It points
     *  into the element's list of children, so adding or removing children
     *  during the loop invalidates it, as it would a vector's. */
    class Iterator {
    public:
      /** The child here, owned by the element. Not valid on end(). */
      XmlElement* operator*() const { return (*list_)[at_].get(); }
      /** On to the next child, or the next matching one when filtered. */
      Iterator& operator++() {
        ++at_;
        skip();
        return *this;
      }
      ///@{
      /** By position only, so compare iterators from the same range. */
      bool operator!=(const Iterator& other) const { return at_ != other.at_; }
      bool operator==(const Iterator& other) const { return at_ == other.at_; }
      ///@}

    private:
      friend class ChildRange;
      Iterator(const std::vector<std::unique_ptr<XmlElement>>* list, size_t at, const std::string* tag)
          : list_(list), at_(at), tag_(tag) {
        skip();
      }
      void skip() {
        if (!tag_) return;
        while (at_ < list_->size() &&
               !((*list_)[at_]->kind_ == NodeKind::Element && (*list_)[at_]->tag_ == *tag_))
          ++at_;
      }
      const std::vector<std::unique_ptr<XmlElement>>* list_;
      size_t at_;
      const std::string* tag_;
    };
    /** The first child, or the first matching one when filtered. */
    Iterator begin() const { return Iterator(list_, 0, filtered_ ? &tag_ : nullptr); }
    /** One past the last child. */
    Iterator end() const { return Iterator(list_, list_->size(), nullptr); }

  private:
    friend class XmlElement;
    ChildRange(const std::vector<std::unique_ptr<XmlElement>>* list, const std::string* tag)
        : list_(list), tag_(tag ? *tag : std::string()), filtered_(tag != nullptr) {}
    const std::vector<std::unique_ptr<XmlElement>>* list_;
    std::string tag_;
    bool filtered_;
  };
  /** Every direct child, text nodes and comments included, in order. */
  ChildRange getChildIterator() const { return ChildRange(&children_, nullptr); }
  /** Only the child elements called `tagName`. */
  ChildRange getChildWithTagNameIterator(const std::string& tagName) const {
    return ChildRange(&children_, &tagName);
  }

  // ── Comparing ────────────────────────────────────────────────────────────

  /** Same tag, same attributes (in order, unless told not to care), same
   *  children recursively -- the same document. */
  bool isEquivalentTo(const XmlElement* other, bool ignoreOrderOfAttributes) const {
    if (!other) return false;
    std::vector<std::pair<const XmlElement*, const XmlElement*>> pending{{this, other}};
    while (!pending.empty()) {
      const XmlElement* a = pending.back().first;
      const XmlElement* b = pending.back().second;
      pending.pop_back();
      if (a->kind_ != b->kind_ || a->tag_ != b->tag_ || a->text_ != b->text_ ||
          a->attributes_.size() != b->attributes_.size() ||
          a->children_.size() != b->children_.size())
        return false;
      if (ignoreOrderOfAttributes && a->attributes_.size() > 8) {
        if (!sameAttributesInAnyOrder(*a, *b)) return false;
      } else {
        for (size_t i = 0; i < a->attributes_.size(); ++i) {
          if (ignoreOrderOfAttributes) {
            const Attribute* match = b->findAttribute(a->attributes_[i].name);
            if (!match || match->value != a->attributes_[i].value) return false;
          } else if (a->attributes_[i].name != b->attributes_[i].name ||
                     a->attributes_[i].value != b->attributes_[i].value) {
            return false;
          }
        }
      }
      for (size_t i = 0; i < a->children_.size(); ++i)
        pending.emplace_back(a->children_[i].get(), b->children_[i].get());
    }
    return true;
  }

  /** XML's Name production over ASCII, any non-ASCII byte allowed: what a
   *  tag or attribute name may be. Names are NOT checked when set -- the
   *  writer writes what it is given -- so this is for checking user input. */
  static bool isValidXmlName(const std::string& name) {
    if (name.empty() || !isNameStart((unsigned char) name[0])) return false;
    for (unsigned char c : name)
      if (!isNameChar(c)) return false;
    return true;
  }

  // ── Writing ──────────────────────────────────────────────────────────────

  /**
   * How toString lays a document out. The defaults write the XML declaration,
   * indent by two spaces, and wrap an element's attributes onto further
   * lines once its start tag passes 60 columns.
   *
   * An element that has text among its children is written on one line with
   * everything below it, whatever the settings: whitespace added inside
   * mixed content would be part of the text when it is read back.
   */
  struct TextFormat {
    std::string dtd;            ///< written after the declaration when not empty
    std::string customHeader;   ///< replaces the XML declaration when not empty
    std::string customEncoding; ///< the encoding the declaration names; "UTF-8" if empty
    bool addDefaultHeader = true;  ///< write `<?xml version="1.0" encoding="..."?>` when there is no customHeader
    int lineWrapLength = 60;    ///< 0: attributes never wrap
    std::string newLine = "\n"; ///< empty: the whole document on one line
    int indentSize = 2;            ///< spaces per level of nesting; 0 or less: no indent

    /** A copy that writes the whole document on one line, attributes never
     *  wrapped: the form to keep in a StateBag string. */
    TextFormat singleLine() const {
      TextFormat f = *this;
      f.newLine.clear();
      f.lineWrapLength = 0;
      return f;
    }
    /** A copy with no XML declaration, custom header or DTD: for text that
     *  goes inside another document. */
    TextFormat withoutHeader() const {
      TextFormat f = *this;
      f.addDefaultHeader = false;
      f.customHeader.clear();
      f.dtd.clear();
      return f;
    }
  };

  /** This node and everything below it as text, with the default
   *  TextFormat: an XML declaration, two-space indent, a final newline. */
  std::string toString() const { return toString(TextFormat()); }
  /** The same, laid out by `format`. The header and DTD are written only
   *  when this node is an element; a text node or a comment is written
   *  alone, escaped. Invalid UTF-8 in values, text and comments is written
   *  as U+FFFD; names are written exactly as given. */
  std::string toString(const TextFormat& format) const {
    std::string out;
    if (kind_ == NodeKind::Element) {
      bool wroteHeader = false;
      if (!format.customHeader.empty()) {
        out += format.customHeader;
        wroteHeader = true;
      } else if (format.addDefaultHeader) {
        out += "<?xml version=\"1.0\" encoding=\"";
        out += format.customEncoding.empty() ? std::string("UTF-8") : format.customEncoding;
        out += "\"?>";
        wroteHeader = true;
      }
      if (wroteHeader) out += format.newLine;
      if (!format.dtd.empty()) {
        out += format.dtd;
        out += format.newLine;
      }
    }
    writeTree(out, format);
    if (kind_ == NodeKind::Element && !format.newLine.empty()) out += format.newLine;
    return out;
  }

  /** Writes toString(format) to a UTF-8 path. False if the file could not be
   *  written completely. */
  bool writeToFile(const std::string& path) const { return writeToFile(path, TextFormat()); }
  /** The same, laid out by `format`. The file is replaced, not appended
   *  to. */
  bool writeToFile(const std::string& path, const TextFormat& format) const {
    const std::string text = toString(format);
    std::FILE* f = pathio::open(path.c_str(), "wb");
    if (!f) return false;
    const bool wrote = text.empty() || std::fwrite(text.data(), 1, text.size(), f) == text.size();
    const bool closed = std::fclose(f) == 0;
    return wrote && closed;
  }

private:
  friend class XmlDocument;
  enum class NodeKind : unsigned char { Element, Text, Comment };

  /** One attribute, its value as text the document means rather than as
   *  written (entities resolved on reading, escaped again on writing). Kept
   *  in document order, which is the order they are written back; setting
   *  an existing one keeps its place. */
  struct Attribute {
    std::string name;  ///< as written, prefix included
    std::string value; ///< decoded text
  };

  XmlElement(NodeKind kind, const std::string& text) : kind_(kind), text_(text) {}

  static const std::string& emptyString() {
    static const std::string empty;
    return empty;
  }

  const Attribute* findAttribute(const std::string& name) const {
    for (const Attribute& a : attributes_)
      if (a.name == name) return &a;
    return nullptr;
  }
  Attribute* findAttribute(const std::string& name) {
    for (Attribute& a : attributes_)
      if (a.name == name) return &a;
    return nullptr;
  }

  // Same attributes whatever their order, for elements of the same count.
  // Both lists sorted by name once, the way the parser finds duplicates:
  // looking each one up in the other is n^2, and an element of 80,000
  // attributes took 19 s to compare. Names are unique within an element
  // (setAttribute replaces, the parser refuses repeats), so equal sorted
  // lists are equal sets.
  static bool sameAttributesInAnyOrder(const XmlElement& a, const XmlElement& b) {
    std::vector<const Attribute*> x, y;
    x.reserve(a.attributes_.size());
    y.reserve(b.attributes_.size());
    for (const Attribute& at : a.attributes_) x.push_back(&at);
    for (const Attribute& at : b.attributes_) y.push_back(&at);
    const auto byName = [](const Attribute* p, const Attribute* q) { return p->name < q->name; };
    std::sort(x.begin(), x.end(), byName);
    std::sort(y.begin(), y.end(), byName);
    for (size_t i = 0; i < x.size(); ++i)
      if (x[i]->name != y[i]->name || x[i]->value != y[i]->value) return false;
    return true;
  }

  void swapWith(XmlElement& other) noexcept {
    std::swap(kind_, other.kind_);
    tag_.swap(other.tag_);
    text_.swap(other.text_);
    attributes_.swap(other.attributes_);
    children_.swap(other.children_);
  }

  // Copies the children of `other` into this (empty) node with an explicit
  // stack of (source, destination) pairs.
  void copyChildrenFrom(const XmlElement& other) {
    std::vector<std::pair<const XmlElement*, XmlElement*>> pending{{&other, this}};
    while (!pending.empty()) {
      const XmlElement* from = pending.back().first;
      XmlElement* to = pending.back().second;
      pending.pop_back();
      to->children_.reserve(from->children_.size());
      for (const auto& c : from->children_) {
        std::unique_ptr<XmlElement> copy(new XmlElement(c->kind_, c->text_));
        copy->tag_ = c->tag_;
        copy->attributes_ = c->attributes_;
        to->children_.push_back(std::move(copy));
        if (!c->children_.empty()) pending.emplace_back(c.get(), to->children_.back().get());
      }
    }
  }

  // Destroys a list of subtrees without recursing: each node's children are
  // moved onto the work list before the node itself is deleted, so every
  // destructor that runs finds no children to recurse into.
  static void releaseList(std::vector<std::unique_ptr<XmlElement>>& list) {
    while (!list.empty()) {
      std::unique_ptr<XmlElement> node = std::move(list.back());
      list.pop_back();
      for (auto& c : node->children_) list.push_back(std::move(c));
      node->children_.clear();
    }
  }
  void releaseChildren() {
    if (children_.empty()) return;
    std::vector<std::unique_ptr<XmlElement>> doomed = std::move(children_);
    children_.clear();
    releaseList(doomed);
  }

  // ── Small text helpers (private: other headers keep their own) ───────────

  static bool isSpace(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
  static bool isNameStart(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':' || c >= 0x80;
  }
  static bool isNameChar(unsigned char c) {
    return isNameStart(c) || (c >= '0' && c <= '9') || c == '-' || c == '.';
  }
  static std::string trimmed(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && isSpace((unsigned char) s[b])) ++b;
    while (e > b && isSpace((unsigned char) s[e - 1])) --e;
    return s.substr(b, e - b);
  }
  static std::string asciiLower(std::string s) {
    for (char& c : s)
      if (c >= 'A' && c <= 'Z') c = (char) (c - 'A' + 'a');
    return s;
  }

  // The whole of `text` as a number, locale-independent; "inf", "-inf",
  // "+inf" and "nan" (any case) included, as formatDouble writes them.
  static bool parseWholeNumber(const std::string& text, double& out) {
    if (text.empty()) return false;
    const std::string lower = asciiLower(text);
    if (lower == "inf" || lower == "+inf" || lower == "infinity") {
      out = std::numeric_limits<double>::infinity();
      return true;
    }
    if (lower == "-inf" || lower == "-infinity") {
      out = -std::numeric_limits<double>::infinity();
      return true;
    }
    if (lower == "nan") {
      out = std::numeric_limits<double>::quiet_NaN();
      return true;
    }
    double v = 0.0;
    if (numtext::parse(text.c_str(), text.size(), &v) != text.size()) return false;
    out = v;
    return true;
  }

  static std::string formatDouble(double value) {
    if (value != value) return "nan";
    if (value == std::numeric_limits<double>::infinity()) return "inf";
    if (value == -std::numeric_limits<double>::infinity()) return "-inf";
    char buffer[40];
    // The shortest of 15, 16 and 17 significant digits that reads back as the
    // same double: 17 always does, and most values need fewer.
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

  // Appends `text` with what XML requires escaped. Invalid UTF-8 becomes
  // U+FFFD, so whatever a string held, the output is a document a parser
  // accepts. NUL has no representation in XML at all and is dropped.
  static void appendEscaped(std::string& out, const std::string& text, bool inAttribute) {
    const size_t n = text.size();
    for (size_t i = 0; i < n;) {
      const unsigned char c = (unsigned char) text[i];
      if (c < 0x80) {
        ++i;
        switch (c) {
          case '&': out += "&amp;"; break;
          case '<': out += "&lt;"; break;
          case '>': out += "&gt;"; break;
          case '"':
            if (inAttribute) out += "&quot;";
            else out += '"';
            break;
          case '\t':
            if (inAttribute) out += "&#9;";
            else out += '\t';
            break;
          case '\n':
            if (inAttribute) out += "&#10;";
            else out += '\n';
            break;
          // A raw CR would be normalised to LF by any reader; as a reference
          // it survives, which is the point of writing it.
          case '\r': out += "&#13;"; break;
          case 0: break;
          default:
            if (c < 0x20 || c == 0x7f) {
              char ref[12];
              std::snprintf(ref, sizeof(ref), "&#%u;", (unsigned) c);
              out += ref;
            } else {
              out += (char) c;
            }
        }
        continue;
      }
      const size_t len = utf8SequenceLength(text, i);
      if (len == 0) {
        out += "\xEF\xBF\xBD";
        ++i;
      } else {
        out.append(text, i, len);
        i += len;
      }
    }
  }

  // Length of the valid UTF-8 sequence starting at `at` (2..4), or 0 if the
  // bytes there are not one: overlong forms, surrogates and anything past
  // U+10FFFF are not UTF-8.
  static size_t utf8SequenceLength(const std::string& s, size_t at) {
    const unsigned char c = (unsigned char) s[at];
    size_t len;
    uint32_t cp;
    if (c >= 0xC2 && c <= 0xDF) {
      len = 2;
      cp = c & 0x1Fu;
    } else if (c >= 0xE0 && c <= 0xEF) {
      len = 3;
      cp = c & 0x0Fu;
    } else if (c >= 0xF0 && c <= 0xF4) {
      len = 4;
      cp = c & 0x07u;
    } else {
      return 0;
    }
    if (at + len > s.size()) return 0;
    for (size_t k = 1; k < len; ++k) {
      const unsigned char cc = (unsigned char) s[at + k];
      if ((cc & 0xC0) != 0x80) return 0;
      cp = (cp << 6) | (cc & 0x3Fu);
    }
    if ((len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) || cp > 0x10FFFF ||
        (cp >= 0xD800 && cp <= 0xDFFF))
      return 0;
    return len;
  }

  static void appendCommentText(std::string& out, const std::string& text) {
    // XML cannot escape "--" inside a comment, and a comment may not end in
    // '-' either; a space between is the least change that stays a comment.
    std::string safe;
    safe.reserve(text.size());
    for (char c : text) {
      if (c == '-' && !safe.empty() && safe.back() == '-') safe += ' ';
      safe += c;
    }
    if (!safe.empty() && safe.back() == '-') safe += ' ';
    std::string escaped;
    // Comments are not entity-decoded, so only the encoding is repaired here.
    for (size_t i = 0; i < safe.size();) {
      const unsigned char c = (unsigned char) safe[i];
      if (c < 0x80) {
        if (c != 0) escaped += (char) c;
        ++i;
        continue;
      }
      const size_t len = utf8SequenceLength(safe, i);
      if (len == 0) {
        escaped += "\xEF\xBF\xBD";
        ++i;
      } else {
        escaped.append(safe, i, len);
        i += len;
      }
    }
    out += "<!--";
    out += escaped;
    out += "-->";
  }

  bool hasTextChild() const {
    for (const auto& c : children_)
      if (c->kind_ == NodeKind::Text) return true;
    return false;
  }

  // '<', the tag name and the attributes -- wrapped, aligned under the first,
  // once the line passes lineWrapLength. The caller closes it with '>' or
  // '/>'.
  void writeStartTag(std::string& out, const TextFormat& format, size_t lineStart) const {
    out += '<';
    out += tag_;
    const size_t alignTo = out.size() - lineStart + 1;
    for (size_t i = 0; i < attributes_.size(); ++i) {
      std::string piece = attributes_[i].name;
      piece += "=\"";
      appendEscaped(piece, attributes_[i].value, true);
      piece += '"';
      const size_t column = out.size() - lineStart;
      if (i > 0 && format.lineWrapLength > 0 && !format.newLine.empty() &&
          column + 1 + piece.size() > (size_t) format.lineWrapLength) {
        out += format.newLine;
        lineStart = out.size();
        out.append(alignTo, ' ');
      } else {
        out += ' ';
      }
      out += piece;
    }
  }

  void writeTree(std::string& out, const TextFormat& format) const {
    if (kind_ == NodeKind::Text) {
      appendEscaped(out, text_, false);
      return;
    }
    if (kind_ == NodeKind::Comment) {
      appendCommentText(out, text_);
      return;
    }
    const bool pretty = !format.newLine.empty();
    const size_t indent = (size_t) (std::max)(0, format.indentSize);
    struct Frame {
      const XmlElement* element;
      size_t next;
      bool oneLine; // mixed content at or above: no whitespace may be added
      size_t depth;
    };
    std::vector<Frame> stack;
    size_t lineStart = out.rfind('\n');
    lineStart = lineStart == std::string::npos ? 0 : lineStart + 1;

    writeStartTag(out, format, lineStart);
    if (children_.empty()) {
      out += "/>";
      return;
    }
    out += '>';
    stack.push_back(Frame{this, 0, hasTextChild(), 0});
    while (!stack.empty()) {
      Frame& top = stack.back();
      if (top.next == top.element->children_.size()) {
        if (pretty && !top.oneLine) {
          out += format.newLine;
          out.append(indent * top.depth, ' ');
        }
        out += "</";
        out += top.element->tag_;
        out += '>';
        stack.pop_back();
        continue;
      }
      const XmlElement& child = *top.element->children_[top.next++];
      const bool oneLine = top.oneLine;
      const size_t depth = top.depth + 1;
      if (pretty && !oneLine) {
        out += format.newLine;
        out.append(indent * depth, ' ');
      }
      if (child.kind_ == NodeKind::Text) {
        appendEscaped(out, child.text_, false);
      } else if (child.kind_ == NodeKind::Comment) {
        appendCommentText(out, child.text_);
      } else {
        // The start tag's column is only needed to align its attributes when
        // they wrap, which needs wrapping on and at least two attributes (only
        // an attribute after the first ever wraps). Everywhere else -- a
        // single-line document, or mixed content written on one long line --
        // there is no newline to find, and scanning the whole output for one
        // on every child would make writing quadratic.
        size_t ls = 0;
        if (!format.newLine.empty() && format.lineWrapLength > 0 && child.attributes_.size() > 1) {
          ls = out.rfind('\n');
          ls = ls == std::string::npos ? 0 : ls + 1;
        }
        child.writeStartTag(out, format, ls);
        if (child.children_.empty()) {
          out += "/>";
        } else {
          out += '>';
          stack.push_back(Frame{&child, 0, oneLine || child.hasTextChild(), depth});
        }
      }
    }
  }

  NodeKind kind_ = NodeKind::Element;
  std::string tag_;
  std::string text_;
  std::vector<Attribute> attributes_;
  std::vector<std::unique_ptr<XmlElement>> children_;
};

/**
 * Parses XML text into an XmlElement tree.
 *
 *     XmlDocument doc(text);
 *     std::unique_ptr<XmlElement> root = doc.getDocumentElement();
 *     if (!root) report(doc.getLastParseError());   // "line 3, column 14: ..."
 *
 * or in one call, `XmlDocument::parse(text, &error)`.
 */
class XmlDocument {
public:
  /** What a document may cost before it is refused. The defaults admit any
   *  plausible preset or project file and refuse a hostile one quickly. */
  struct Limits {
    size_t maxInputBytes = (size_t) 64 << 20; ///< the text itself
    int maxDepth = 256;                       ///< elements open at once
    size_t maxNodes = (size_t) 1 << 20;       ///< elements, text nodes, comments and attributes
  };

  /** Keeps `documentText` to parse; nothing is read until
   *  getDocumentElement(), which may be called more than once. */
  explicit XmlDocument(std::string documentText) : text_(std::move(documentText)) {}

  /**
   * The root element, or nullptr with getLastParseError() saying why.
   * `onlyReadOuterDocumentElement` stops after the root's start tag -- its
   * name and attributes, no children -- which is the cheap way to ask what
   * kind of file this is.
   */
  std::unique_ptr<XmlElement> getDocumentElement(bool onlyReadOuterDocumentElement = false) {
    Parser p(*this, onlyReadOuterDocumentElement);
    std::unique_ptr<XmlElement> root = p.run();
    lastError_ = p.error;
    if (!lastError_.empty()) root.reset();
    return root;
  }

  /** The root, only if its tag is `requiredTag`: the start tag is read first,
   *  and the rest only when it matches. */
  std::unique_ptr<XmlElement> getDocumentElementIfTagMatches(const std::string& requiredTag) {
    std::unique_ptr<XmlElement> outer = getDocumentElement(true);
    if (!outer) return nullptr;
    if (outer->getTagName() != requiredTag) {
      lastError_ = "the root element is <" + outer->getTagName() + ">, not <" + requiredTag + ">";
      return nullptr;
    }
    return getDocumentElement(false);
  }

  /** "" after a successful parse. */
  const std::string& getLastParseError() const { return lastError_; }

  /** Whitespace-only text between elements is dropped by default; false keeps
   *  it as text nodes. Text with anything else in it is always kept whole. */
  void setEmptyTextElementsIgnored(bool shouldBeIgnored) { ignoreEmptyText_ = shouldBeIgnored; }
  /** Comments are read and dropped by default; true keeps them as comment
   *  nodes (isCommentElement). */
  void setCommentsKept(bool shouldBeKept) { keepComments_ = shouldBeKept; }

  /** The limits for this document's next getDocumentElement(). The static
   *  parse() and parseFile() always use the defaults. */
  void setLimits(const Limits& limits) { limits_ = limits; }
  /** The limits the next getDocumentElement() will apply. */
  const Limits& getLimits() const { return limits_; }

  /** Parses with the default settings; nullptr and `errorOut` set on failure. */
  static std::unique_ptr<XmlElement> parse(const std::string& text, std::string* errorOut = nullptr) {
    XmlDocument doc(text);
    std::unique_ptr<XmlElement> root = doc.getDocumentElement();
    if (errorOut) *errorOut = doc.getLastParseError();
    return root;
  }

  /** Reads a file by UTF-8 path and parses it. A file larger than the default
   *  input limit is refused before it is read. */
  static std::unique_ptr<XmlElement> parseFile(const std::string& path,
                                               std::string* errorOut = nullptr) {
    std::string text;
    std::string error;
    if (!readFile(path, Limits().maxInputBytes, text, error)) {
      if (errorOut) *errorOut = error;
      return nullptr;
    }
    return parse(text, errorOut);
  }

private:
  static bool readFile(const std::string& path, size_t limit, std::string& out, std::string& error) {
    std::FILE* f = pathio::open(path.c_str(), "rb");
    if (!f) {
      error = "cannot open " + path;
      return false;
    }
    out.clear();
    char buffer[65536];
    for (;;) {
      const size_t got = std::fread(buffer, 1, sizeof(buffer), f);
      if (got == 0) break;
      if (out.size() + got > limit) {
        std::fclose(f);
        error = "the file is larger than the XML size limit";
        return false;
      }
      out.append(buffer, got);
    }
    const bool failed = std::ferror(f) != 0;
    std::fclose(f);
    if (failed) error = "error reading " + path;
    return !failed;
  }

  // ── The parser ───────────────────────────────────────────────────────────
  //
  // A loop over a stack of open elements rather than a recursive descent:
  // XML's nesting is exactly a stack, and depth then costs heap, not frames.

  /** One getDocumentElement() call's state: the document and its options,
   *  the input (transcoded to UTF-8 first when it is Latin-1), the position,
   *  the nodes made so far and the first error, reported by line and column.
   *  Holds a reference to its XmlDocument, so it lives inside that call and
   *  nowhere else. */
  struct Parser {
    Parser(const XmlDocument& d, bool outerOnly) : doc(d), onlyOuter(outerOnly) {}

    const XmlDocument& doc;
    bool onlyOuter;
    std::string transcoded; // a Latin-1 document, as UTF-8
    const std::string* in = nullptr;
    size_t pos = 0;
    size_t nodes = 0;
    std::string error;

    size_t size() const { return in->size(); }
    unsigned char at(size_t i) const { return i < in->size() ? (unsigned char) (*in)[i] : 0; }
    bool startsWith(const char* literal) const {
      const size_t n = std::strlen(literal);
      return pos + n <= in->size() && in->compare(pos, n, literal) == 0;
    }

    bool fail(const std::string& message) {
      if (!error.empty()) return false;
      // Line and column, the column in characters (not bytes), so the
      // position matches what an editor shows.
      size_t line = 1, column = 1;
      const size_t end = (std::min)(pos, in ? in->size() : (size_t) 0);
      for (size_t i = 0; i < end; ++i) {
        const unsigned char c = (unsigned char) (*in)[i];
        if (c == '\n') {
          ++line;
          column = 1;
        } else if ((c & 0xC0) != 0x80) {
          ++column;
        }
      }
      char where[64];
      std::snprintf(where, sizeof(where), "line %zu, column %zu: ", line, column);
      error = where + message;
      return false;
    }

    bool countNode() {
      if (++nodes > doc.limits_.maxNodes) return fail("the document has more nodes than the limit allows");
      return true;
    }

    void skipSpace() {
      while (pos < size() && XmlElement::isSpace(at(pos))) ++pos;
    }

    std::string readName() {
      const size_t from = pos;
      if (pos < size() && XmlElement::isNameStart(at(pos))) {
        ++pos;
        while (pos < size() && XmlElement::isNameChar(at(pos))) ++pos;
      }
      return in->substr(from, pos - from);
    }

    static void appendUtf8(std::string& out, uint32_t cp) {
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

    // ── Before anything: the bytes ────────────────────────────────────────

    // The XML declaration's pseudo-attribute `name`, or "" if absent.
    static std::string declAttribute(const std::string& decl, const char* name) {
      const std::string key = name;
      size_t from = 0;
      while ((from = decl.find(key, from)) != std::string::npos) {
        const bool boundary = from == 0 || XmlElement::isSpace((unsigned char) decl[from - 1]);
        size_t i = from + key.size();
        while (i < decl.size() && XmlElement::isSpace((unsigned char) decl[i])) ++i;
        if (boundary && i < decl.size() && decl[i] == '=') {
          ++i;
          while (i < decl.size() && XmlElement::isSpace((unsigned char) decl[i])) ++i;
          if (i < decl.size() && (decl[i] == '"' || decl[i] == '\'')) {
            const size_t close = decl.find(decl[i], i + 1);
            if (close != std::string::npos) return decl.substr(i + 1, close - i - 1);
          }
          return std::string();
        }
        from += key.size();
      }
      return std::string();
    }

    // Every byte checked once, up front: well-formed UTF-8, and no character
    // XML forbids (controls other than tab, LF and CR; U+FFFE and U+FFFF).
    bool validateCharacters() {
      const std::string& s = *in;
      for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char) s[i];
        if (c < 0x80) {
          if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') {
            pos = i;
            return fail(c == 0 ? "a NUL byte in the document" : "a control character in the document");
          }
          ++i;
          continue;
        }
        const size_t len = XmlElement::utf8SequenceLength(s, i);
        if (len == 0) {
          pos = i;
          return fail("invalid UTF-8");
        }
        if (len == 3 && c == 0xEF && (unsigned char) s[i + 1] == 0xBF &&
            ((unsigned char) s[i + 2] == 0xBE || (unsigned char) s[i + 2] == 0xBF)) {
          pos = i;
          return fail("U+FFFE and U+FFFF are not XML characters");
        }
        i += len;
      }
      return true;
    }

    bool readPrologBytes() {
      const std::string& raw = doc.text_;
      in = &raw;
      if (raw.size() > doc.limits_.maxInputBytes) return fail("the document is larger than the size limit");
      if (raw.size() >= 2 && (((unsigned char) raw[0] == 0xFE && (unsigned char) raw[1] == 0xFF) ||
                              ((unsigned char) raw[0] == 0xFF && (unsigned char) raw[1] == 0xFE) ||
                              (raw[0] == 0 && raw[1] == '<') || (raw[0] == '<' && raw[1] == 0)))
        return fail("UTF-16 documents are not supported; save it as UTF-8");
      if (raw.size() >= 3 && (unsigned char) raw[0] == 0xEF && (unsigned char) raw[1] == 0xBB &&
          (unsigned char) raw[2] == 0xBF)
        pos = 3;

      // The declaration is ASCII whatever the encoding, so it can be read
      // before the encoding is known -- which is the only way to learn it.
      if (startsWith("<?xml") && XmlElement::isSpace(at(pos + 5))) {
        const size_t end = raw.find("?>", pos);
        if (end == std::string::npos) return fail("unterminated XML declaration");
        const std::string decl = raw.substr(pos + 5, end - pos - 5);
        const std::string version = declAttribute(decl, "version");
        if (version.empty()) return fail("the XML declaration has no version");
        if (version.compare(0, 2, "1.") != 0) return fail("unsupported XML version " + version);
        const std::string encoding = XmlElement::asciiLower(declAttribute(decl, "encoding"));
        const bool latin1 = encoding == "iso-8859-1" || encoding == "latin1" ||
                            encoding == "iso_8859-1" || encoding == "latin-1";
        if (!encoding.empty() && !latin1 && encoding != "utf-8" && encoding != "utf8" &&
            encoding != "us-ascii" && encoding != "ascii")
          return fail("unsupported encoding \"" + declAttribute(decl, "encoding") +
                      "\"; save it as UTF-8");
        if (latin1) {
          // Byte for code point, after the declaration: everything up to it
          // is kept as it is, so positions found so far stay true.
          transcoded.reserve(raw.size() + raw.size() / 8);
          transcoded.assign(raw, 0, end + 2);
          for (size_t i = end + 2; i < raw.size(); ++i) appendUtf8(transcoded, (unsigned char) raw[i]);
          if (transcoded.size() > doc.limits_.maxInputBytes)
            return fail("the document is larger than the size limit");
          in = &transcoded;
        }
        pos = end + 2;
      }
      const size_t resume = pos;
      if (!validateCharacters()) return false;
      pos = resume;
      return true;
    }

    // ── Markup that is read and (mostly) dropped ─────────────────────────

    // "<!--" at pos. Ends at the first "-->"; a "--" before it is tolerated
    // (see the header comment).
    bool readComment(std::string* textOut) {
      const size_t from = pos + 4;
      const size_t end = in->find("-->", from);
      if (end == std::string::npos) return fail("unterminated comment");
      if (textOut) *textOut = normaliseNewlines(in->substr(from, end - from));
      pos = end + 3;
      return true;
    }

    // "<?" at pos.
    bool readProcessingInstruction() {
      const size_t start = pos;
      pos += 2;
      const std::string target = readName();
      if (target.empty()) return fail("a processing instruction needs a target name");
      if (XmlElement::asciiLower(target) == "xml") {
        pos = start;
        return fail("the XML declaration is only allowed at the very start");
      }
      if (!startsWith("?>") && !XmlElement::isSpace(at(pos)))
        return fail("expected whitespace or '?>' after the processing instruction target");
      const size_t end = in->find("?>", pos);
      if (end == std::string::npos) return fail("unterminated processing instruction");
      pos = end + 2;
      return true;
    }

    // "<!DOCTYPE" at pos. Read past -- quotes, the internal subset's brackets,
    // its comments and processing instructions -- and ignored: no entity it
    // declares is ever expanded, and nothing it names is ever fetched.
    bool skipDoctype() {
      pos += 9;
      int brackets = 0;
      while (pos < size()) {
        const unsigned char c = at(pos);
        if (c == '"' || c == '\'') {
          const size_t close = in->find((char) c, pos + 1);
          if (close == std::string::npos) break;
          pos = close + 1;
        } else if (startsWith("<!--")) {
          const size_t end = in->find("-->", pos + 4);
          if (end == std::string::npos) break;
          pos = end + 3;
        } else if (startsWith("<?")) {
          const size_t end = in->find("?>", pos + 2);
          if (end == std::string::npos) break;
          pos = end + 2;
        } else if (c == '[') {
          ++brackets;
          ++pos;
        } else if (c == ']') {
          if (brackets > 0) --brackets;
          ++pos;
        } else if (c == '>' && brackets == 0) {
          ++pos;
          return true;
        } else {
          ++pos;
        }
      }
      return fail("unterminated DOCTYPE");
    }

    // Comments, processing instructions, whitespace -- and a DOCTYPE, once,
    // if `doctypeAllowed` -- until something else.
    bool readMisc(bool doctypeAllowed) {
      bool sawDoctype = false;
      for (;;) {
        skipSpace();
        if (pos >= size()) return true;
        if (startsWith("<!--")) {
          if (!readComment(nullptr)) return false;
        } else if (startsWith("<?")) {
          if (!readProcessingInstruction()) return false;
        } else if (startsWith("<!DOCTYPE")) {
          if (!doctypeAllowed) return fail("a DOCTYPE is only allowed before the root element");
          if (sawDoctype) return fail("more than one DOCTYPE");
          sawDoctype = true;
          if (!skipDoctype()) return false;
        } else {
          return true;
        }
      }
    }

    // ── Character data ───────────────────────────────────────────────────

    static std::string normaliseNewlines(const std::string& s) {
      if (s.find('\r') == std::string::npos) return s;
      std::string out;
      out.reserve(s.size());
      for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\r') {
          out += '\n';
          if (i + 1 < s.size() && s[i + 1] == '\n') ++i;
        } else {
          out += s[i];
        }
      }
      return out;
    }

    // '&' at pos: one of the five predefined entities or a character
    // reference, appended to `out`. Anything else is an error that names it.
    bool readReference(std::string& out) {
      const size_t start = pos;
      ++pos;
      if (at(pos) == '#') {
        ++pos;
        const bool hex = at(pos) == 'x';
        if (hex) ++pos;
        uint32_t value = 0;
        size_t digits = 0;
        bool tooBig = false;
        for (;; ++pos, ++digits) {
          const unsigned char c = at(pos);
          uint32_t d;
          if (c >= '0' && c <= '9') d = (uint32_t) (c - '0');
          else if (hex && c >= 'a' && c <= 'f') d = (uint32_t) (c - 'a' + 10);
          else if (hex && c >= 'A' && c <= 'F') d = (uint32_t) (c - 'A' + 10);
          else break;
          if (!tooBig) {
            value = value * (hex ? 16u : 10u) + d;
            if (value > 0x10FFFF) tooBig = true;
          }
        }
        if (digits == 0 || at(pos) != ';') {
          pos = start;
          return fail("malformed character reference");
        }
        ++pos;
        if (tooBig || value == 0 || (value >= 0xD800 && value <= 0xDFFF) || value == 0xFFFE ||
            value == 0xFFFF) {
          pos = start;
          return fail("a character reference to something that is not a character");
        }
        appendUtf8(out, value);
        return true;
      }
      const size_t nameFrom = pos;
      while (pos < size() && pos - nameFrom < 32 && XmlElement::isNameChar(at(pos))) ++pos;
      const std::string name = in->substr(nameFrom, pos - nameFrom);
      if (name.empty() || at(pos) != ';') {
        pos = start;
        return fail("'&' must be written as &amp;");
      }
      ++pos;
      if (name == "lt") out += '<';
      else if (name == "gt") out += '>';
      else if (name == "amp") out += '&';
      else if (name == "quot") out += '"';
      else if (name == "apos") out += '\'';
      else {
        pos = start;
        return fail("undefined entity &" + name + "; -- entities declared in a DTD are not expanded");
      }
      return true;
    }

    // ── Elements ──────────────────────────────────────────────────────────

    // '<' at pos, a name after it. The element with its attributes; `empty`
    // says whether it closed itself.
    std::unique_ptr<XmlElement> readStartTag(bool& empty) {
      ++pos;
      std::unique_ptr<XmlElement> e(new XmlElement(readName()));
      if (e->tag_.empty()) {
        fail("expected an element name after '<'");
        return nullptr;
      }
      if (!countNode()) return nullptr;
      for (;;) {
        const size_t before = pos;
        skipSpace();
        if (pos >= size()) {
          fail("unterminated start tag <" + e->tag_ + ">");
          return nullptr;
        }
        if (at(pos) == '>') {
          ++pos;
          empty = false;
          break;
        }
        if (at(pos) == '/') {
          if (at(pos + 1) != '>') {
            ++pos;
            fail("expected '>' after '/'");
            return nullptr;
          }
          pos += 2;
          empty = true;
          break;
        }
        if (pos == before) {
          fail("expected whitespace, '>' or '/>' in <" + e->tag_ + ">");
          return nullptr;
        }
        XmlElement::Attribute a;
        a.name = readName();
        if (a.name.empty()) {
          fail("expected an attribute name in <" + e->tag_ + ">");
          return nullptr;
        }
        skipSpace();
        if (at(pos) != '=') {
          fail("expected '=' after attribute " + a.name);
          return nullptr;
        }
        ++pos;
        skipSpace();
        const unsigned char quote = at(pos);
        if (quote != '"' && quote != '\'') {
          fail("expected a quoted value for attribute " + a.name);
          return nullptr;
        }
        ++pos;
        for (;;) {
          if (pos >= size()) {
            fail("unterminated value for attribute " + a.name);
            return nullptr;
          }
          const unsigned char c = at(pos);
          if (c == quote) {
            ++pos;
            break;
          }
          if (c == '<') {
            fail("'<' is not allowed in an attribute value");
            return nullptr;
          }
          if (c == '&') {
            if (!readReference(a.value)) return nullptr;
            continue;
          }
          // Attribute-value normalisation: a literal tab or newline is a
          // space (a CR LF pair is one); a character reference is kept as
          // written, which is how the writer preserves them.
          if (c == '\r') {
            a.value += ' ';
            ++pos;
            if (at(pos) == '\n') ++pos;
            continue;
          }
          a.value += (c == '\t' || c == '\n') ? ' ' : (char) c;
          ++pos;
        }
        if (!countNode()) return nullptr;
        e->attributes_.push_back(std::move(a));
      }
      if (!checkDuplicateAttributes(*e)) return nullptr;
      return e;
    }

    bool checkDuplicateAttributes(const XmlElement& e) {
      const auto& attrs = e.attributes_;
      if (attrs.size() < 2) return true;
      if (attrs.size() <= 8) {
        for (size_t i = 0; i < attrs.size(); ++i)
          for (size_t j = i + 1; j < attrs.size(); ++j)
            if (attrs[i].name == attrs[j].name)
              return fail("duplicate attribute " + attrs[i].name + " in <" + e.tag_ + ">");
        return true;
      }
      std::vector<const std::string*> names;
      names.reserve(attrs.size());
      for (const auto& a : attrs) names.push_back(&a.name);
      std::sort(names.begin(), names.end(),
                [](const std::string* x, const std::string* y) { return *x < *y; });
      for (size_t i = 1; i < names.size(); ++i)
        if (*names[i] == *names[i - 1])
          return fail("duplicate attribute " + *names[i] + " in <" + e.tag_ + ">");
      return true;
    }

    // Whitespace that came from a CDATA section is whitespace like any other:
    // the writer cannot know where text came from, and a document must read
    // back as the tree it was written from.
    bool flushText(XmlElement& parent, std::string& text) {
      if (text.empty()) return true;
      bool keep = !doc.ignoreEmptyText_;
      for (size_t i = 0; !keep && i < text.size(); ++i)
        if (!XmlElement::isSpace((unsigned char) text[i])) keep = true;
      if (keep) {
        if (!countNode()) return false;
        parent.children_.push_back(XmlElement::createTextElement(text));
      }
      text.clear();
      return true;
    }

    std::unique_ptr<XmlElement> run() {
      if (!readPrologBytes()) return nullptr;
      if (!readMisc(true)) return nullptr;
      if (pos >= size()) {
        fail("no root element");
        return nullptr;
      }
      if (at(pos) != '<' || !XmlElement::isNameStart(at(pos + 1))) {
        fail("expected the root element");
        return nullptr;
      }
      bool empty = false;
      std::unique_ptr<XmlElement> root = readStartTag(empty);
      if (!root || onlyOuter) return root;

      std::vector<XmlElement*> open;
      if (!empty) open.push_back(root.get());
      std::string text;
      while (!open.empty()) {
        if (pos >= size()) {
          fail("unclosed element <" + open.back()->tag_ + ">");
          return nullptr;
        }
        const unsigned char c = at(pos);
        if (c == '&') {
          if (!readReference(text)) return nullptr;
          continue;
        }
        if (c != '<') {
          // A run of character data, up to the next markup or reference.
          size_t end = pos;
          while (end < size() && at(end) != '<' && at(end) != '&') ++end;
          text += normaliseNewlines(in->substr(pos, end - pos));
          pos = end;
          continue;
        }
        XmlElement& parent = *open.back();
        if (at(pos + 1) == '/') {
          if (!flushText(parent, text)) return nullptr;
          const size_t tagAt = pos;
          pos += 2;
          const std::string name = readName();
          skipSpace();
          if (at(pos) != '>') {
            fail("expected '>' to end </" + name + ">");
            return nullptr;
          }
          if (name != parent.tag_) {
            pos = tagAt;
            fail("</" + name + "> does not close <" + parent.tag_ + ">");
            return nullptr;
          }
          ++pos;
          open.pop_back();
          continue;
        }
        if (startsWith("<!--")) {
          if (doc.keepComments_) {
            std::string comment;
            if (!flushText(parent, text) || !readComment(&comment) || !countNode())
              return nullptr;
            parent.children_.push_back(XmlElement::createCommentElement(comment));
          } else if (!readComment(nullptr)) {
            // A dropped comment does not split the text around it.
            return nullptr;
          }
          continue;
        }
        if (startsWith("<![CDATA[")) {
          const size_t from = pos + 9;
          const size_t end = in->find("]]>", from);
          if (end == std::string::npos) {
            fail("unterminated CDATA section");
            return nullptr;
          }
          text += normaliseNewlines(in->substr(from, end - from));
          pos = end + 3;
          continue;
        }
        if (startsWith("<?")) {
          if (!readProcessingInstruction()) return nullptr;
          continue;
        }
        if (startsWith("<!")) {
          fail(startsWith("<!DOCTYPE") ? "a DOCTYPE is only allowed before the root element"
                                       : "unexpected '<!' inside an element");
          return nullptr;
        }
        if (!flushText(parent, text)) return nullptr;
        if ((int) open.size() >= doc.limits_.maxDepth) {
          fail("elements are nested more deeply than the limit allows");
          return nullptr;
        }
        std::unique_ptr<XmlElement> child = readStartTag(empty);
        if (!child) return nullptr;
        XmlElement* raw = child.get();
        parent.children_.push_back(std::move(child));
        if (!empty) open.push_back(raw);
      }

      if (!readMisc(false)) return nullptr;
      if (pos < size()) {
        fail("content after the root element");
        return nullptr;
      }
      return root;
    }
  };

  std::string text_;
  std::string lastError_;
  bool ignoreEmptyText_ = true;
  bool keepComments_ = false;
  Limits limits_;
};

} // namespace sonore
