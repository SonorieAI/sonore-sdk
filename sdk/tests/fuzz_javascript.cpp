// SPDX-License-Identifier: Apache-2.0
// Mutation fuzzing for JavascriptEngine: the lexer, the parser, the
// interpreter and the built-ins, fed scripts nobody wrote on purpose.
//
// A script in a preset is untrusted input in the most literal sense -- it is
// a program -- so the engine is held to more than a parser is. Every case
// must come back (no crash, no hang, and under ASan no report), and it must
// come back within its time limit and its memory limit: an engine that
// survives a hostile script by taking a minute or a gigabyte has not survived
// it in a plugin.
//
// Each seed is a working program, and the mutations are what turns it into
// something else: every truncation, random byte flips, runs of the
// characters that break a JavaScript grammar (brackets, quotes, backslashes,
// template markers), and splices of fragments that are each a known way to
// hurt an interpreter -- an endless loop, unbounded recursion, a growing
// array, a self-referencing structure, a string doubled until it is huge.
// Splicing produces programs that PARSE and do something new; flips mostly
// produce syntax errors, which is the parser's share of the work.
//
//   fuzz_javascript                      a quick pass (ctest)
//   fuzz_javascript --iterations 3000    the deep pass, under ASan
//
// A watchdog thread turns a hang into a named failure.
#include <sonore/javascript.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <psapi.h>
#if defined(_MSC_VER)
#pragma comment(lib, "psapi.lib")
#endif
#else
#include <sys/resource.h>
#endif

using namespace sonore;

namespace {

const char* const kSeeds[] = {
    // Closures, recursion, arrays.
    R"~(function mk(n) { var c = n; return function () { c += 1; return c; }; }
var f = mk(10), out = [];
for (let i = 0; i < 5; i++) out.push(f() * i);
function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
out.push(fib(12));
out.map(x => x % 7).filter(x => x > 2).reduce((a, b) => a + b, 0);)~",
    // Objects, prototypes, for-in, JSON.
    R"~(function P(x) { this.x = x; this[2] = "two"; }
P.prototype.get = function () { return this.x; };
var p = new P({a: [1, 2, {b: null}]}), keys = [];
for (var k in p) keys.push(k);
var s = JSON.stringify(p, null, 2);
var back = JSON.parse(s);
[keys.join(), back.x.a[2].b === null, p instanceof P, Object.keys(back).length];)~",
    // Strings.
    R"~(var t = "Hello, Wörld \u{1F600}!";
var parts = t.split(" ").map(w => w.toUpperCase() + w.length);
var r = t.replace("o", "0").replaceAll("l", "L").padStart(30, "*").slice(-20);
`${parts.join("|")} ${r} ${t.indexOf("W")} ${t.charCodeAt(13)} ${t.codePointAt(13)}`;)~",
    // Control flow, switch, try.
    R"~(var log = [];
for (var i = 0; i < 20; i++) {
  switch (i % 4) { case 0: log.push("z"); break; case 1: continue; default: log.push(i); }
  try { if (i == 7) throw new RangeError("seven"); if (i == 9) null.x; }
  catch (e) { log.push(e.name); } finally { log.push("f"); }
  if (i > 15) break;
}
var j = 0; do { j += 3; } while (j < 10);
while (j > 0) j -= 2;
log.length + j;)~",
    // Numbers and Math.
    R"~(var xs = [0.1, -2.5, 1e21, 5e-324, NaN, -0, 2 ** 53 + 1, 0xff, 0o7, 0b11, 1_000];
xs.map(x => [x.toFixed(3), x.toPrecision(5), x.toExponential(2), (x | 0), x >>> 1, x.toString(16),
  Math.round(x), Math.max(x, 1), Math.hypot(x, 3), parseFloat(String(x)), parseInt("0x" + x)]);)~",
    // Spread, rest, defaults, optional chaining, nullish, destructured-free.
    R"~(function g(a, b = a * 2, ...rest) { return [a, b, rest.length, arguments.length]; }
var o = {deep: {er: [1, 2, 3]}, fn() { return this.deep?.er?.[1] ?? "none"; }};
[g(1), g(...[4, 5, 6, 7]), o.fn(), o.missing?.thing, o.missing ?? "dflt", {...o.deep, z: 1}];)~",
    // Sorting and arrays.
    R"~(var a = [5, 3, 8, 1, 9, 2];
