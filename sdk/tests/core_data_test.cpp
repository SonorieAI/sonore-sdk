// SPDX-License-Identifier: Apache-2.0
// The data utilities: XML, translations, text diff, expressions, running
// statistics, the leak detector and the unit-test framework.
//
// Real behaviour with real numbers: documents parsed into known trees and
// written back byte for byte, hostile documents refused with the error that
// names the problem (XXE and billion laughs among them), translation files
// read and looked up under a thread that keeps swapping the language, diffs
// applied over thousands of random strings and checked for MINIMALITY against
// a dynamic-programming LCS, expressions checked against the same formula
// compiled as C++, statistics against a two-pass computation, the leak
// detector against a child process that really leaks, and the unit-test
// framework against sample tests that pass, fail and throw.
//
// On Windows <windows.h> comes first, without NOMINMAX, so this file is also
// the proof that every one of these headers survives its min and max macros.
//
//   core_data_test            the whole suite
//   core_data_test --leak-child   (internal: the process that leaks on purpose)
#if defined(_WIN32)
#include <windows.h>
#endif

// On in every build of this test, so the detector is exercised in release too.
#define SONORE_CHECK_MEMORY_LEAKS 1

#include <sonore/expression.h>
#include <sonore/leak_detector.h>
#include <sonore/localisation.h>
#include <sonore/statistics.h>
#include <sonore/text_diff.h>
#include <sonore/unit_test.h>
#include <sonore/xml.h>

#include <atomic>
#include <chrono>
#include <clocale>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "temp_path.h"

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}
void check(bool ok, const std::string& what) { check(ok, what.c_str()); }

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

double seconds(std::chrono::steady_clock::time_point from) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count();
}

// A small deterministic generator for the random cases (splitmix64).
struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed) {}
  uint64_t next() {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  int below(int n) { return (int) (next() % (uint64_t) n); }
  double uniform() { return (double) (next() >> 11) * (1.0 / 9007199254740992.0); }
};

