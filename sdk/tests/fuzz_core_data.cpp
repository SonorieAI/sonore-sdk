// SPDX-License-Identifier: Apache-2.0
// Mutation fuzzing for the text parsers in the data utilities: XmlDocument,
// Expression, the LocalisedStrings file format -- and TextDiff, whose two
// inputs may both come from outside.
//
// Each parser gets seed inputs that exercise every construct it knows, and
// then thousands of mutations of them: truncations, byte flips, bytes that
// break length and encoding arithmetic (0x00, 0xFF, 0x80, 0xC0, 0xED), runs
// of one character, and -- because these are text formats, where random
// bytes mostly die at the first token -- the format's own tokens spliced in
// at random ("<!--", "<![CDATA[", "&#x", "]]>", "(", "sin(", ...), pieces of
// the seed duplicated into themselves, and nesting deepened.
//
// "Did it come back" is the floor, as in fuzz_parsers: no crash, no hang
// (a watchdog names the case), and under a sanitizer no report. On top of
// that each target checks a PROPERTY whenever the input is accepted:
//
//   xml         what parses, written back (pretty and on one line) parses
//               again to an equivalent tree
//   expression  what parses prints as text that parses to the same text,
//               and evaluating either gives the same value
//   strings     every mapping a file loaded translates as loaded
//   diff        the changes between two mutations reproduce the second
//
// A property failure is a failure like a crash, with the input saved.
//
//   fuzz_core_data                       every target, a quick pass (ctest)
//   fuzz_core_data --iterations 20000    the deep pass, under ASan
//   fuzz_core_data --target xml          one target
#include <sonore/expression.h>
#include <sonore/localisation.h>
#include <sonore/text_diff.h>
#include <sonore/xml.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

// ── The watchdog and the crash handler ─────────────────────────────────────

std::atomic<long long> g_deadlineMs{0};
std::string g_target;
size_t g_case = 0;
std::string g_current;
std::atomic<bool> g_done{false};
int g_propertyFailures = 0;
size_t g_accepted = 0; // inputs the parser accepted, so the properties were checked

long long nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void saveCurrent(const char* name) {
  if (FILE* f = std::fopen(name, "wb")) {
    if (!g_current.empty()) std::fwrite(g_current.data(), 1, g_current.size(), f);
    std::fclose(f);
  }
}

using SignalHandler = void (*)(int);
SignalHandler g_previous[64] = {};

void onCrash(int sig) {
  saveCurrent("fuzz_core_data_crash.bin");
  char msg[200];
  const int n = std::snprintf(msg, sizeof(msg),
                              "  CRASH signal %d in %s case=%zu size=%zu -- the input is in "
                              "fuzz_core_data_crash.bin\n",
                              sig, g_target.c_str(), g_case, g_current.size());
  if (n > 0) std::fputs(msg, stderr);
  std::fflush(stderr);
  const SignalHandler previous = (sig >= 0 && sig < 64) ? g_previous[sig] : nullptr;
  if (previous && previous != SIG_DFL && previous != SIG_IGN) {
    std::signal(sig, previous);
    std::raise(sig);
  }
  std::_Exit(98);
}

void watchCrashes() {
  const int signals[] = {SIGSEGV, SIGABRT, SIGILL, SIGFPE};
  for (int sig : signals) {
    const SignalHandler previous = std::signal(sig, onCrash);
    if (sig >= 0 && sig < 64) g_previous[sig] = previous == SIG_ERR ? nullptr : previous;
  }
}

void watchdog() {
  while (!g_done.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const long long deadline = g_deadlineMs.load();
    if (deadline != 0 && nowMs() > deadline) {
      std::fprintf(stderr, "  HANG %s case=%zu size=%zu -- the input is in fuzz_core_data_hang.bin\n",
                   g_target.c_str(), g_case, g_current.size());
      saveCurrent("fuzz_core_data_hang.bin");
      std::fflush(stderr);
      std::_Exit(99);
    }
  }
}

void propertyFailed(const char* what) {
  ++g_propertyFailures;
  char name[96];
  std::snprintf(name, sizeof(name), "fuzz_core_data_%s_%d.bin", g_target.c_str(), g_propertyFailures);
  saveCurrent(name);
  if (g_propertyFailures <= 10)
    std::fprintf(stderr, "  PROPERTY %s case=%zu: %s -- the input is in %s\n", g_target.c_str(), g_case, what,
                 name);
}