a.sort((x, y) => y - x); a.splice(2, 2, "s", "t"); a.unshift(0); a.reverse();
var b = a.concat([[1, [2]]]).flat(2); b.length = 6;
[a.indexOf("s"), b.includes(8), Array.from("abc"), new Array(3).fill(7), b.join("-"), a.at(-1)];)~",
    // eval, Function methods, globals.
    R"~(var sum = function () { var t = 0; for (var i = 0; i < arguments.length; i++) t += arguments[i]; return t; };
var bound = sum.bind(null, 1, 2);
var e = eval("var ev = 5; ev * 2");
[sum.apply(null, [1, 2, 3]), bound(3), sum.call(null, 4), e, ev, typeof undefinedThing, globalThis.ev];)~",
    // Something hostile, already: deep recursion and a growing array.
    R"~(var grow = [];
function deep(n) { grow.push(n); return n > 0 ? deep(n - 1) + 1 : 0; }
var d = 0; try { d = deep(100); } catch (e) { d = -1; }
var cyc = {}; cyc.self = cyc; cyc.list = [cyc, grow];
[d, grow.length, String(cyc.list).length];)~",
    // Template literals and nesting.
    R"~(var n = 3, nested = `a${`b${n > 2 ? `c${n}` : "d"}`}e`;
var tags = [1, 2, 3].map(i => `<${i}>${"x".repeat(i)}</${i}>`).join("\n");
nested + tags.length;)~",
    // String search, spreads, a bound chain under new and instanceof, and
    // console.log of shared structure with an Error whose message's
    // toString changes what is being printed (review round 24).
    R"~(var h = "ab".repeat(500) + "abc", F = function () { this.k = 1; };
var G = F; for (var i = 0; i < 50; i++) G = G.bind(null);
var e = new Error(); e.message = {toString: function () { delete w.b; return "m"; }};
var w = {a: e, b: [h, h], c: {d: [1, 2]}};
console.log(w.c, [w.c, w.c], Object.keys(w)); console.log(w);
var lst = [h.indexOf("abc"), h.lastIndexOf("ab"), h.includes("bab"), h.split("ba").length, h.replaceAll("ab", "x").length];
var sp = [...lst, ...lst], ob = {...lst};
[lst.join(), sp.length, Object.keys(ob).length, new G() instanceof F, JSON.parse(JSON.stringify(w.c)).d[1]];)~",
    // Replacement patterns, whose $`, $' and $& copy parts of the subject,
    // and for-in over a string and over an object an index shadows (review
    // round 28).
    R"~(var s = "ab".repeat(300) + "c", r = [];
r.push(s.replace("c", "[$`]").length, s.replaceAll("b", "$&$'").length, s.replace("", "$$|$x").slice(0, 4));
r.push("x".repeat(50).replaceAll("", "$&$&").length, s.replace("ab", function (m, at) { return at + m; }).length);
var n = 0; for (var k in s) { n++; if (n > 40) break; }
var o = Object.create({0: "p", z: 2}); o[0] = 1; o.q = 3; for (var k in o) r.push(k);
r.push(Object.keys("xyz").join(), Object.values([4, 5]).join(), n, JSON.stringify({..."hi"}));
r.join();)~",
};

// Fragments spliced into the seeds: each a known way to stress an interpreter.
const char* const kFragments[] = {
    "while(1){}", "for(;;){}", "do{}while(1);", "function r(){return r()}r();",
    "var A=[];while(1)A.push(A.length+'xxxxxxxxxxxxxxxx');", "var S='ab';while(1)S+=S;",
    "var O={};for(var I=0;;I++)O['k'+I]=I;", "x.y.z", "null.q", "(", ")", "{", "}", "[", "]",
    "`", "${", "\"", "'", "\\", "/*", "*/", "//", "?.", "??", "**", "=>", "...", "new ", "delete ",
    "typeof ", "void ", "in ", "instanceof ", "0x", "1e999", "-0", "NaN", "\\u{110000}", "\\u{",
    "\\x", "JSON.parse(", "JSON.stringify(this)", "eval(", "Function(", "arguments", "this",
    "let ", "const ", "var ", "return ", "break;", "continue;", "throw 1;", "try{", "}catch(e){",
    "}finally{", "switch(1){case 1:", "default:", ";", ",", "a=[a];", "o.o=o;", "Array(1e9)",
    "'x'.repeat(1e9)", "[].concat(new Array(200000).fill(0))", "Object.create(null)",
    "Math.max(...new Array(100000).fill(1))", "\n", "\x00", "\xff", "\xed\xa0\x80", "\xf0\x9f\x98",
    "label:", "class C{}", "async function(){}", "function*g(){}", "get x(){}", "${", "}`",
    "for(var B=0;B<20000;B++)G=G.bind(null);new G();", "[...h,...h,...h]", "({...lst})",
    "console.log(w,w,[w,w]);", "h.repeat(4000).indexOf(h.repeat(2000)+'z')", ".lastIndexOf(", ".split(",
    "throw [w,w,w];", "Object.keys(sp)", "JSON.parse('{\"a\":1,\"b\":[1,2]}')",
};