void appendUtf8(std::string& out, uint32_t cp) {
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

// Text a writer has to escape: markup characters, quotes, tabs and newlines,
// a CR, control characters, and characters of one to four UTF-8 bytes.
std::string randomXmlText(Rng& r, int maxLength) {
  static const uint32_t pool[] = {'a', 'b', 'Z', '0', ' ', '&', '<', '>', '"', '\'', '\t', '\n', '\r',
                                  0x01, 0x1F, 0x7F, 0xE9, 0x416, 0x20AC, 0x1F600, ']', '-', ';', '#'};
  std::string s;
  const int n = 1 + r.below(maxLength);
  for (int i = 0; i < n; ++i) appendUtf8(s, pool[r.below((int) (sizeof(pool) / sizeof(pool[0])))]);
  return s;
}

bool isAllXmlSpace(const std::string& s) {
  for (char c : s)
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return false;
  return true;
}

std::string randomName(Rng& r) {
  static const char* names[] = {"a", "item", "ns:tag", "x-y", "_z", "Param.1", "\xC3\xA9l\xC3\xA9ment"};
  return names[r.below(7)];
}

// A random tree a write-then-parse must reproduce exactly: no empty or
// whitespace-only text (dropped by default when read), no two text nodes side
// by side (they are one text when read).
void growRandomTree(Rng& r, sonore::XmlElement& e, int depth, bool comments) {
  const int attrs = r.below(4);
  for (int i = 0; i < attrs; ++i) e.setAttribute("at" + std::to_string(i), randomXmlText(r, 12));
  const int kids = depth > 4 ? 0 : r.below(5);
  bool lastWasText = false;
  for (int i = 0; i < kids; ++i) {
    const int kind = r.below(comments ? 4 : 3);
    if (kind == 0 && !lastWasText) {
      std::string t = randomXmlText(r, 16);
      if (isAllXmlSpace(t)) t += "x";
      e.addTextElement(t);
      lastWasText = true;
    } else if (kind == 3) {
      std::string c = "note " + std::to_string(r.below(100));
      e.addChildElement(sonore::XmlElement::createCommentElement(c));
      lastWasText = false;
    } else {
      growRandomTree(r, *e.createNewChildElement(randomName(r)), depth + 1, comments);
      lastWasText = false;
    }
  }
}

// ── XML ────────────────────────────────────────────────────────────────────

void testXmlKnownDocument() {
  std::printf("\nXML: a known document, every construct\n");
  const std::string doc =
      "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
      "<!-- prolog comment -->\r\n"
      "<?app-instruction data here?>\r\n"
      "<!DOCTYPE preset SYSTEM \"http://example.invalid/preset.dtd\" [\r\n"
      "  <!ENTITY brand \"Sonore > all\">\r\n"
      "  <!-- ] a bracket in a comment -->\r\n"
      "  <!ATTLIST preset version CDATA '1'>\r\n"
      "]>\r\n"
      "<preset version='2' name=\"Warm &amp; &quot;wide&quot;\" tab=\"a\tb\" nl=\"a\r\nb\" ref=\"a&#10;b\">\r\n"
      "  <param id=\"gain\" value=\"-6.5\"/>\r\n"
      "  <param id=\"mix\" value=\"0.25\" enabled=\"yes\" />\r\n"
      "  <notes>Line one\r\nline &lt;two&gt; &#x20AC;&#8364;&#x1F600; <![CDATA[<raw> & ]] stays]]> end</notes>\r\n"
      "  <empty></empty>\r\n"
      "  <mixed>one <b>two</b> three<!-- dropped --> four<?pi dropped?></mixed>\r\n"
      "</preset>\r\n"
      "<!-- epilog -->\r\n";
  sonore::XmlDocument d(doc);
  std::unique_ptr<sonore::XmlElement> root = d.getDocumentElement();
  check(root != nullptr && d.getLastParseError().empty(),
        "parses: BOM, declaration, prolog comment and PI, DOCTYPE with an internal subset, CRLF");
  if (!root) {
    std::printf("       error: %s\n", d.getLastParseError().c_str());
    return;
  }
  check(root->getTagName() == "preset" && root->getNumAttributes() == 5, "root <preset> with 5 attributes");
  check(root->getIntAttribute("version") == 2, "single-quoted attribute, read as an int");
  check(root->getStringAttribute("name") == "Warm & \"wide\"", "&amp; and &quot; in an attribute");
  check(root->getStringAttribute("tab") == "a b" && root->getStringAttribute("nl") == "a b",
        "a literal tab and a CR LF in an attribute value normalise to one space each");
  check(root->getStringAttribute("ref") == "a\nb", "&#10; in an attribute survives as a newline");
  check(root->getNumChildElements() == 5, "five children, whitespace-only text between them dropped");
  sonore::XmlElement* gain = root->getChildByAttribute("id", "gain");
  check(gain && gain->getDoubleAttribute("value") == -6.5, "getChildByAttribute finds gain = -6.5");
  sonore::XmlElement* mix = root->getChildByAttribute("id", "mix");
  check(mix && mix->getDoubleAttribute("value") == 0.25 && mix->getBoolAttribute("enabled"),
        "mix = 0.25, enabled=\"yes\" is true");
  const sonore::XmlElement* notes = root->getChildByName("notes");
  const std::string expectedNotes =
      "Line one\nline <two> \xE2\x82\xAC\xE2\x82\xAC\xF0\x9F\x98\x80 <raw> & ]] stays end";
  check(notes && notes->getAllSubText() == expectedNotes,
        "text: CR LF to LF, entities, hex/decimal/astral character references, CDATA kept raw");
  check(notes && notes->getNumChildElements() == 1 && notes->getChildElement(0)->isTextElement(),
        "character data, references and CDATA side by side are ONE text node");
  const sonore::XmlElement* empty = root->getChildByName("empty");
  check(empty && empty->getNumChildElements() == 0, "<empty></empty> has no children");
  const sonore::XmlElement* mixed = root->getChildByName("mixed");
  check(mixed && mixed->getNumChildElements() == 3 && mixed->getAllSubText() == "one two three four",
        "mixed content keeps its order; a dropped comment and PI do not split the text around them");
  check(root->getChildElementAllSubText("notes", "?") == expectedNotes &&
            root->getChildElementAllSubText("missing", "fallback") == "fallback",
        "getChildElementAllSubText, with its default");

  // The same document with comments kept.
  sonore::XmlDocument d2(doc);
  d2.setCommentsKept(true);
  std::unique_ptr<sonore::XmlElement> withComments = d2.getDocumentElement();
  const sonore::XmlElement* mixed2 = withComments ? withComments->getChildByName("mixed") : nullptr;
  check(mixed2 && mixed2->getNumChildElements() == 5 && mixed2->getChildElement(3)->isCommentElement() &&
            mixed2->getChildElement(3)->getText() == " dropped ",
        "setCommentsKept(true): the comment is a node of its own, and splits the text");

  // Whitespace kept.
  sonore::XmlDocument d3("<a> <b/> </a>");
  d3.setEmptyTextElementsIgnored(false);
  std::unique_ptr<sonore::XmlElement> ws = d3.getDocumentElement();
  check(ws && ws->getNumChildElements() == 3 && ws->getChildElement(0)->getText() == " ",
        "setEmptyTextElementsIgnored(false) keeps whitespace text");

  // Only the outer element.
  sonore::XmlDocument d4(doc);
  std::unique_ptr<sonore::XmlElement> outer = d4.getDocumentElement(true);
  check(outer && outer->getTagName() == "preset" && outer->getNumChildElements() == 0 &&
            outer->getNumAttributes() == 5,
        "getDocumentElement(true): the root's name and attributes, no children");
  check(d4.getDocumentElementIfTagMatches("preset") != nullptr &&
            d4.getDocumentElementIfTagMatches("patch") == nullptr &&
            contains(d4.getLastParseError(), "<preset>"),
        "getDocumentElementIfTagMatches accepts <preset>, refuses when asked for <patch>");

  // Latin-1, transcoded.
  std::unique_ptr<sonore::XmlElement> latin =
      sonore::XmlDocument::parse("<?xml version='1.0' encoding='ISO-8859-1'?><n v='caf\xE9'>\xE0 la</n>");
  check(latin && latin->getStringAttribute("v") == "caf\xC3\xA9" &&
            latin->getAllSubText() == "\xC3\xA0 la",
        "a document declared ISO-8859-1 is transcoded: \\xE9 reads as U+00E9");
}

void testXmlWriting() {
  std::printf("\nXML: writing, byte for byte\n");
  sonore::XmlElement root("preset");
  root.setAttribute("name", "A & B <\"quoted\">");
  root.setAttribute("gain", -6.5);
  root.setAttribute("tenth", 0.1);
  root.setAttribute("count", (size_t) 3);
  root.setAttribute("on", true);
  root.setAttribute("ws", "t\tn\nr\r");
  sonore::XmlElement* p = root.createNewChildElement("param");
  p->setAttribute("id", "drive");
  root.createNewChildElement("notes")->addTextElement("x < y & z > \"w\"");
  sonore::XmlElement* mixed = root.createNewChildElement("p");
  mixed->addTextElement("one ");
  mixed->createNewChildElement("b")->createNewChildElement("i")->addTextElement("two");
  mixed->addTextElement(" three");
  root.addChildElement(sonore::XmlElement::createCommentElement("a -- b -"));

  const std::string pretty = root.toString();
  const std::string expectedPretty =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<preset name=\"A &amp; B &lt;&quot;quoted&quot;&gt;\"\n"
      "        gain=\"-6.5\" tenth=\"0.1\" count=\"3\" on=\"true\"\n"
      "        ws=\"t&#9;n&#10;r&#13;\">\n"
      "  <param id=\"drive\"/>\n"
      "  <notes>x &lt; y &amp; z &gt; \"w\"</notes>\n"
      "  <p>one <b><i>two</i></b> three</p>\n"
      "  <!--a - - b - -->\n"
      "</preset>\n";
  check(pretty == expectedPretty,
        "pretty: declaration, escapes, attributes wrapped at 60 columns, mixed content on one line, "
        "\"--\" in a comment split");
  if (pretty != expectedPretty) std::printf("----\n%s----\n", pretty.c_str());

  sonore::XmlElement::TextFormat f;
  const std::string single = root.toString(f.singleLine().withoutHeader());
  const std::string expectedSingle =
      "<preset name=\"A &amp; B &lt;&quot;quoted&quot;&gt;\" gain=\"-6.5\" tenth=\"0.1\" count=\"3\" "
      "on=\"true\" ws=\"t&#9;n&#10;r&#13;\"><param id=\"drive\"/><notes>x &lt; y &amp; z &gt; "
      "\"w\"</notes><p>one <b><i>two</i></b> three</p><!--a - - b - --></preset>";
  check(single == expectedSingle, "singleLine().withoutHeader(): one line, nothing added");
  if (single != expectedSingle) std::printf("----\n%s\n----\n", single.c_str());

  std::unique_ptr<sonore::XmlElement> back = sonore::XmlDocument::parse(pretty);
  check(back && back->getStringAttribute("ws") == "t\tn\nr\r" && back->getDoubleAttribute("tenth") == 0.1,
        "read back: tab, newline and CR in an attribute survive; 0.1 is exactly 0.1");

  sonore::XmlElement::TextFormat custom;
  custom.dtd = "<!DOCTYPE preset>";
  custom.customEncoding = "us-ascii";
  custom.indentSize = 4;
  custom.lineWrapLength = 0;
  sonore::XmlElement tiny("a");
  tiny.createNewChildElement("b");
  check(tiny.toString(custom) == "<?xml version=\"1.0\" encoding=\"us-ascii\"?>\n<!DOCTYPE preset>\n<a>\n    <b/>\n</a>\n",
        "TextFormat: a DTD line, a custom encoding name, indent of 4");

  // Invalid UTF-8 and NUL in a string: the output is still a document.
  sonore::XmlElement bad("bad");
  bad.setAttribute("v", std::string("ok\xFF\xC3", 4) + std::string(1, '\0') + "end");
  const std::string badText = bad.toString();
  std::unique_ptr<sonore::XmlElement> badBack = sonore::XmlDocument::parse(badText);
  check(badBack && badBack->getStringAttribute("v") == "ok\xEF\xBF\xBD\xEF\xBF\xBD" "end",
        "invalid UTF-8 is written as U+FFFD and NUL dropped: the output always parses");

  // Doubles: shortest exact text, '.' whatever the locale.
  sonore::XmlElement nums("n");
  const double values[] = {0.1, 1.0 / 3.0, -2.5e-300, 6.02214076e23, 123456789.0, -0.0};
  bool exact = true;
  for (double v : values) {
    nums.setAttribute("v", v);
    exact = exact && nums.getDoubleAttribute("v", 99.0) == v;
  }
  nums.setAttribute("v", 0.1);
  check(exact && nums.getStringAttribute("v") == "0.1", "doubles read back exactly, 0.1 written as \"0.1\"");
  nums.setAttribute("v", std::numeric_limits<double>::infinity());
  const bool inf = nums.getStringAttribute("v") == "inf" && std::isinf(nums.getDoubleAttribute("v"));
  nums.setAttribute("v", std::nan(""));
  check(inf && std::isnan(nums.getDoubleAttribute("v", 0.0)), "inf and nan written and read back");

  // A nested child (not the root) with several attributes still wraps and
  // aligns under its own '<', so skipping the column scan for small elements
  // did not break the elements that do wrap.
  sonore::XmlElement wrapRoot("root");
  sonore::XmlElement* wrapChild = wrapRoot.createNewChildElement("child");
  wrapChild->setAttribute("alpha", "one");
  wrapChild->setAttribute("bravo", "two");
  wrapChild->setAttribute("charlie", "three");
  wrapChild->setAttribute("delta", "four");
  const std::string wrapped = wrapRoot.toString(sonore::XmlElement::TextFormat().withoutHeader());
  check(wrapped ==
            "<root>\n"
            "  <child alpha=\"one\" bravo=\"two\" charlie=\"three\"\n"
            "         delta=\"four\"/>\n"
            "</root>\n",
        "a nested child's attributes wrap aligned under its own '<'");
  if (wrapped != "<root>\n  <child alpha=\"one\" bravo=\"two\" charlie=\"three\"\n         delta=\"four\"/>\n</root>\n")
    std::printf("----\n%s----\n", wrapped.c_str());

  // Writing a flat document is linear, not quadratic: finding the current
  // column by scanning the whole output on every child made a wide element or
  // a single-line serialisation O(n^2). 200k children must write in well under
  // a second; before the fix this took many seconds.
  {
    sonore::XmlElement wide("list");
    for (int i = 0; i < 200000; ++i) wide.createNewChildElement("x");
    sonore::XmlElement mixedWide("p");
    for (int i = 0; i < 200000; ++i) {
      mixedWide.addTextElement("t");
      mixedWide.createNewChildElement("x");
    }
    const auto t0 = std::chrono::steady_clock::now();
    const std::string flat = wide.toString(sonore::XmlElement::TextFormat().singleLine());
    const std::string pretty2 = wide.toString();
    const std::string mixed2 = mixedWide.toString(); // mixed content: one long line
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    check(flat.size() > 700000 && pretty2.size() > 1000000 && mixed2.size() > 900000 && took < 3.0,
          "a 200k-child document serialises in linear time (single line, pretty and mixed)");
    if (took >= 3.0) std::printf("       serialising 200k children took %.2f s\n", took);
  }

  // Move-assigning a node from one of its own descendants must not read the
  // source after this node's children (which hold it) are freed.
  {
    std::unique_ptr<sonore::XmlElement> tree = sonore::XmlDocument::parse("<a><b k=\"1\"><c/></b></a>");
    sonore::XmlElement* b = tree->getChildElement(0);
    *tree = std::move(*b);
    check(tree->getTagName() == "b" && tree->getStringAttribute("k") == "1" &&
              tree->getNumChildElements() == 1 && tree->getChildElement(0)->getTagName() == "c",
          "move-assigning a node from its own child keeps the child's contents, no use-after-free");
  }
}

void testXmlTypedGetters() {
  std::printf("\nXML: typed getters, whole-value, saturating\n");
  sonore::XmlElement e("e");
  e.setAttribute("i", " 42 ");
  e.setAttribute("neg", "-17");
  e.setAttribute("big", "99999999999999999999");
  e.setAttribute("small", "-99999999999999999999");
  e.setAttribute("frac", "2.7");
  e.setAttribute("exp", "1e3");
  e.setAttribute("junk", "12px");
  e.setAttribute("huge", "1e300");
  check(e.getIntAttribute("i") == 42 && e.getIntAttribute("neg") == -17, "\" 42 \" is 42, \"-17\" is -17");
  check(e.getIntAttribute("big") == 2147483647 && e.getIntAttribute("small") == (-2147483647 - 1) &&
            e.getIntAttribute("huge") == 2147483647,
        "past int's range saturates rather than wrapping");
  check(e.getIntAttribute("frac") == 2 && e.getIntAttribute("exp") == 1000, "2.7 truncates to 2, 1e3 is 1000");
  check(e.getIntAttribute("junk", -1) == -1 && e.getIntAttribute("missing", 7) == 7,
        "\"12px\" is not a number: the default, not 12");
  check(e.getDoubleAttribute("junk", -1.0) == -1.0, "getDoubleAttribute refuses \"12px\" too");
  const char* trues[] = {"true", "YES", "on", "y", "1", "2.5"};
  const char* falses[] = {"false", "No", "OFF", "n", "0", "0.0"};
  bool all = true;
  for (const char* t : trues) {
    e.setAttribute("b", t);
    all = all && e.getBoolAttribute("b", false);
  }
  for (const char* t : falses) {
    e.setAttribute("b", t);
    all = all && !e.getBoolAttribute("b", true);
  }
  e.setAttribute("b", "maybe");
  check(all && e.getBoolAttribute("b", true) && !e.getBoolAttribute("b", false),
        "bools: true/yes/on/y/non-zero, false/no/off/n/zero, anything else the default");
  check(e.compareAttribute("neg", "-17") && !e.compareAttribute("i", "42") &&
            e.compareAttribute("b", "MAYBE", true),
        "compareAttribute, exact and case-folded");
  e.setAttribute("i", 5);
  check(e.getNumAttributes() == 9 && e.getAttributeName(0) == "i" && e.getAttributeValue(0) == "5",
        "setAttribute on an existing name replaces it in place");
  e.removeAttribute("i");
  check(!e.hasAttribute("i") && e.getNumAttributes() == 8 && e.getAttributeName(0) == "neg",
        "removeAttribute");

  // A comma locale, as a German or Spanish host sets: '.' must still be the point.
  const char* commaLocales[] = {"de_DE.UTF-8", "de_DE.utf8", "de_DE", "German_Germany.1252", "German", "fr_FR.UTF-8"};
  const char* chosen = nullptr;
  for (const char* name : commaLocales)
    if (std::setlocale(LC_NUMERIC, name)) {
      chosen = name;
      break;
    }
  if (chosen) {
    sonore::XmlElement n("n");
    n.setAttribute("v", 0.5);
    std::unique_ptr<sonore::XmlElement> back = sonore::XmlDocument::parse("<n v=\"2.25\"/>");
    check(n.getStringAttribute("v") == "0.5" && n.getDoubleAttribute("v") == 0.5 && back &&
              back->getDoubleAttribute("v") == 2.25,
          std::string("under the comma locale ") + chosen + ", 0.5 is written \"0.5\" and \"2.25\" read as 2.25");
    std::setlocale(LC_NUMERIC, "C");
  } else {
    std::printf("  --   no comma locale installed; the locale check is skipped\n");
  }
}

void testXmlChildren() {
  std::printf("\nXML: children, iteration, copies\n");
  sonore::XmlElement root("root");
  sonore::XmlElement* a = root.createNewChildElement("item");
  a->setAttribute("id", "a");
  root.addTextElement("text");
  sonore::XmlElement* b = root.addChildElement(std::unique_ptr<sonore::XmlElement>(new sonore::XmlElement("item")));
  b->setAttribute("id", "b");
  sonore::XmlElement* first = root.prependChildElement(std::unique_ptr<sonore::XmlElement>(new sonore::XmlElement("first")));
  sonore::XmlElement* mid = root.insertChildElement(std::unique_ptr<sonore::XmlElement>(new sonore::XmlElement("other")), 2);
  check(root.getNumChildElements() == 5 && root.getFirstChildElement() == first && root.getChildElement(2) == mid &&
            root.getIndexOfChildElement(b) == 4,
        "create, add, prepend and insert put children where asked");
  int all = 0, items = 0;
  for (sonore::XmlElement* c : root.getChildIterator()) all += c ? 1 : 0;
  for (sonore::XmlElement* c : root.getChildWithTagNameIterator("item")) items += c->hasTagName("item") ? 1 : 0;
  check(all == 5 && items == 2, "getChildIterator sees all five; getChildWithTagNameIterator(\"item\") two");
  check(root.getChildByName("item") == a && root.getChildByAttribute("id", "b") == b &&
            root.getChildByName("text") == nullptr,
        "getChildByName / getChildByAttribute (text nodes never match)");

  sonore::XmlElement* deep = b->createNewChildElement("deep")->createNewChildElement("deeper");
  check(root.findParentElementOf(deep) == b->getChildByName("deep") && root.findParentElementOf(b) == &root &&
            root.findParentElementOf(&root) == nullptr,
        "findParentElementOf searches the whole tree");

  sonore::XmlElement copy(root);
  check(copy.isEquivalentTo(&root, false), "a copy is equivalent");
  copy.getChildByName("item")->setAttribute("id", "changed");
  check(root.getChildByName("item")->getStringAttribute("id") == "a" && !copy.isEquivalentTo(&root, false),
        "and deep: changing the copy leaves the original alone");

  sonore::XmlElement reordered("e"), plain("e");
  plain.setAttribute("x", "1");
  plain.setAttribute("y", "2");
  reordered.setAttribute("y", "2");
  reordered.setAttribute("x", "1");
  check(!plain.isEquivalentTo(&reordered, false) && plain.isEquivalentTo(&reordered, true),
        "isEquivalentTo: attribute order matters unless told otherwise");

  // Ignoring order on a wide element sorts once rather than looking each
  // attribute up in the other: 80,000 attributes took 19 s.
  {
    // Built by parsing (which sorts to find repeats), not setAttribute,
    // which looks each name up and would make building them quadratic.
    const int n = 50000;
    auto wide = [=](bool reversed, int changedValue, bool changedName) {
      std::string text = "<w";
      for (int k = 0; k < n; ++k) {
        const int i = reversed ? n - 1 - k : k;
        text += (changedName && i == 777) ? std::string(" other") : " k" + std::to_string(i);
        text += "=\"" + std::to_string(i == changedValue ? -1 : i) + "\"";
      }
      return sonore::XmlDocument::parse(text + "/>");
    };
    const auto wideA = wide(false, -1, false), wideB = wide(true, -1, false);
    const auto wideC = wide(true, 777, false), wideD = wide(true, -1, true);
    const auto t0 = std::chrono::steady_clock::now();
    const bool same = wideA->isEquivalentTo(wideB.get(), true);
    const bool valueDiffers = !wideA->isEquivalentTo(wideC.get(), true);
    const bool nameDiffers = !wideA->isEquivalentTo(wideD.get(), true);
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    check(same && valueDiffers && nameDiffers && !wideA->isEquivalentTo(wideB.get(), false) && took < 2.0,
          "isEquivalentTo ignoring order: 50,000 attributes compared in n log n, a changed value or name found");
  }

  std::unique_ptr<sonore::XmlElement> removed = root.removeChildElement(mid);
  check(removed && removed->getTagName() == "other" && root.getNumChildElements() == 4 &&
            root.removeChildElement(deep) == nullptr,
        "removeChildElement hands back a direct child, refuses a grandchild");
  check(root.replaceChildElement(a, sonore::XmlElement::createTextElement("swapped")) &&
            root.getChildElement(1)->getText() == "swapped",
        "replaceChildElement");
  root.deleteAllTextElements();
  check(root.getNumChildElements() == 2 && root.getAllSubText().empty(), "deleteAllTextElements");
  root.deleteAllChildElementsWithTagName("item");
  check(root.getNumChildElements() == 1 && root.getFirstChildElement()->getTagName() == "first",
        "deleteAllChildElementsWithTagName");
  root.setText("replaced");
  check(root.getNumChildElements() == 1 && root.getAllSubText() == "replaced",
        "setText on an element replaces its children with one text node");
  // ...from text one of those children holds: it was read after the children
  // were deleted (a use after free under ASan, garbage text without).
  {
    const std::string longText(200, 't');
    sonore::XmlElement e("e");
    e.addTextElement(longText);
    e.createNewChildElement("x")->setAttribute("v", longText + "v");
    e.setText(e.getChildElement(0)->getText());
    const bool fromText = e.getNumChildElements() == 1 && e.getAllSubText() == longText;
    e.createNewChildElement("x")->setAttribute("v", longText + "v");
    e.setText(e.getChildElement(1)->getStringAttribute("v"));
    check(fromText && e.getNumChildElements() == 1 && e.getAllSubText() == longText + "v",
          "setText from a child's own text or attribute, which it deletes");
  }
  sonore::XmlElement ns("svg:rect");
  check(ns.getTagNameWithoutNamespace() == "rect" && ns.getNamespace() == "svg" &&
            ns.hasTagNameIgnoringNamespace("rect"),
        "namespace prefix helpers");
  check(sonore::XmlElement::isValidXmlName("a-b.c_d:e") && !sonore::XmlElement::isValidXmlName("1a") &&
            !sonore::XmlElement::isValidXmlName("a b") && !sonore::XmlElement::isValidXmlName(""),
        "isValidXmlName");
}

void testXmlRoundTrips() {
  std::printf("\nXML: random trees written and read back\n");
  Rng r(0xC0FFEE);
  int pretty = 0, single = 0, withComments = 0;
  const int trees = 1500;
  for (int i = 0; i < trees; ++i) {
    sonore::XmlElement root(randomName(r));
    growRandomTree(r, root, 0, false);
    std::unique_ptr<sonore::XmlElement> a = sonore::XmlDocument::parse(root.toString());
    if (a && a->isEquivalentTo(&root, false)) ++pretty;
    sonore::XmlElement::TextFormat f;
    std::unique_ptr<sonore::XmlElement> b = sonore::XmlDocument::parse(root.toString(f.singleLine()));
    if (b && b->isEquivalentTo(&root, false)) ++single;
    sonore::XmlElement commented(randomName(r));
    growRandomTree(r, commented, 0, true);
    sonore::XmlDocument d(commented.toString());
    d.setCommentsKept(true);
    std::unique_ptr<sonore::XmlElement> c = d.getDocumentElement();
    if (c && c->isEquivalentTo(&commented, false)) ++withComments;
  }
  check(pretty == trees, std::to_string(pretty) + "/" + std::to_string(trees) +
                             " pretty-printed trees read back equivalent (markup, quotes, CR, controls, astral)");
  check(single == trees, std::to_string(single) + "/" + std::to_string(trees) + " single-line trees likewise");
  check(withComments == trees, std::to_string(withComments) + "/" + std::to_string(trees) +
                                   " trees with comments, read with comments kept");
}

void testXmlRefusals() {
  std::printf("\nXML: malformed and hostile documents are refused, by name\n");
  struct Case {
    const char* name;
    std::string text;
    const char* errorHas;
  };
  const std::vector<Case> cases = {
      {"empty document", "", "no root element"},
      {"only a comment", "<!-- x -->", "no root element"},
      {"text before the root", "hello <a/>", "expected the root element"},
      {"unclosed element", "<a><b></b>", "unclosed element <a>"},
      {"mismatched end tag", "<a><b></a></b>", "</a> does not close <b>"},
      {"two roots", "<a/><b/>", "content after the root element"},
      {"text after the root", "<a/>junk", "content after the root element"},
      {"duplicate attribute", "<a x='1' x='2'/>", "duplicate attribute x"},
      {"duplicate among many", "<a a0='' a1='' a2='' a3='' a4='' a5='' a6='' a7='' a8='' a3=''/>",
       "duplicate attribute a3"},
      {"unquoted attribute", "<a x=1/>", "expected a quoted value"},
      {"attribute without '='", "<a x/>", "expected '=' after attribute x"},
      {"no space between attributes", "<a x='1'y='2'/>", "expected whitespace"},
      {"'<' in an attribute", "<a x='<'/>", "'<' is not allowed"},
      {"bare '&'", "<a>fish & chips</a>", "must be written as &amp;"},
      {"HTML entity", "<a>&nbsp;</a>", "undefined entity &nbsp;"},
      {"&#0;", "<a>&#0;</a>", "not a character"},
      {"surrogate reference", "<a>&#xD800;</a>", "not a character"},
      {"reference past U+10FFFF", "<a>&#x110000;</a>", "not a character"},
      {"huge reference", "<a>&#99999999999999999999;</a>", "not a character"},
      {"reference without ';'", "<a>&#65</a>", "malformed character reference"},
      {"invalid UTF-8", "<a>\xC3\x28</a>", "invalid UTF-8"},
      {"overlong UTF-8", "<a>\xC0\xAF</a>", "invalid UTF-8"},
      {"encoded surrogate", "<a>\xED\xA0\x80</a>", "invalid UTF-8"},
      {"NUL byte", std::string("<a>x\0y</a>", 10), "NUL"},
      {"control character", "<a>\x01</a>", "control character"},
      {"U+FFFF", "<a>\xEF\xBF\xBF</a>", "U+FFFE and U+FFFF"},
      {"UTF-16 BOM", "\xFF\xFE<\0a\0/\0>\0", "UTF-16"},
      {"unsupported encoding", "<?xml version='1.0' encoding='Shift_JIS'?><a/>", "unsupported encoding"},
      {"XML 2.0", "<?xml version='2.0'?><a/>", "unsupported XML version"},
      {"declaration not at the start", " <?xml version='1.0'?><a/>", "only allowed at the very start"},
      {"unterminated comment", "<a><!-- x </a>", "unterminated comment"},
      {"unterminated CDATA", "<a><![CDATA[ x </a>", "unterminated CDATA"},
      {"unterminated PI", "<a><?pi x </a>", "unterminated processing instruction"},
      {"unterminated DOCTYPE", "<!DOCTYPE a [ <!ENTITY x 'y'> ", "unterminated DOCTYPE"},
      {"DOCTYPE inside the root", "<a><!DOCTYPE a></a>", "only allowed before the root"},
      {"two DOCTYPEs", "<!DOCTYPE a><!DOCTYPE a><a/>", "more than one DOCTYPE"},
      {"unterminated start tag", "<a x='1'", "unterminated"},
      {"'<' then nothing", "<a><", "expected an element name"},
  };
  for (const Case& c : cases) {
    std::string error;
    std::unique_ptr<sonore::XmlElement> e = sonore::XmlDocument::parse(c.text, &error);
    const bool ok = !e && contains(error, c.errorHas) && contains(error, "line ");
    check(ok, std::string(c.name) + " -> \"" + error + "\"");
  }

  std::string error;
  sonore::XmlDocument::parse("<a>\n  <b></c>\n</a>", &error);
  check(contains(error, "line 2, column 6"), "the position is line and column: \"" + error + "\"");
  sonore::XmlDocument::parse("<a>\xC3\xA9\xC3\xA9<</a>", &error);
  check(contains(error, "line 1, column 7"), "columns count characters, not bytes: \"" + error + "\"");
}

void testXmlHostile() {
  std::printf("\nXML: XXE, billion laughs, depth, breadth, size\n");
  std::string error;
  // XXE: an external entity is declared and used. Nothing is fetched; using it
  // is an error that names it.
  const std::string xxe =
      "<?xml version=\"1.0\"?>\n<!DOCTYPE foo [ <!ELEMENT foo ANY >\n"
      "<!ENTITY xxe SYSTEM \"file:///etc/passwd\" >]>\n<foo>&xxe;</foo>";
  const bool xxeRefused = !sonore::XmlDocument::parse(xxe, &error);
  check(xxeRefused && contains(error, "undefined entity &xxe;"),
        "XXE: the external entity is never resolved -- \"" + error + "\"");
  const std::string xxeUnused = "<!DOCTYPE foo SYSTEM \"http://example.invalid/evil.dtd\"><foo>safe</foo>";
  std::unique_ptr<sonore::XmlElement> unused = sonore::XmlDocument::parse(xxeUnused, &error);
  check(unused && unused->getAllSubText() == "safe", "an external DTD is named and ignored, the document still reads");

  std::string laughs = "<?xml version=\"1.0\"?>\n<!DOCTYPE lolz [\n <!ENTITY lol \"lol\">\n";
  laughs += " <!ENTITY lol1 \"&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;&lol;\">\n";
  for (int i = 2; i <= 9; ++i) {
    laughs += " <!ENTITY lol" + std::to_string(i) + " \"";
    for (int k = 0; k < 10; ++k) laughs += "&lol" + std::to_string(i - 1) + ";";
    laughs += "\">\n";
  }
  laughs += "]>\n<lolz>&lol9;</lolz>";
  auto t0 = std::chrono::steady_clock::now();
  check(!sonore::XmlDocument::parse(laughs, &error) && contains(error, "undefined entity &lol9;") && seconds(t0) < 2.0,
        "billion laughs: refused at the first reference, nothing expanded");

  // 100,000 levels: refused at the depth limit, quickly, without recursion.
  std::string deep;
  for (int i = 0; i < 100000; ++i) deep += "<d>";
  t0 = std::chrono::steady_clock::now();
  check(!sonore::XmlDocument::parse(deep, &error) && contains(error, "nested more deeply"),
        "100,000 nested elements: refused at the depth limit (256)");
  // And with the limit lifted, parsed, copied, written and destroyed -- none
  // of which recurses, so none of it can run out of stack.
  for (int i = 0; i < 100000; ++i) deep += "</d>";
  sonore::XmlDocument deepDoc(deep);
  sonore::XmlDocument::Limits lifted;
  lifted.maxDepth = 200000;
  deepDoc.setLimits(lifted);
  {
    std::unique_ptr<sonore::XmlElement> tower = deepDoc.getDocumentElement();
    bool ok = tower != nullptr;
    if (ok) {
      sonore::XmlElement copy(*tower);
      sonore::XmlElement::TextFormat f;
      const std::string text = copy.toString(f.singleLine().withoutHeader());
      ok = text.size() == deep.size() - 3 && copy.isEquivalentTo(tower.get(), false);
    }
    check(ok, "100,000 levels with the limit lifted: parsed, deep-copied, written, compared");
  }
  check(seconds(t0) < 60.0, "...and destroyed, all without recursion");

  // 50,000 attributes on one element: duplicates are found by sorting.
  std::string wide = "<w";
  for (int i = 0; i < 50000; ++i) wide += " a" + std::to_string(i) + "=''";
  t0 = std::chrono::steady_clock::now();
  std::unique_ptr<sonore::XmlElement> w = sonore::XmlDocument::parse(wide + "/>", &error);
  const bool wideOk = w && w->getNumAttributes() == 50000;
  const bool dupFound = !sonore::XmlDocument::parse(wide + " a49999=''/>", &error) &&
                        contains(error, "duplicate attribute a49999");
  check(wideOk && dupFound && seconds(t0) < 20.0,
        "50,000 attributes read; a duplicate among them found by sorting, not 1.25e9 comparisons");

  sonore::XmlDocument::Limits tight;
  tight.maxNodes = 100;
  std::string many = "<r>";
  for (int i = 0; i < 200; ++i) many += "<i/>";
  many += "</r>";
  sonore::XmlDocument manyDoc(many);
  manyDoc.setLimits(tight);
  check(!manyDoc.getDocumentElement() && contains(manyDoc.getLastParseError(), "more nodes than the limit"),
        "the node limit refuses 200 elements when 100 are allowed");
  tight.maxNodes = (size_t) 1 << 20;
  tight.maxInputBytes = 10;
  sonore::XmlDocument bigDoc("<a>0123456789</a>");
  bigDoc.setLimits(tight);
  check(!bigDoc.getDocumentElement() && contains(bigDoc.getLastParseError(), "size limit"),
        "the size limit refuses a document past it");
}

void testXmlFiles() {
  std::printf("\nXML: files by UTF-8 path\n");
  const std::string path = sonoretest::tempDir() + "/core_data_\xC3\xA9\xE9\x9F\xB3.xml";
  sonore::XmlElement root("settings");
  root.createNewChildElement("value")->setAttribute("x", 1.25);
  check(root.writeToFile(path), "writeToFile through a non-ASCII path");
  std::string error;
  std::unique_ptr<sonore::XmlElement> back = sonore::XmlDocument::parseFile(path, &error);
  check(back && back->isEquivalentTo(&root, false), "parseFile reads it back equivalent");
  check(!sonore::XmlDocument::parseFile(path + ".missing", &error) && contains(error, "cannot open"),
        "a missing file is an error, not a crash");
  sonore::pathio::remove(path.c_str());
}

// ── LocalisedStrings ───────────────────────────────────────────────────────

void testLocalisation() {
  std::printf("\nLocalisedStrings: the file format, lookups, fallbacks\n");
  const std::string file =
      "\xEF\xBB\xBF// French translations for the test plugin\r\n"
      "language: French\r\n"
      "countries: fr be, ch  lu\r\n"
      "\r\n"
      "\"Save preset\" = \"Enregistrer le preset\"\r\n"
      "  \"Gain\"=\"Gain\"   // same in both\n"
      "\"Sample \\\"%s\\\" not found\" = \"\xC3\x89\x63hantillon \\\"%s\\\" introuvable\"\n"
      "\"Two\\nlines\" = \"Deux\\nlignes\"\n"
      "\"Path\\\\\" = \"Chemin\\\\\"\n"
      "\"Untranslated\" = \"\"\n"
      "# a hash comment\n"
      "\"Broken = \"no closing quote\n"
      "\"No equals\" \"x\"\n"
      "\"\" = \"empty original\"\r"
      "\"Last line\" = \"Derni\xC3\xA8re ligne\"";
  sonore::LocalisedStrings fr(file);
  check(fr.getLanguageName() == "French", "language: French");
  check(fr.getCountryCodes() == std::vector<std::string>{"fr", "be", "ch", "lu"},
        "countries: fr be ch lu (spaces and commas)");
  check(fr.translate("Save preset") == "Enregistrer le preset", "a plain mapping");
  check(fr.translate("Sample \"%s\" not found") == "\xC3\x89\x63hantillon \"%s\" introuvable",
        "escaped quotes on both sides, UTF-8 kept");
  check(fr.translate("Two\nlines") == "Deux\nlignes" && fr.translate("Path\\") == "Chemin\\",
        "\\n and \\\\ escapes (a string may end in a backslash)");
  check(fr.translate("Untranslated") == "Untranslated", "an empty translation leaves the original");
  check(fr.translate("Not in the file") == "Not in the file" &&
            fr.translate("Not in the file", "?") == "?",
        "a missing string is itself, or the given fallback result");
  check(fr.translate("Last line") == "Derni\xC3\xA8re ligne", "CR line ends and a last line with no newline");
  check(fr.getMalformedLines() == std::vector<int>{12, 13, 14}, "malformed mappings reported by line: 12, 13, 14");
  check(fr.size() == 6, "six mappings loaded");
  check(fr.translate("save preset") == "save preset", "case matters by default");

  sonore::LocalisedStrings folded(file, true);
  check(folded.translate("SAVE PRESET") == "Enregistrer le preset" && folded.isIgnoringCase(),
        "ignoreCase: ASCII case folded on lookup");

  auto canadian = std::unique_ptr<sonore::LocalisedStrings>(
      new sonore::LocalisedStrings("language: French (Canada)\n\"Save preset\" = \"Sauvegarder le preset\"\n"));
  canadian->setFallback(std::unique_ptr<sonore::LocalisedStrings>(new sonore::LocalisedStrings(fr)));
  check(canadian->translate("Save preset") == "Sauvegarder le preset" &&
            canadian->translate("Gain") == "Gain" && canadian->translate("Last line") == "Derni\xC3\xA8re ligne",
        "a regional table falls back on the general one");
  sonore::LocalisedStrings copied(*canadian);
  check(copied.getFallback() && copied.getFallback() != canadian->getFallback() &&
            copied.translate("Last line") == "Derni\xC3\xA8re ligne",
        "a copy copies its fallbacks, not shares them");

  sonore::LocalisedStrings built;
  built.setTranslation("Hello", "Bonjour");
  built.addStrings(fr);
  built.setTranslation("Gain", "");
  check(built.translate("Hello") == "Bonjour" && built.translate("Save preset") == "Enregistrer le preset" &&
            built.translate("Gain") == "Gain" && built.getLanguageName() == "French",
        "setTranslation and addStrings; an empty translation removes one");
  const std::map<std::string, std::string> m = fr.getMappings();
  check(m.size() == 6 && m.at("Two\nlines") == "Deux\nlignes", "getMappings");

  // Files.
  const std::string path = sonoretest::tempDir() + "/fran\xC3\xA7\x61is.txt";
  if (std::FILE* f = sonore::pathio::open(path.c_str(), "wb")) {
    std::fwrite(file.data(), 1, file.size(), f);
    std::fclose(f);
  }
  std::string error;
  std::unique_ptr<sonore::LocalisedStrings> loaded = sonore::LocalisedStrings::fromFile(path, false, &error);
  check(loaded && loaded->translate("Save preset") == "Enregistrer le preset", "fromFile, by UTF-8 path");
  check(!sonore::LocalisedStrings::fromFile(path + ".none", false, &error) && contains(error, "cannot open"),
        "fromFile on a missing file says so");

  // UTF-16 with its byte-order mark, as Notepad's "Unicode" saves it and the
  // reference reads it. Read as bytes, every other one was a NUL: the file
  // loaded no mapping, no language, and reported no line as malformed.
  const std::u16string wide = u"language: French\r\n\"Save preset\" = \"Enregistrer le preset\"\r\n"
                              u"\"Summer\" = \"Été \U0001F3B5\"\r\n";
  for (int big = 0; big < 2; ++big) {
    std::string bytes = big ? "\xFE\xFF" : "\xFF\xFE";
    for (char16_t unit : wide) {
      const char high = (char) (unit >> 8), low = (char) (unit & 0xFF);
      bytes += big ? high : low;
      bytes += big ? low : high;
    }
    if (std::FILE* f = sonore::pathio::open(path.c_str(), "wb")) {
      std::fwrite(bytes.data(), 1, bytes.size(), f);
      std::fclose(f);
    }
    std::unique_ptr<sonore::LocalisedStrings> utf16 = sonore::LocalisedStrings::fromFile(path, false, &error);
    check(utf16 && utf16->getLanguageName() == "French" && utf16->size() == 2 &&
              utf16->translate("Save preset") == "Enregistrer le preset" &&
              utf16->translate("Summer") == "\xC3\x89t\xC3\xA9 \xF0\x9F\x8E\xB5" && utf16->getMalformedLines().empty(),
          big ? "a UTF-16 big-endian file loads too"
              : "a UTF-16 file (Notepad's \"Unicode\") loads, a character past U+FFFF included");
  }
  sonore::pathio::remove(path.c_str());

  // The process-wide table.
  sonore::LocalisedStrings::setCurrentMappings(nullptr);
  check(SONORE_TRANS("Save preset") == "Save preset" && sonore::translate("Gain", "x") == "x",
        "with no current table, SONORE_TRANS gives the original");
  sonore::LocalisedStrings::setCurrentMappings(std::unique_ptr<sonore::LocalisedStrings>(new sonore::LocalisedStrings(fr)));
  check(SONORE_TRANS("Save preset") == "Enregistrer le preset" &&
            sonore::translate(std::string("Gain")) == "Gain" &&
            std::string(SONORE_NEEDS_TRANS("Save preset")) == "Save preset",
        "setCurrentMappings: SONORE_TRANS translates; SONORE_NEEDS_TRANS only marks");
  std::shared_ptr<const sonore::LocalisedStrings> held = sonore::LocalisedStrings::getCurrentMappings();
  sonore::LocalisedStrings::setCurrentMappings(nullptr);
  check(held && held->translate("Save preset") == "Enregistrer le preset" &&
            SONORE_TRANS("Save preset") == "Save preset",
        "a held table outlives being replaced");

  // Readers on four threads while the language changes 2,000 times.
  const std::string en = "Save preset", frText = "Enregistrer le preset", de = "Preset speichern";
  std::atomic<bool> stop{false};
  std::atomic<long> reads{0}, wrong{0};
  std::vector<std::thread> readers;
  for (int t = 0; t < 4; ++t)
    readers.emplace_back([&] {
      while (!stop.load()) {
        const std::string s = sonore::translate(en);
        if (s != en && s != frText && s != de) ++wrong;
        ++reads;
      }
    });
  // The readers must actually be reading while the table changes: wait for
  // them to start, and keep each change waiting until they have moved on.
  while (reads.load() < 100) std::this_thread::yield();
  for (int i = 0; i < 2000; ++i) {
    const long before = reads.load();
    while (reads.load() < before + 4) std::this_thread::yield();
    if (i % 3 == 0) sonore::LocalisedStrings::setCurrentMappings(nullptr);
    else if (i % 3 == 1) sonore::LocalisedStrings::setCurrentMappings(std::unique_ptr<sonore::LocalisedStrings>(new sonore::LocalisedStrings(fr)));
    else
      sonore::LocalisedStrings::setCurrentMappings(std::unique_ptr<sonore::LocalisedStrings>(
          new sonore::LocalisedStrings("\"Save preset\" = \"Preset speichern\"")));
  }
  stop = true;
  for (std::thread& t : readers) t.join();
  sonore::LocalisedStrings::setCurrentMappings(nullptr);
  check(wrong.load() == 0 && reads.load() > 0,
        std::to_string(reads.load()) + " lookups on 4 threads during 2,000 language changes, every one valid");
}

// ── TextDiff ───────────────────────────────────────────────────────────────

size_t lcsLength(const std::string& a, const std::string& b) {
  std::vector<size_t> prev(b.size() + 1, 0), cur(b.size() + 1, 0);
  for (size_t i = 1; i <= a.size(); ++i) {
    for (size_t j = 1; j <= b.size(); ++j)
      cur[j] = a[i - 1] == b[j - 1] ? prev[j - 1] + 1 : (std::max)(prev[j], cur[j - 1]);
    std::swap(prev, cur);
  }
  return prev[b.size()];
}

std::string randomFrom(Rng& r, const char* alphabet, int maxLength) {
  const int n = r.below(maxLength + 1);
  const size_t k = std::strlen(alphabet);
  std::string s;
  for (int i = 0; i < n; ++i) s += alphabet[r.below((int) k)];
  return s;
}

std::string mutate(Rng& r, std::string s, int edits) {
  for (int e = 0; e < edits; ++e) {
    const int op = r.below(3);
    const size_t at = s.empty() ? 0 : (size_t) r.below((int) s.size() + 1);
    if (op == 0 || s.empty()) s.insert(at, 1, (char) ('a' + r.below(26)));
    else if (op == 1 && at < s.size()) s.erase(at, 1);
    else if (at < s.size()) s[at] = (char) ('a' + r.below(26));
  }
  return s;
}

void testTextDiff() {
  std::printf("\nTextDiff: changes that reproduce the target, minimally\n");
  {
    sonore::TextDiff d("hello world", "hello there world");
    check(d.changes.size() == 1 && d.changes[0].start == 6 && d.changes[0].length == 0 &&
              d.changes[0].insertedText == "there " && !d.changes[0].isDeletion(),
          "\"hello world\" -> \"hello there world\": one insertion of \"there \" at 6");
    sonore::TextDiff del("the quick brown fox", "the brown fox");
    check(del.changes.size() == 1 && del.changes[0].isDeletion() && del.changes[0].start == 4 &&
              del.changes[0].length == 6,
          "a deletion: 6 bytes at 4");
    sonore::TextDiff two("abcdef", "aXcdeY");
    check(two.changes.size() == 2 && two.appliedTo("abcdef") == "aXcdeY" && two.changes[0].start == 1 &&
              two.changes[0].length == 1 && two.changes[0].insertedText == "X" && two.changes[1].start == 5,
          "two replacements, the second's offset counted after the first is applied");
    check(sonore::TextDiff("", "abc").appliedTo("") == "abc" && sonore::TextDiff("abc", "").appliedTo("abc").empty() &&
              sonore::TextDiff("", "").changes.empty() && sonore::TextDiff("same", "same").changes.empty(),
          "empty to text, text to empty, and no change at all");
    const std::string a = "caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC", b = "caf\xC3\xA8 \xE6\x97\xA5\xE6\x9C\xAC\xF0\x9F\x98\x80";
    sonore::TextDiff u(a, b);
    bool boundaries = u.appliedTo(a) == b;
    for (const auto& c : u.changes)
      boundaries = boundaries && (c.start >= b.size() || ((unsigned char) b[c.start] & 0xC0) != 0x80);
    check(boundaries && u.changes.size() == 2 && u.changes[0].insertedText == "\xC3\xA8",
          "UTF-8 is compared by character: \"\xC3\xA9\" -> \"\xC3\xA8\" is one whole character, not one byte");
  }

  Rng r(12345);
  int applied = 0, minimal = 0, total = 0;
  for (int i = 0; i < 3000; ++i) {
    const char* alphabet = i % 3 == 0 ? "ab" : i % 3 == 1 ? "abcd" : "abcdefghijklmnopqrstuvwxyz";
    std::string a = randomFrom(r, alphabet, 60);
    std::string b = i % 2 ? mutate(r, a, 1 + r.below(8)) : randomFrom(r, alphabet, 60);
    sonore::TextDiff d(a, b);
    ++total;
    if (d.appliedTo(a) == b) ++applied;
    size_t edited = 0;
    for (const auto& c : d.changes) edited += c.length + c.insertedText.size();
    if (d.isMinimal() && edited == a.size() + b.size() - 2 * lcsLength(a, b)) ++minimal;
  }
  check(applied == total, std::to_string(applied) + "/" + std::to_string(total) + " random ASCII pairs: applying reproduces the target");
  check(minimal == total, std::to_string(minimal) + "/" + std::to_string(total) +
                              " minimal: removed + inserted == N + M - 2 LCS, against a DP LCS");

  int bytes = 0;
  for (int i = 0; i < 1000; ++i) {
    std::string a, b;
    const int n = r.below(80), m = r.below(80);
    for (int k = 0; k < n; ++k) a += (char) (r.below(4) == 0 ? 0x80 + r.below(128) : 'a' + r.below(3));
    for (int k = 0; k < m; ++k) b += (char) (r.below(4) == 0 ? 0x80 + r.below(128) : 'a' + r.below(3));
    if (sonore::TextDiff(a, b).appliedTo(a) == b) ++bytes;
  }
  check(bytes == 1000, std::to_string(bytes) + "/1000 random byte strings with invalid UTF-8: exact round trip");

  // A long text with a few edits: fast, minimal, small.
  std::string longText = randomFrom(r, "abcdefghij ", 0);
  for (int i = 0; i < 200000; ++i) longText += (char) ('a' + r.below(26));
  std::string edited = longText;
  edited.insert(1000, "INSERTED");
  edited.erase(150000, 10);
  edited[199000] = '#';
  auto t0 = std::chrono::steady_clock::now();
  sonore::TextDiff big(longText, edited);
  size_t bigEdited = 0;
  for (const auto& ch : big.changes) bigEdited += ch.length + ch.insertedText.size();
  // 8 inserted, 10 removed, 1 replaced: 20 characters. The ten removed may
  // come out as two changes -- removing "xyzx" or "zxyz" is the same cost when
  // the text around allows both -- so the count is bounded, not fixed.
  check(big.appliedTo(longText) == edited && big.isMinimal() && bigEdited == 20 && big.changes.size() >= 3 &&
            big.changes.size() <= 5 && seconds(t0) < 5.0,
        "200,000 characters, three edits: " + std::to_string(big.changes.size()) +
            " changes of 20 characters in all, minimal, quick");

  // Budget: a tiny one still reproduces the target, but says it is not minimal.
  std::string x, y;
  for (int i = 0; i < 400; ++i) {
    x += (char) ('a' + r.below(3));
    y += (char) ('a' + r.below(3));
  }
  sonore::TextDiff starved(x, y, 50);
  check(starved.appliedTo(x) == y && !starved.isMinimal(), "a starved budget: still exact, isMinimal() false");

  // The size guard: two unrelated 1.2-million-character strings.
  std::string p1, p2;
  for (int i = 0; i < 1200000; ++i) {
    p1 += (char) ('a' + r.below(26));
    p2 += (char) ('A' + r.below(26));
  }
  p1 = "same start " + p1 + " same end";
  p2 = "same start " + p2 + " same end";
  t0 = std::chrono::steady_clock::now();
  sonore::TextDiff guarded(p1, p2);
  check(guarded.appliedTo(p1) == p2 && guarded.changes.size() == 1 && guarded.changes[0].start == 11 &&
            !guarded.isMinimal() && seconds(t0) < 20.0,
        "2.4 million differing characters: past the size guard, one replacement between the common ends");
  const sonore::TextDiff::Change c = guarded.changes[0];
  check(c.appliedTo("short") == std::string("short") + c.insertedText, "a change applied to the wrong text clamps, never throws");
}

// ── Expression ─────────────────────────────────────────────────────────────

void testExpression() {
  std::printf("\nExpression: results against the same formula in C++\n");
  struct Case {
    const char* text;
    std::function<double(double, double)> cpp;
  };
  const std::vector<Case> cases = {
      {"1 + 2 * 3", [](double, double) { return 1.0 + 2.0 * 3.0; }},
      {"(1 + 2) * 3", [](double, double) { return (1.0 + 2.0) * 3.0; }},
      {"2 - 3 - 4", [](double, double) { return 2.0 - 3.0 - 4.0; }},
      {"2 / 4 / 8", [](double, double) { return 2.0 / 4.0 / 8.0; }},
      {"-2 * -3", [](double, double) { return -2.0 * -3.0; }},
      {"--x", [](double x, double) { return -(-x); }},
      {"+x - -y", [](double x, double y) { return x - -y; }},
      {"x * y + x / y - (x - y) * (x + y)", [](double x, double y) { return x * y + x / y - (x - y) * (x + y); }},
      {"sin(x) * cos(y) + sqrt(abs(x - y))",
       [](double x, double y) { return std::sin(x) * std::cos(y) + std::sqrt(std::fabs(x - y)); }},
      {"pow(x, 3) - exp(y / 10) + log(abs(x) + 10)",
       [](double x, double y) { return std::pow(x, 3.0) - std::exp(y / 10.0) + std::log(std::fabs(x) + 10.0); }},
      {"min(x, y, 3) + max(1, 2) * max(y)",
       [](double x, double y) { return (std::min)((std::min)(x, y), 3.0) + 2.0 * y; }},
      {"tan(x / 7) + atan2(y, x) + atan(y) + asin(0.5) + acos(0.25)",
       [](double x, double y) { return std::tan(x / 7.0) + std::atan2(y, x) + std::atan(y) + std::asin(0.5) + std::acos(0.25); }},
      {"floor(x) + ceil(y) + round(x * y) + fmod(x, 3) + log10(1000)",
       [](double x, double y) { return std::floor(x) + std::ceil(y) + std::round(x * y) + std::fmod(x, 3.0) + std::log10(1000.0); }},
      {"1e3 + .5 + 2. + 1.5E-2", [](double, double) { return 1e3 + .5 + 2. + 1.5E-2; }},
      {"  x\t*\n( y+1 )  ", [](double x, double y) { return x * (y + 1.0); }},
  };
  Rng r(777);
  int agree = 0, total = 0;
  for (const Case& c : cases) {
    std::string error;
    sonore::Expression e(c.text, error);
    if (!error.empty()) {
      check(false, std::string(c.text) + " failed to parse: " + error);
      continue;
    }
    bool all = true;
    for (int i = 0; i < 200; ++i) {
      const double x = (r.uniform() - 0.5) * 20.0, y = (r.uniform() - 0.5) * 20.0;
      sonore::Expression::VariableScope scope;
      scope.set("x", x);
      scope.set("y", y);
      std::string evalError;
      const double got = e.evaluate(scope, evalError), want = c.cpp(x, y);
      const bool same = evalError.empty() &&
                        ((std::isnan(got) && std::isnan(want)) || got == want ||
                         std::fabs(got - want) <= 1e-12 * (std::max)(1.0, std::fabs(want)));
      all = all && same;
      ++total;
      if (same) ++agree;
    }
    check(all, std::string(c.text) + " agrees with C++ over 200 random x, y");
  }
  check(agree == total, std::to_string(agree) + "/" + std::to_string(total) + " evaluations agree");
  check(std::isinf(sonore::Expression::parse("1 / 0").evaluate()) &&
            std::isnan(sonore::Expression::parse("sqrt(-1)").evaluate()),
        "IEEE as C++: 1/0 is inf, sqrt(-1) is NaN -- not errors");

  std::printf("\nExpression: errors, limits, hostile input\n");
  struct Bad {
    const char* text;
    const char* errorHas;
  };
  const Bad bad[] = {
      {"", "empty expression"},       {"   ", "empty expression"},       {"1 +", "unexpected end"},
      {"(1 + 2", "expected ')'"},     {"1 + 2)", "unexpected ')'"},     {"1 2", "unexpected '2'"},
      {"sin(1, ", "unexpected end"},  {"max(1 2)", "expected ',' or ')'"}, {"1e", "exponent needs digits"},
      {"1e999", "out of range"},      {"@", "unexpected '@'"},           {"x..y", "unexpected '.'"},
      {".", "malformed number"},      {"2 * \xC3\xA9", "byte 0xC3"},
  };
  for (const Bad& b : bad) {
    std::string error;
    sonore::Expression e = sonore::Expression::parse(b.text, &error);
    check(contains(error, b.errorHas) && contains(error, "column ") && e.evaluate() == 0.0,
          std::string("\"") + b.text + "\" -> \"" + error + "\"");
  }
  struct BadEval {
    const char* text;
    const char* errorHas;
  };
  const BadEval badEval[] = {
      {"sin(1, 2)", "sin takes 1 argument, given 2"},
      {"pow(2)", "pow takes 2 arguments, given 1"},
      {"min()", "min takes at least 1 argument"},
      {"nosuch(1)", "unknown function: nosuch"},
      {"q + 1", "unknown symbol: q"},
  };
  for (const BadEval& b : badEval) {
    std::string error;
    const double v = sonore::Expression::parse(b.text).evaluate(sonore::Expression::Scope(), error);
    check(v == 0.0 && contains(error, b.errorHas), std::string("evaluating \"") + b.text + "\" -> \"" + error + "\"");
  }

  std::string deepText(150, '(');
  deepText += "1";
  deepText += std::string(150, ')');
  std::string error;
  sonore::Expression::parse(deepText, &error);
  check(contains(error, "nested more deeply"), "150 nested parentheses: refused at the depth limit (100)");
  std::string deepCalls;
  for (int i = 0; i < 150; ++i) deepCalls += "abs(";
  deepCalls += "1" + std::string(150, ')');
  sonore::Expression::parse(deepCalls, &error);
  check(contains(error, "nested more deeply"), "150 nested calls: refused likewise");
  sonore::Expression::parse(std::string(70000, '1'), &error);
  check(contains(error, "longer than the limit"), "70,000 characters: refused at the length limit");
  sonore::Expression::Limits few;
  few.maxNodes = 10;
  sonore::Expression::parse("1+1+1+1+1+1+1+1+1+1+1", &error, few);
  check(contains(error, "more terms than the limit"), "the term limit");

  // 20,000 unary minuses: 20,001 nodes, a tree 20,001 deep, and nothing recurses.
  const std::string minuses = std::string(20000, '-') + "7";
  sonore::Expression chain = sonore::Expression::parse(minuses, &error);
  check(error.empty() && chain.evaluate() == 7.0, "20,000 unary minuses parse and evaluate to 7 without recursion");
  const std::string chainText = chain.toString();
  check(chainText.size() == minuses.size() && sonore::Expression::parse(chainText).evaluate() == 7.0,
        "...and print back as themselves");

  // Built in code, 300,000 deep, then evaluated and destroyed.
  {
    sonore::Expression sum(0.0);
    for (int i = 0; i < 300000; ++i) sum = sum + sonore::Expression(1.0);
    sonore::Expression::Limits generous;
    generous.maxEvaluationSteps = 2000000;
    std::string evalError;
    const double v = sum.evaluate(sonore::Expression::Scope(), evalError, generous);
    sonore::Expression::Limits stingy;
    std::string stingyError;
    sum.evaluate(sonore::Expression::Scope(), stingyError, stingy);
    check(v == 300000.0 && evalError.empty() && contains(stingyError, "more steps"),
          "a sum 300,000 terms deep: 300000 within a raised step budget, refused within the default");
  }
  check(true, "...and destroyed without recursion");

  // Symbols defined in terms of each other.
  sonore::Expression::VariableScope scope;
  scope.set("bpm", 120.0);
  scope.set("beat", sonore::Expression::parse("60 / bpm"));
  scope.set("bar", sonore::Expression::parse("beat * 4"));
  check(sonore::Expression::parse("bar * 2").evaluate(scope) == 4.0, "symbols through other symbols: bar * 2 = 4");
  scope.set("a", sonore::Expression::parse("b + 1"));
  scope.set("b", sonore::Expression::parse("a * 2"));
  std::string cycle;
  sonore::Expression::parse("a").evaluate(scope, cycle);
  check(contains(cycle, "defined in terms of itself"), "a = b + 1, b = a * 2: \"" + cycle + "\"");
  scope.set("s0", 1.0);
  for (int i = 1; i <= 40; ++i)
    scope.set("s" + std::to_string(i),
              sonore::Expression::parse("s" + std::to_string(i - 1) + " + s" + std::to_string(i - 1)));
  std::string blowup;
  auto t0 = std::chrono::steady_clock::now();
  sonore::Expression::parse("s40").evaluate(scope, blowup);
  check(contains(blowup, "more steps") && seconds(t0) < 10.0, "s40 = s39 + s39 ... (2^40 terms): stopped by the step budget");

  std::printf("\nExpression: printing, simplifying, looking inside\n");
  const char* texts[] = {"1 + 2 * 3", "(1 + 2) * 3", "a - (b - c)", "a - b - c", "a / (b * c)", "-(a + b)",
                         "--a", "f(a, -b, (c + d) * e)", "min(1)", "x.y * 2.5e-07", "a + (b + c)"};
  bool stable = true;
  for (const char* t : texts) {
    const sonore::Expression e = sonore::Expression::parse(t);
    const std::string once = e.toString();
    const std::string twice = sonore::Expression::parse(once).toString();
    stable = stable && once == t && twice == once;
    if (once != t) std::printf("       %s printed as %s\n", t, once.c_str());
  }
  check(stable, "toString gives back exactly what was parsed, parentheses only where needed");
  // Random expressions: print, re-parse, same value to the bit.
  int sameValue = 0;
  const int randomCount = 2000;
  for (int i = 0; i < randomCount; ++i) {
    std::string t;
    const int terms = 1 + r.below(6);
    for (int k = 0; k < terms; ++k) {
      if (k) t += " +-*/"[1 + r.below(4)];
      const int kind = r.below(5);
      if (kind == 0) t += std::to_string(r.below(100)) + "." + std::to_string(r.below(10));
      else if (kind == 1) t += "x";
      else if (kind == 2) t += "(-y + " + std::to_string(r.below(9)) + ")";
      else if (kind == 3) t += "sin(x * " + std::to_string(r.below(5)) + ")";
      else t += "max(x, y, 0.5)";
    }
    sonore::Expression::VariableScope s;
    s.set("x", r.uniform() * 4 - 2);
    s.set("y", r.uniform() * 4 - 2);
    const sonore::Expression e = sonore::Expression::parse(t);
    const double a = e.evaluate(s), b = sonore::Expression::parse(e.toString()).evaluate(s);
    if (a == b || (std::isnan(a) && std::isnan(b))) ++sameValue;
  }
  check(sameValue == randomCount, std::to_string(sameValue) + "/" + std::to_string(randomCount) +
                                      " random expressions printed and re-parsed give the identical value");

  check(sonore::Expression::parse("x * (2 * 3)").simplified().toString() == "x * 6" &&
            sonore::Expression::parse("-(1 + 2) + x").simplified().toString() == "-3 + x" &&
            sonore::Expression::parse("sin(0) + 1 / 0").simplified().toString() == "sin(0) + 1 / 0",
        "simplified folds constant operators, never functions or a division by zero");

  const sonore::Expression e = sonore::Expression::parse("f(a, 2) + -b * a");
  check(e.getType() == sonore::Expression::Type::operatorType && e.getSymbolOrFunction() == "+" &&
            e.getNumInputs() == 2 && e.getInput(0).getType() == sonore::Expression::Type::functionType &&
            e.getInput(0).getSymbolOrFunction() == "f" && e.getInput(0).getNumInputs() == 2 &&
            e.getInput(0).getInput(1).getType() == sonore::Expression::Type::constantType &&
            e.getInput(1).getInput(0).getSymbolOrFunction() == "-" &&
            e.getInput(1).getInput(0).getNumInputs() == 1,
        "getType / getSymbolOrFunction / getNumInputs / getInput walk the tree");
  check(e.findReferencedSymbols() == std::vector<std::string>{"a", "b"} && e.referencesSymbol("b") &&
            !e.referencesSymbol("f"),
        "findReferencedSymbols: a, b (f is a function, not a symbol)");
  check(e.withRenamedSymbol("a", "gain").toString() == "f(gain, 2) + -b * gain", "withRenamedSymbol");
  const sonore::Expression built =
      (sonore::Expression::symbol("x") + sonore::Expression(2.0)) * sonore::Expression(3.0) -
      sonore::Expression::function("max", {sonore::Expression(1.0), -sonore::Expression::symbol("x")});
  sonore::Expression::VariableScope one;
  one.set("x", 1.0);
  check(built.toString() == "(x + 2) * 3 - max(1, -x)" && built.evaluate(one) == 8.0,
        "built with operators: (x + 2) * 3 - max(1, -x) = 8 at x = 1");

  // A scope that adds a function, falling back on the standard ones.
  struct DbScope : sonore::Expression::Scope {
    bool evaluateFunction(const std::string& name, const double* p, int n, double& result,
                          std::string& err) const override {
      if (name == "db") {
        if (n != 1) return false;
        result = std::pow(10.0, p[0] / 20.0);
        return true;
      }
      return Scope::evaluateFunction(name, p, n, result, err);
    }
  };
  check(std::fabs(sonore::Expression::parse("db(-6) * sqrt(4)").evaluate(DbScope()) - 2.0 * std::pow(10.0, -0.3)) < 1e-15,
        "a Scope adds db(), the standard sqrt() still works");

  const char* commaLocales[] = {"de_DE.UTF-8", "de_DE.utf8", "de_DE", "German_Germany.1252", "German"};
  for (const char* name : commaLocales)
    if (std::setlocale(LC_NUMERIC, name)) {
      const bool ok = sonore::Expression::parse("0.5 * 3").evaluate() == 1.5 &&
                      sonore::Expression(0.25).toString() == "0.25";
      std::setlocale(LC_NUMERIC, "C");
      check(ok, std::string("under ") + name + ", \"0.5 * 3\" is 1.5 and 0.25 prints as \"0.25\"");
      break;
    }
}

// ── StatisticsAccumulator ──────────────────────────────────────────────────

void testStatistics() {
  std::printf("\nStatisticsAccumulator: against a two-pass computation\n");
  Rng r(4242);
  std::vector<double> data;
  for (int i = 0; i < 100000; ++i) data.push_back((r.uniform() - 0.3) * 50.0 + std::sin(i * 0.01) * 3.0);
  sonore::StatisticsAccumulator<double> acc;
  acc.addValues(data.data(), data.size());
  // Two passes: the mean, then squared deviations from it.
  double sum = 0.0, lo = data[0], hi = data[0], sumSquares = 0.0;
  for (double v : data) {
    sum += v;
    lo = (std::min)(lo, v);
    hi = (std::max)(hi, v);
    sumSquares += v * v;
  }
  const double mean = sum / (double) data.size();
  double dev = 0.0;
  for (double v : data) dev += (v - mean) * (v - mean);
  const double variance = dev / (double) data.size(), sampleVariance = dev / (double) (data.size() - 1);
  const double rms = std::sqrt(sumSquares / (double) data.size());
  auto close = [](double a, double b, double tol) { return std::fabs(a - b) <= tol * (std::max)(1.0, std::fabs(b)); };
  check(acc.getCount() == 100000 && close(acc.getAverage(), mean, 1e-12), "mean of 100,000 values");
  check(close(acc.getVariance(), variance, 1e-11) && close(acc.getSampleVariance(), sampleVariance, 1e-11),
        "population and sample variance");
  check(close(acc.getStandardDeviation(), std::sqrt(variance), 1e-11) &&
            close(acc.getSampleStandardDeviation(), std::sqrt(sampleVariance), 1e-11),
        "standard deviations");
  check(close(acc.getRMS(), rms, 1e-11), "RMS");
  check(acc.getMinValue() == lo && acc.getMaxValue() == hi, "minimum and maximum exactly");

  // A DC offset of 1e9 with a ripple of +-1: the sum-of-squares formula
  // cancels to nothing; Welford does not.
  sonore::StatisticsAccumulator<double> offset;
  std::vector<double> dc;
  for (int i = 0; i < 100000; ++i) dc.push_back(1e9 + (r.uniform() * 2.0 - 1.0));
  double dcSum = 0.0, dcSq = 0.0;
  for (double v : dc) {
    offset.addValue(v);
    dcSum += v;
    dcSq += v * v;
  }
  const double dcMean = dcSum / 1e5;
  double dcDev = 0.0;
  for (double v : dc) dcDev += (v - dcMean) * (v - dcMean);
  const double truth = dcDev / 1e5;
  const double naive = dcSq / 1e5 - dcMean * dcMean;
  std::printf("       variance of 1e9 +- 1: two-pass %.6f, Welford %.6f, sum of squares %.6f\n", truth,
              offset.getVariance(), naive);
  check(close(offset.getVariance(), truth, 1e-6) && std::fabs(naive - truth) > 1e-3,
        "1e9 +- 1: Welford matches two-pass (1/3); the sum-of-squares formula does not");

  // Merging per-block accumulators equals accumulating everything.
  sonore::StatisticsAccumulator<double> merged, block;
  for (size_t i = 0; i < data.size(); ++i) {
    block.addValue(data[i]);
    if (i % 977 == 976 || i + 1 == data.size()) {
      merged += block;
      block.reset();
    }
  }
  merged += sonore::StatisticsAccumulator<double>();
  check(merged.getCount() == acc.getCount() && close(merged.getAverage(), mean, 1e-12) &&
            close(merged.getVariance(), variance, 1e-11) && merged.getMinValue() == lo && merged.getMaxValue() == hi,
        "103 blocks merged with += equal one accumulator over all of it");

  // float in, double inside.
  sonore::StatisticsAccumulator<float> f;
  double fSum = 0.0;
  for (int i = 0; i < 1000000; ++i) {
    const float v = (float) (0.1 + 1e-3 * (i % 7));
    f.addValue(v);
    fSum += (double) v;
  }
  check(std::fabs((double) f.getAverage() - fSum / 1e6) <= 1e-7 * (fSum / 1e6),
        "a million floats: the mean is right to float precision (accumulated in double)");

  sonore::StatisticsAccumulator<double> empty;
  check(empty.getCount() == 0 && empty.getAverage() == 0.0 && empty.getVariance() == 0.0 &&
            empty.getRMS() == 0.0 && std::isinf(empty.getMinValue()) && empty.getMinValue() > 0 &&
            std::isinf(empty.getMaxValue()) && empty.getMaxValue() < 0,
        "empty: zeros, minimum +inf, maximum -inf");
  sonore::StatisticsAccumulator<double> single;
  single.addValue(-3.0);
  check(single.getAverage() == -3.0 && single.getVariance() == 0.0 && single.getSampleVariance() == 0.0 &&
            single.getRMS() == 3.0 && single.getMinValue() == -3.0 && single.getMaxValue() == -3.0,
        "one value: its mean, no variance, RMS of its magnitude");
  sonore::StatisticsAccumulator<double> withNan;
  withNan.addValue(1.0);
  withNan.addValue(std::nan(""));
  withNan.addValue(5.0);
  check(std::isnan(withNan.getAverage()) && withNan.getMinValue() == 1.0 && withNan.getMaxValue() == 5.0,
        "a NaN poisons the mean, but not the minimum and maximum");
  sonore::StatisticsAccumulator<long double> ld;
  ld.addValue(1.0L);
  ld.addValue(3.0L);
  check(ld.getAverage() == 2.0L && ld.getVariance() == 1.0L, "long double works too");
}

// ── LeakedObjectDetector ───────────────────────────────────────────────────

struct Counted {
  int value = 0;
  SONORE_LEAK_DETECTOR(Counted)
};

class PrivateCounted {
public:
  PrivateCounted() = default;

private:
  SONORE_LEAK_DETECTOR(PrivateCounted)
};

struct DoubleDeleted {};
struct Leaky {
  SONORE_LEAK_DETECTOR(Leaky)
};

std::vector<std::string> g_reports;
void captureReport(const char* className, int live, const char* message) {
  g_reports.push_back(std::string(className) + "|" + std::to_string(live) + "|" + message);
}

void testLeakDetector(const char* argv0) {
  std::printf("\nLeakedObjectDetector: counting, reporting, and a process that leaks\n");
  using sonore::LeakedObjectDetector;
  check(LeakedObjectDetector<Counted>::getLiveCount() == 0, "no Counted alive at the start");
  {
    Counted a, b;
    Counted c = a;
    std::unique_ptr<Counted> d(new Counted());
    check(LeakedObjectDetector<Counted>::getLiveCount() == 4, "two made, one copied, one on the heap: 4 alive");
    Counted moved = std::move(c);
    a = b;
    check(LeakedObjectDetector<Counted>::getLiveCount() == 5, "a move is a new object (5), assignment is not");
  }
  check(LeakedObjectDetector<Counted>::getLiveCount() == 0, "all gone: 0");

  sonore::setLeakReportHandler(captureReport);
  PrivateCounted* p = new PrivateCounted();
  const bool reported = LeakedObjectDetector<PrivateCounted>::reportIfLeaked();
  check(reported && g_reports.size() == 1 && contains(g_reports[0], "PrivateCounted|1|") &&
            contains(g_reports[0], "1 instance(s) of class PrivateCounted were never deleted"),
        "reportIfLeaked names the class (declared private) and the count: " +
            (g_reports.empty() ? std::string() : g_reports[0].substr(g_reports[0].rfind('|') + 1, 70)));
  delete p;
  check(!LeakedObjectDetector<PrivateCounted>::reportIfLeaked() && g_reports.size() == 1,
        "nothing alive: nothing reported");

  // A double delete, as the detector sees one: an object destroyed twice. Its
  // destructor touches only the class's counter, never the dead storage.
  alignas(LeakedObjectDetector<DoubleDeleted>) unsigned char storage[sizeof(LeakedObjectDetector<DoubleDeleted>)];
  auto* d = new (storage) LeakedObjectDetector<DoubleDeleted>();
  d->~LeakedObjectDetector();
  d->~LeakedObjectDetector();
  check(g_reports.size() == 2 && contains(g_reports[1], "|-1|") && contains(g_reports[1], "deleted than were created") &&
            LeakedObjectDetector<DoubleDeleted>::getLiveCount() == 0,
        "destroyed twice: reported at once as more deleted than created");
  check(std::string(LeakedObjectDetector<DoubleDeleted>::getClassName()) == "(a class without SONORE_LEAK_DETECTOR)",
        "a detector held without the macro is still counted, under a generic name");
  sonore::setLeakReportHandler(nullptr);

  // The real thing: a child process that leaks a Leaky and exits. The report
  // comes from the counter's destructor during static destruction.
  const std::string out = sonoretest::tempDir() + "/leak_child.txt";
#if defined(_WIN32)
  const std::string command = "\"\"" + std::string(argv0) + "\" --leak-child 2> \"" + out + "\"\"";
#else
  const std::string command = "\"" + std::string(argv0) + "\" --leak-child 2> \"" + out + "\"";
#endif
  const int rc = std::system(command.c_str());
  std::string text;
  if (std::FILE* f = std::fopen(out.c_str(), "rb")) {
    char buffer[1024];
    size_t n;
    while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0) text.append(buffer, n);
    std::fclose(f);
  }
  check(contains(text, "*** Leaked objects detected: 1 instance(s) of class Leaky were never deleted"),
        "a child process that leaks one Leaky reports it at exit (rc " + std::to_string(rc) + ")");
  std::remove(out.c_str());
}