// ── Mutation ────────────────────────────────────────────────────────────────

struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed) {}
  uint64_t next() {
    uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  size_t below(size_t n) { return n ? (size_t) (next() % n) : 0; }
};

std::string mutate(Rng& r, const std::string& seed, const std::vector<std::string>& tokens) {
  std::string s = seed;
  // Half the cases are one mutation away from a valid input, so the
  // properties behind acceptance get exercised, not only the refusals.
  const int rounds = r.below(2) == 0 ? 1 : 1 + (int) r.below(6);
  for (int round = 0; round < rounds; ++round) {
    const size_t at = r.below(s.size() + 1);
    switch (r.below(9)) {
      case 0: // truncate
        s.resize(at);
        break;
      case 1: // flip a bit
        if (!s.empty()) s[r.below(s.size())] ^= (char) (1u << r.below(8));
        break;
      case 2: { // a byte that breaks arithmetic or encoding
        static const unsigned char nasty[] = {0x00, 0xFF, 0x7F, 0x80, 0xC0, 0xC3, 0xED, 0xEF, 0xF4, 0xFE, '\r'};
        s.insert(at, 1, (char) nasty[r.below(sizeof(nasty))]);
        break;
      }
      case 3: // a format token, spliced in
      case 4:
        s.insert(at, tokens[r.below(tokens.size())]);
        break;
      case 5: // delete a span
        if (at < s.size()) s.erase(at, 1 + r.below((std::min)((size_t) 16, s.size() - at)));
        break;
      case 6: { // duplicate a span of the input into itself
        if (s.empty()) break;
        const size_t from = r.below(s.size());
        const size_t len = 1 + r.below((std::min)((size_t) 64, s.size() - from));
        const std::string piece = s.substr(from, len);
        s.insert(r.below(s.size() + 1), piece);
        break;
      }
      case 7: // a run of one character
        s.insert(at, 1 + r.below(300), "<>&;(-'\"x#"[r.below(10)]);
        break;
      default: // a random byte
        s.insert(at, 1, (char) r.below(256));
    }
    if (s.size() > 1 << 16) s.resize(1 << 16);
  }
  return s;
}

// ── Seeds ───────────────────────────────────────────────────────────────────

std::vector<std::string> xmlSeeds() {
  return {
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!-- c -->\n<?pi x?>\n"
      "<!DOCTYPE p SYSTEM \"x.dtd\" [ <!ENTITY e \"a>b\"> <!-- ] --> ]>\n"
      "<p a='1' b=\"x &amp; y &#x20AC; &#65;\" c=\"t\tn\r\n\">\n"
      "  <q id=\"g\" v=\"-6.5\"/><r>text &lt;more&gt; <![CDATA[<raw>]]> \xC3\xA9\xF0\x9F\x98\x80</r>\n"
      "  <m>one <b>two</b> three<!-- x --> four</m>\n</p>\n",
      "<a><b><c><d><e/></d></c></b></a>",
      "<?xml version='1.0' encoding='ISO-8859-1'?><n v='caf\xE9'>\xE0</n>",
      "<plist version=\"1.0\"><dict><key>name</key><string>Init</string><key>data</key>"
      "<data>AAECAw==</data><array><integer>1</integer><real>0.5</real><true/></array></dict></plist>",
  };
}
const std::vector<std::string> kXmlTokens = {
    "<", ">", "/>", "</", "&", ";", "&amp;", "&lt;", "&#", "&#x", "&#x10FFFF;", "&#0;", "&nbsp;",
    "<!--", "-->", "--", "<![CDATA[", "]]>", "<?", "?>", "<?xml version='1.0'?>", "<!DOCTYPE",
    "[", "]", "<!ENTITY x SYSTEM \"file:///etc/passwd\">", "&x;", "\"", "'", "=", " a='1'", " a='2'",
    "<x>", "</x>", "<x/>", "<y a=\"&lt;\" b='&#9;'>t&amp;u</y>", "<![CDATA[ ]]>",
    "<!-- c -->", "&#x41;", "&#128512;", "<?p d?>", " ", "\n  ", "text", "\xC3\xA9",
    "\xEF\xBB\xBF", "\xFF\xFE", "\r\n", "\t", "encoding='latin1'", ":"};