std::atomic<long long> g_deadlineMs{0};
std::atomic<bool> g_done{false};
std::string g_current;
const char* g_phase = "";
size_t g_case = 0;
size_t g_seedIndex = 0;
long long g_slowestMs = 0;
size_t g_maxEngineBytes = 0;
long g_ok = 0, g_errors = 0;

long long nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void watchdog() {
  while (!g_done.load()) {
    const long long d = g_deadlineMs.load();
    if (d && nowMs() > d) {
      std::printf("  FAIL hang: seed %zu, phase %s, case %zu -- this script did not come back:\n%s\n",
                  g_seedIndex, g_phase, g_case, g_current.c_str());
      std::fflush(stdout);
      std::_Exit(3);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

constexpr size_t kMemoryLimit = (size_t) 24 << 20;

void runCase(const std::string& code, uint32_t variant) {
  g_current = code;
  const long long t0 = nowMs();
  g_deadlineMs.store(t0 + 20000);
  {
    JavascriptEngine js;
    js.maximumExecutionTime = 0.05;
    js.maximumMemoryBytes = kMemoryLimit;
    js.maximumStringLength = (size_t) 1 << 20;
    js.maximumArrayLength = (size_t) 1 << 18;
    js.consoleOutput = [](const std::string&) {};
    JavascriptEngine::Result r;
    JsVar v;
    if (variant % 2) {
      v = js.evaluate(code, &r);
    } else {
      r = js.execute(code);
    }
    (r.wasOk() ? g_ok : g_errors) += 1;
    // What came back is used: converted, measured and written as JSON, with
    // no engine to help.
    if (v.isArray() || v.isObject()) {
      const std::string json = v.toJsonString(variant % 3);
      JsVar again = JsVar::parseJson(json);
      (void) again.toJson();
      (void) v.clone();
    }
    (void) v.toString();
    // And the engine is still usable after whatever the script did.
    if (variant % 5 == 0) {
      js.maximumExecutionTime = 1.0;
      // Room to work in, on top of whatever the script left ALIVE. A script
      // that fills its 24 MB with an object held by a global keeps it: the
      // collector cannot free what is reachable, so "1 + 1" ran out of
      // memory too. Whether a case gets there depends on the machine -- the
      // 50 ms limit and the 24 MB one race -- which is why it surfaced on one
      // CI run and not the one before. The time limit is lifted here for the
      // same reason.
      js.maximumMemoryBytes = js.getMemoryUsage() + kMemoryLimit;
      JavascriptEngine::Result r2;
      const JsVar two = js.evaluate("1 + 1", &r2);
      if (!r2.wasOk() || two.toInt() != 2) {
        std::printf("  FAIL the engine did not recover after:\n%s\n%s\n", code.c_str(),
                    r2.getDescription().c_str());
        std::_Exit(4);
      }
      js.callFunction("f", {1, "x"});
    }
    const size_t used = js.getMemoryUsage();
    if (used > g_maxEngineBytes) g_maxEngineBytes = used;
  }
  // The text itself as JSON too, for the reader's sake.
  std::string error;
  (void) JsVar::parseJson(code, &error);
  g_deadlineMs.store(0);
  const long long took = nowMs() - t0;
  if (took > g_slowestMs) g_slowestMs = took;
}

size_t fuzzSeed(const std::string& seed, size_t iterations, uint32_t rngSeed) {
  std::mt19937 rng(rngSeed);
  size_t cases = 0;
  g_phase = "seed";
  g_case = 0;
  runCase(seed, 0);
  runCase(seed, 1);
  cases += 2;

  g_phase = "truncate";
  const size_t step = seed.size() > 200 ? seed.size() / 200 : 1;
  for (size_t len = 0; len < seed.size(); len += step) {
    g_case = len;
    runCase(seed.substr(0, len), (uint32_t) len);
    ++cases;
  }

  g_phase = "flip";
  for (size_t it = 0; it < iterations; ++it) {
    g_case = it;
    std::string b = seed;
    const int flips = 1 + (int) (rng() % 6);
    for (int k = 0; k < flips; ++k) b[rng() % b.size()] = (char) rng();
    runCase(b, rng());
    ++cases;
  }

  g_phase = "runs";
  static const char kBreakers[] = "{}()[]\"'`\\/*;,.$?:=+-<>!&|0123456789eE\n";
  for (size_t it = 0; it < iterations; ++it) {
    g_case = it;
    std::string b = seed;
    const size_t at = rng() % b.size();
    const size_t len = 1 + rng() % 12;
    const char c = kBreakers[rng() % (sizeof(kBreakers) - 1)];
    for (size_t k = 0; k < len && at + k < b.size(); ++k) b[at + k] = (rng() % 4 == 0) ? (char) rng() : c;
    runCase(b, rng());
    ++cases;
  }

  g_phase = "splice";
  const size_t fragments = sizeof(kFragments) / sizeof(kFragments[0]);
  for (size_t it = 0; it < iterations * 2; ++it) {
    g_case = it;
    std::string b = seed;
    const int n = 1 + (int) (rng() % 3);
    for (int k = 0; k < n; ++k) {
      size_t at = rng() % (b.size() + 1);
      // Mostly at a statement boundary, so the fragment runs rather than
      // making a syntax error of whatever token it landed in.
      if (rng() % 4 != 0) {
        const size_t boundary = b.find_first_of(";\n", at);
        at = boundary == std::string::npos ? b.size() : boundary + 1;
      }
      b.insert(at, kFragments[rng() % fragments]);
    }
    if (rng() % 4 == 0 && b.size() > 8) {
      // A slice of the program copied elsewhere in it.
      const size_t from = rng() % b.size();
      const size_t len = 1 + rng() % (std::min)((size_t) 60, b.size() - from);
      b.insert(rng() % b.size(), b.substr(from, len));
    }
    runCase(b, rng());
    ++cases;
  }

  g_phase = "mixed";
  for (size_t it = 0; it < iterations / 2; ++it) {
    g_case = it;
    std::string b = seed.substr(0, rng() % seed.size());
    b.insert(rng() % (b.size() + 1), kFragments[rng() % fragments]);
    for (int k = 0; k < 2 && !b.empty(); ++k) b[rng() % b.size()] = (char) rng();
    runCase(b, rng());
    ++cases;
  }
  return cases;
}

size_t peakResidentBytes() {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS counters;
  if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
    return (size_t) counters.PeakWorkingSetSize;
  return 0;
#else
  struct rusage usage;
  if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
#if defined(__APPLE__)
  return (size_t) usage.ru_maxrss; // bytes on macOS
#else
  return (size_t) usage.ru_maxrss * 1024; // kilobytes on Linux
#endif
#endif
}

} // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  size_t iterations = 40; // the ctest pass: seconds, not minutes
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--iterations" && i + 1 < argc) iterations = (size_t) std::atol(argv[++i]);
    else {
      std::fprintf(stderr, "usage: fuzz_javascript [--iterations N]\n");
      return 2;
    }
  }
  std::printf("Sonore SDK JavaScript fuzz -- %zu iterations per phase\n", iterations);
  std::thread dog(watchdog);
  size_t total = 0;
  const auto t0 = std::chrono::steady_clock::now();
  const size_t seeds = sizeof(kSeeds) / sizeof(kSeeds[0]);
  for (size_t i = 0; i < seeds; ++i) {
    g_seedIndex = i;
    const size_t n = fuzzSeed(kSeeds[i], iterations, 0x5EED0000u + (uint32_t) i);
    std::printf("  ok   seed %zu: %zu cases came back\n", i, n);
    total += n;
  }
  g_done = true;
  dog.join();
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const size_t peak = peakResidentBytes();
  std::printf("%zu cases in %.1f s: %ld ran, %ld ended in an error; slowest %lld ms; most an engine held "
              "%.1f MB (limit %.0f MB); process peak %.0f MB\n",
              total, seconds, g_ok, g_errors, g_slowestMs, g_maxEngineBytes / 1048576.0,
              kMemoryLimit / 1048576.0, peak / 1048576.0);
  // An engine may run past its limit by one allocation before the check that
  // stops it; twice the limit would mean the meter is not doing its job.
  const bool memoryOk = g_maxEngineBytes <= 2 * kMemoryLimit;
  if (!memoryOk) std::printf("  FAIL an engine held more than twice its memory limit\n");
  std::printf(memoryOk ? "JAVASCRIPT FUZZ PASSED\n" : "JAVASCRIPT FUZZ FAILED\n");
  return memoryOk ? 0 : 1;
}