// What the child does: one Leaky that is never destroyed. Placement-new into
// static storage rather than `new`, so a leak sanitizer in the parent's build
// has nothing of its own to report -- the detector is what is under test.
int leakChild() {
  alignas(Leaky) static unsigned char storage[sizeof(Leaky)];
  new (storage) Leaky();
  return 0;
}

// ── UnitTest / UnitTestRunner ──────────────────────────────────────────────

int g_shutdowns = 0;

struct PassingTests : sonore::UnitTest {
  PassingTests() : UnitTest("Passing", "alpha") {}
  void runTest() override {
    beginTest("comparisons");
    expect(true);
    expectEquals(2 + 2, 4);
    expectEquals(std::string("abc"), "abc");
    expectEquals((size_t) 3, 3);
    expectNotEquals(1, 2);
    expectGreaterThan(3.0, 2.0);
    expectLessThan(-1, 0);
    expectGreaterOrEqual(2, 2);
    expectLessOrEqual(2, 2);
    beginTest("tolerances");
    expectWithinAbsoluteError(0.1 + 0.2, 0.3, 1e-15);
    expectWithinAbsoluteError(1.0f, 1.00001f, 1e-4f);
    expectDoesNotThrow([] {});
    expectThrows([] { throw std::runtime_error("x"); });
    expectThrowsType<std::runtime_error>([] { throw std::runtime_error("x"); });
    logMessage("a message from inside a test");
  }
  void shutdown() override { ++g_shutdowns; }
};