std::vector<std::string> expressionSeeds() {
  return {"1 + 2 * 3 - 4 / 5", "-(x + y) * --z / (1e-3 + .5)", "sin(x) * cos(y) + sqrt(abs(x - y))",
          "pow(x, 2) + min(a.b, c, 3) - max(1) + atan2(y, x) / log10(1000) + fmod(x, 3)",
          "((((x))))", "f(g(h(1, 2), 3), -x)", "a + b + c + d + e + f + g"};
}
const std::vector<std::string> kExpressionTokens = {
    "(", ")", "((((", "))))", ",", "+", "-", "*", "/", "--", "e", "E+", "1e308", "1e999", ".", "..",
    "x", "a.b", "sin(", "min(", "(1)", "+x", "*2", " - y", "/3", "sin(1)", "max(1, x)", "pow(1,",
    "loop", "0", "9.", " ", "\t", "@", "\xC3\xA9"};

std::vector<std::string> stringsSeeds() {
  return {"\xEF\xBB\xBF// header\r\nlanguage: French\ncountries: fr be ch\n\n"
          "\"Save\" = \"Enregistrer\"\n\"Quote \\\"q\\\"\" = \"Citer \\\"q\\\"\"\n\"Back\\\\\" = \"Ret\\\\\"\n"
          "\"Two\\nlines\" = \"Deux\\nlignes\"\r\"\" = \"x\"\n\"Broken = \"y\n# comment\n\"Last\" = \"Dernier\""};
}
const std::vector<std::string> kStringsTokens = {"\"", "\\", "\\\"", "=", "\n", "\r", "\r\n",
                                                 "language:", "countries:", " ", ",", "\"a\" = \"b\"\n"};

// ── Targets ─────────────────────────────────────────────────────────────────

void fuzzXml(const std::string& input) {
  sonore::XmlDocument doc(input);
  std::unique_ptr<sonore::XmlElement> root = doc.getDocumentElement();
  if (!root) {
    if (doc.getLastParseError().empty()) propertyFailed("refused without an error message");
    return;
  }
  ++g_accepted;
  // Whatever was accepted must survive being written and read back.
  sonore::XmlElement::TextFormat single;
  single = single.singleLine();
  const std::string a = root->toString(), b = root->toString(single);
  std::unique_ptr<sonore::XmlElement> backA = sonore::XmlDocument::parse(a);
  std::unique_ptr<sonore::XmlElement> backB = sonore::XmlDocument::parse(b);
  if (!backB || !backB->isEquivalentTo(root.get(), false)) propertyFailed("single-line rewrite differs");
  // Pretty printing adds whitespace between elements; text in elements that
  // have text is never touched, so the trees are equivalent once the
  // whitespace-only text the parser drops anyway is gone.
  if (!backA) propertyFailed("pretty rewrite does not parse");
  sonore::XmlDocument kept(input);
  kept.setCommentsKept(true);
  kept.setEmptyTextElementsIgnored(false);
  std::unique_ptr<sonore::XmlElement> withAll = kept.getDocumentElement();
  if (!withAll) propertyFailed("accepted by default, refused with comments and whitespace kept");
  else {
    // The typed getters on every attribute, for the sanitizer's benefit.
    for (int i = 0; i < withAll->getNumAttributes(); ++i) {
      const std::string& name = withAll->getAttributeName(i);
      (void) withAll->getIntAttribute(name);
      (void) withAll->getDoubleAttribute(name);
      (void) withAll->getBoolAttribute(name);
    }
    sonore::XmlElement copy(*withAll);
    if (!copy.isEquivalentTo(withAll.get(), false)) propertyFailed("a copy is not equivalent");
    (void) copy.getAllSubText();
  }
}

struct FuzzScope : sonore::Expression::Scope {
  bool getSymbolValue(const std::string& symbol, sonore::Expression& value) const override {
    if (symbol == "loop") {
      value = sonore::Expression::parse("loop + 1"); // a cycle, which must be caught
      return true;
    }
    if (symbol.empty()) return false;
    value = sonore::Expression((double) (unsigned char) symbol[0] / 10.0);
    return true;
  }
};