struct FailingTests : sonore::UnitTest {
  FailingTests() : UnitTest("Failing", "beta") {}
  void runTest() override {
    beginTest("deliberate failures");
    expectEquals(1 + 1, 3, "arithmetic");
    expectWithinAbsoluteError(1.0, 1.5, 0.1);
    expect(false, "plainly false");
    expectWithinAbsoluteError(std::nan(""), 0.0, 1.0);
    expectThrowsType<std::logic_error>([] { throw std::runtime_error("wrong type"); });
  }
};

struct ThrowingTests : sonore::UnitTest {
  ThrowingTests() : UnitTest("Throwing", "beta") {}
  void runTest() override {
    beginTest("throws midway");
    expect(true);
    throw std::runtime_error("boom");
  }
  void shutdown() override { ++g_shutdowns; }
};

std::vector<uint32_t> g_randomDraws;
struct RandomTests : sonore::UnitTest {
  RandomTests() : UnitTest("Random", "gamma") {}
  void runTest() override {
    beginTest("draws");
    sonore::Random random = getRandom();
    for (int i = 0; i < 4; ++i) g_randomDraws.push_back(random.nextUint32());
    expect(true);
  }
};

PassingTests g_passing;
FailingTests g_failing;
ThrowingTests g_throwing;
RandomTests g_random;