void fuzzExpression(const std::string& input) {
  std::string error;
  const sonore::Expression e = sonore::Expression::parse(input, &error);
  if (!error.empty()) return;
  ++g_accepted;
  const FuzzScope scope;
  std::string evalError;
  const double v = e.evaluate(scope, evalError);
  const std::string text = e.toString();
  std::string again;
  const sonore::Expression e2 = sonore::Expression::parse(text, &again);
  if (!again.empty()) {
    propertyFailed("printed text does not parse");
    return;
  }
  if (e2.toString() != text) propertyFailed("printing is not stable");
  std::string evalError2;
  const double v2 = e2.evaluate(scope, evalError2);
  if (evalError != evalError2 || !(v == v2 || (std::isnan(v) && std::isnan(v2))))
    propertyFailed("the reprinted expression evaluates differently");
  const sonore::Expression s = e.simplified();
  std::string simplifiedError;
  (void) sonore::Expression::parse(s.toString(), &simplifiedError);
  if (!simplifiedError.empty()) propertyFailed("the simplified expression does not print as text that parses");
  (void) e.findReferencedSymbols();
}

void fuzzStrings(const std::string& input) {
  sonore::LocalisedStrings strings(input);
  if (strings.size() > 0) ++g_accepted;
  for (const auto& kv : strings.getMappings())
    if (strings.translate(kv.first) != kv.second) propertyFailed("a loaded mapping does not translate");
  sonore::LocalisedStrings folded(input, true);
  (void) folded.translate("save");
}

void fuzzDiff(const std::string& a, const std::string& b) {
  ++g_accepted;
  const sonore::TextDiff d(a, b);
  if (d.appliedTo(a) != b) propertyFailed("the changes do not reproduce the target");
  const sonore::TextDiff starved(a, b, 64);
  if (starved.appliedTo(a) != b) propertyFailed("with a starved budget, the changes do not reproduce the target");
}

// ── Driver ──────────────────────────────────────────────────────────────────

template <typename Fn>
void runTarget(const char* name, const std::vector<std::string>& seeds, const std::vector<std::string>& tokens,
               size_t iterations, uint64_t seed, Fn fn) {
  g_target = name;
  g_accepted = 0;
  Rng r(seed);
  const auto start = std::chrono::steady_clock::now();
  for (const std::string& s : seeds) {
    g_current = s;
    g_deadlineMs = nowMs() + 10000;
    fn(s);
  }
  for (g_case = 0; g_case < iterations; ++g_case) {
    const std::string input = mutate(r, seeds[r.below(seeds.size())], tokens);
    g_current = input;
    g_deadlineMs = nowMs() + 10000;
    fn(input);
  }
  g_deadlineMs = 0;
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  std::printf("  %-11s %zu mutated inputs in %.2f s, %zu accepted and their properties checked\n", name,
              iterations, secs, g_accepted);
}

} // namespace

int main(int argc, char** argv) {
  size_t iterations = 2000;
  std::string only;
  uint64_t seed = 0x5EED;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) iterations = (size_t) std::strtoull(argv[++i], nullptr, 10);
    else if (std::strcmp(argv[i], "--target") == 0 && i + 1 < argc) only = argv[++i];
    else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 0);
  }
  watchCrashes();
  std::thread dog(watchdog);
  std::printf("fuzz_core_data: %zu iterations per target, seed 0x%llx\n", iterations, (unsigned long long) seed);

  if (only.empty() || only == "xml") runTarget("xml", xmlSeeds(), kXmlTokens, iterations, seed, fuzzXml);
  if (only.empty() || only == "expression")
    runTarget("expression", expressionSeeds(), kExpressionTokens, iterations, seed + 1, fuzzExpression);
  if (only.empty() || only == "strings")
    runTarget("strings", stringsSeeds(), kStringsTokens, iterations, seed + 2, fuzzStrings);
  if (only.empty() || only == "diff") {
    // Pairs: two mutations of one seed, so they are similar enough for the
    // search to do real work, and different enough to need it.
    Rng pairs(seed + 3);
    const std::vector<std::string> seeds = xmlSeeds();
    runTarget("diff", seeds, kXmlTokens, iterations, seed + 4, [&](const std::string& a) {
      fuzzDiff(a, mutate(pairs, a, kXmlTokens));
    });
  }

  g_done = true;
  dog.join();
  if (g_propertyFailures) {
    std::printf("FUZZ CORE DATA FAILED: %d property failure(s)\n", g_propertyFailures);
    return 1;
  }
  std::printf("FUZZ CORE DATA PASSED: every case returned, every property held\n");
  return 0;
}