struct QuietRunner : sonore::UnitTestRunner {
  std::vector<std::string> log;
  int abortAfter = -1;
  int started = 0;

protected:
  void logMessage(const std::string& m) override { log.push_back(m); }
  bool shouldAbortTests() override { return abortAfter >= 0 && started++ >= abortAfter; }
};

void testUnitTestFramework() {
  std::printf("\nUnitTest / UnitTestRunner: sample tests that pass, fail and throw\n");
  const std::vector<std::string> categories = sonore::UnitTest::getAllCategories();
  check(categories == std::vector<std::string>{"alpha", "beta", "gamma"} &&
            sonore::UnitTest::getAllTests().size() == 4 && sonore::UnitTest::getTestsInCategory("beta").size() == 2,
        "the registry: four static tests in categories alpha, beta, gamma");

  QuietRunner all;
  all.runAllTests(0x1234);
  const int results = all.getNumResults();
  check(results == 5, "five sections run (" + std::to_string(results) + ")");
  const sonore::UnitTestRunner::TestResult* cmp = all.getResult(0);
  const sonore::UnitTestRunner::TestResult* tol = all.getResult(1);
  const sonore::UnitTestRunner::TestResult* fail = all.getResult(2);
  const sonore::UnitTestRunner::TestResult* thrown = all.getResult(3);
  check(cmp && cmp->unitTestName == "Passing" && cmp->subcategoryName == "comparisons" && cmp->passes == 9 &&
            cmp->failures == 0,
        "Passing / comparisons: 9 passed");
  check(tol && tol->passes == 5 && tol->failures == 0, "Passing / tolerances: 5 passed");
  check(fail && fail->failures == 5 && fail->passes == 0 && fail->messages.size() == 5,
        "Failing: 5 failures recorded");
  if (fail && fail->messages.size() == 5) {
    check(contains(fail->messages[0], "arithmetic -- Expected value: 3, Actual value: 2"),
          "expectEquals' message: \"" + fail->messages[0] + "\"");
    check(contains(fail->messages[1], "Expected value within 0.1 of: 1.5, Actual value: 1"),
          "expectWithinAbsoluteError's message: \"" + fail->messages[1] + "\"");
    check(contains(fail->messages[2], "plainly false") && contains(fail->messages[2], "!!! Test 3 failed"),
          "expect's message, numbered");
    check(contains(fail->messages[4], "another type"), "expectThrowsType names a wrong-typed exception");
  }
  check(thrown && thrown->passes == 1 && thrown->failures == 1 && thrown->messages.size() == 1 &&
            contains(thrown->messages[0], "an exception escaped runTest(): boom"),
        "an exception escaping runTest is a failure with its message, not a crash");
  check(g_shutdowns == 2, "shutdown() ran for both tests that have one, the throwing one included");
  check(all.getTotalFailures() == 6 && all.getTotalPasses() == 16, "totals: 16 passed, 6 failed");
  bool sawSeed = false, sawMessage = false, sawSummary = false;
  for (const std::string& line : all.log) {
    sawSeed = sawSeed || line == "Random seed: 0x1234";
    sawMessage = sawMessage || line == "a message from inside a test";
    sawSummary = sawSummary || line == "6 test failure(s)";
  }
  check(sawSeed && sawMessage && sawSummary, "the log has the seed, the test's own message, and the summary");

  const std::vector<uint32_t> first = g_randomDraws;
  g_randomDraws.clear();
  QuietRunner again;
  again.runTestsInCategory("gamma", 0x1234);
  const std::vector<uint32_t> second = g_randomDraws;
  g_randomDraws.clear();
  QuietRunner other;
  other.runTestsInCategory("gamma", 0x5678);
  check(first.size() == 4 && first == second && g_randomDraws != first && again.getNumResults() == 1,
        "getRandom: the same seed gives the same numbers, another seed others; a category runs alone");

  QuietRunner aborted;
  aborted.abortAfter = 1;
  aborted.runAllTests(1);
  check(aborted.getNumResults() == 2, "shouldAbortTests stops the run after the first test");

  {
    struct Temporary : sonore::UnitTest {
      Temporary() : UnitTest("Temporary", "delta") {}
      void runTest() override {}
    } temporary;
    check(sonore::UnitTest::getAllTests().size() == 5, "a test made later registers itself");
  }
  check(sonore::UnitTest::getAllTests().size() == 4, "and unregisters when destroyed");
}

} // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "--leak-child") == 0) return leakChild();

  const auto t0 = std::chrono::steady_clock::now();
  testXmlKnownDocument();
  testXmlWriting();
  testXmlTypedGetters();
  testXmlChildren();
  testXmlRoundTrips();
  testXmlRefusals();
  testXmlHostile();
  testXmlFiles();
  testLocalisation();
  testTextDiff();
  testExpression();
  testStatistics();
  testLeakDetector(argv[0]);
  testUnitTestFramework();

  std::printf("\n%d checks, %d failed, %.1f s\n", g_checks, g_failures, seconds(t0));
  std::printf(g_failures == 0 ? "CORE DATA TESTS PASSED\n" : "CORE DATA TESTS FAILED\n");
  return g_failures == 0 ? 0 : 1;
}
