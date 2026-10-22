// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: JavascriptEngine, JsVar and DynamicObject.
//
// The table of scripts below is plain ECMAScript, and every expected value in
// it was checked against node (the values are what node prints, not what this
// engine was believed to print). The engine is also run against node on
// thousands of generated programs; that harness lives outside the repository
// because it needs node, and this test must not.
//
// Beyond the language: the reference's additions, errors and their line
// numbers, native objects called from script and script functions called from
// C++, and the limits -- a loop that never ends, recursion that never ends,
// nesting a thousand deep, a script that eats memory, and stop() from another
// thread -- each of which must come back as a Result, promptly, and leave the
// engine usable.
#include <sonore/javascript.h>

// MSVC's Debug runtime gives every standard container a proxy object for
// iterator debugging, so the same script holds several times the memory it
// does in Release, and a check sized against maximumMemoryBytes ran out of
// memory first in CI's Windows-Debug job. Those checks get this much more
// room there; what they test (depth, string and array limits) is unchanged.
#if defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL > 0
static constexpr size_t kDebugRoom = 4;
#else
static constexpr size_t kDebugRoom = 1;
#endif

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
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

static int g_checks = 0;
static int g_failures = 0;

/** The process's peak resident memory in bytes, 0 where it cannot be read:
 *  what the engine allocated past its meter shows here and nowhere else. */
static size_t peakResidentBytes() {
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

static void check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) ++g_failures;
  std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

/** A value as the table writes it: strings in single quotes, arrays and
 *  objects as JSON, everything else as JavaScript prints it. */
static std::string show(const JsVar& v) {
  if (v.isString()) return "'" + v.toString() + "'";
  if (v.isArray() || v.isObject()) return v.toJsonString();
  if (v.isFunction()) return "function";
  return v.toString();
}

struct Case {
  const char* code;
  const char* expected;
};

// ── Standard ECMAScript, verified against node ─────────────────────────────

static const Case kLanguage[] = {
    // Arithmetic, precedence and numbers as JavaScript prints them.
    {R"~(1 + 2 * 3)~", R"~(7)~"},
    {R"~((1 + 2) * 3)~", R"~(9)~"},
    {R"~(2 ** 3 ** 2)~", R"~(512)~"},
    {R"~((-2) ** 2)~", R"~(4)~"},
    {R"~(10 % 3 + -7 % 3)~", R"~(0)~"},
    {R"~(7 / 2)~", R"~(3.5)~"},
    {R"~([1 / 0, -1 / 0, 0 / 0])~", R"~([null,null,null])~"},
    {R"~(String(1 / 0) + String(0 / 0))~", R"~('InfinityNaN')~"},
    {R"~(0.1 + 0.2)~", R"~(0.30000000000000004)~"},
    {R"~(String(1e21) + " " + String(1e-7) + " " + String(123e-20) + " " + String(2 ** 53))~",
     R"~('1e+21 1e-7 1.23e-18 9007199254740992')~"},
    {R"~(String(100 / 3) + " " + String(-0) + " " + String(0.000001) + " " + String(5e-324))~",
     R"~('33.333333333333336 0 0.000001 5e-324')~"},
    {R"~(String(2 ** -24) + " " + String(1 / 3) + " " + String(123456789012345680000))~",
     R"~('5.960464477539063e-8 0.3333333333333333 123456789012345680000')~"},
    {R"~(5 - -3 + +"4")~", R"~(12)~"},
    {R"~(0x1F + 0o17 + 0b101 + 1_000)~", R"~(1051)~"},
    {R"~(010 + 09)~", R"~(17)~"},
    // Coercions.
    {R"~("3" + 4 + 5)~", R"~('345')~"},
    {R"~(3 + 4 + "5")~", R"~('75')~"},
    {R"~("3" * "4" - "2")~", R"~(10)~"},
    {R"~([true + 1, null + 1, undefined + 1])~", R"~([2,1,null])~"},
    {R"~([] + [])~", R"~('')~"},
    {R"~([] + {})~", R"~('[object Object]')~"},
    {R"~([1, 2] + [3])~", R"~('1,23')~"},
    {R"~([1 < 2 < 3, 3 > 2 > 1, "10" < "9", 10 < 9, "b" > "a"])~", R"~([true,false,true,false,true])~"},
    {R"~([null == undefined, null === undefined, NaN == NaN, 0 == "", "0" == false, [1] == 1, null == 0])~",
     R"~([true,false,false,true,true,true,false])~"},
    {R"~([1 === 1.0, "1" !== 1, {} === {}, 0 === -0])~", R"~([true,true,false,true])~"},
    {R"~([typeof null, typeof [], typeof function () {}, typeof undeclaredName, typeof undefined])~",
     R"~(["object","object","function","undefined","undefined"])~"},
    {R"~([Number(""), Number("  12  "), Number("12px"), Number("0b101"), Number(null), Number([]), Number([7]), Number("1e3")])~",
     R"~([0,12,null,5,0,0,7,1000])~"},
    {R"~([String(null), String(undefined), String(true), String([1, [2, 3]]), String({})])~",
     R"~(["null","undefined","true","1,2,3","[object Object]"])~"},
    {R"~([!!"", !!"0", !!0, !!NaN, !![], !!{}, !!null])~", R"~([false,true,false,false,true,true,false])~"},
    // Bitwise operators and ToInt32.
    {R"~([~5, 5 & 3, 5 | 3, 5 ^ 3, 1 << 31, -1 >>> 0, -16 >> 2, -16 >>> 28])~",
     R"~([-6,1,7,6,-2147483648,4294967295,-4,15])~"},
    {R"~([2 ** 32 | 0, 4294967297 | 0, 1.9 | 0, -1.9 | 0, NaN | 0, 2 ** 31 | 0])~",
     R"~([0,1,1,-1,0,-2147483648])~"},
    // Logical operators, short circuits, conditional.
    {R"~([0 || "x", 1 && 2, null ?? 5, 0 ?? 5, "" || null || "last"])~", R"~(["x",2,5,0,"last"])~"},
    {R"~(var c = 0; false && c++; true || c++; null ?? c++; c)~", R"~(1)~"},
    {R"~(var q = 5; q > 3 ? "big" : "small")~", R"~('big')~"},
    // Assignment, compound assignment, ++ and --.
    {R"~(var a = 5; a += 2; a *= 3; a -= 1; a /= 4; a %= 3; a **= 3; a)~", R"~(8)~"},
    {R"~(var i = 1; var j = i++ + ++i; [i, j])~", R"~([3,4])~"},
    {R"~(var s = "a"; s += 1; s += null; s)~", R"~('a1null')~"},
    {R"~(var o = {n: 1}; o.n++; o.n += 10; o["n"] *= 2; --o.n; o.n)~", R"~(23)~"},
    {R"~(var u; u ??= 4; u ||= 9; u &&= u + 1; u)~", R"~(5)~"},
    {R"~(var m = 6; m <<= 2; m >>= 1; m |= 1; m ^= 3; m &= 14; m)~", R"~(14)~"},
    // var, let, const, hoisting, scopes.
    {R"~(f(); function f() { return 1; } typeof f)~", R"~('function')~"},
    {R"~(var h = typeof hv; var hv = 1; h)~", R"~('undefined')~"},
    {R"~(let x = 1; { let x = 2; } x)~", R"~(1)~"},
    {R"~(var y = 1; { var y = 2; } y)~", R"~(2)~"},
    {R"~(const k = [1]; k.push(2); k)~", R"~([1,2])~"},
    {R"~(var out = []; for (var i = 0; i < 3; i++) out.push(i); i)~", R"~(3)~"},
    // Closures and recursion.
    {R"~(function mk() { var c = 0; return function () { return ++c; }; } var f1 = mk(), g1 = mk(); f1(); f1(); g1(); [f1(), g1()])~",
     R"~([3,2])~"},
    {R"~(var fs = []; for (let i = 0; i < 3; i++) fs.push(() => i); fs.map(f => f()))~", R"~([0,1,2])~"},
    {R"~(var gs = []; for (var i = 0; i < 3; i++) gs.push(() => i); gs.map(f => f()))~", R"~([3,3,3])~"},
    {R"~(var hs = []; for (const k of ["a", "b"]) hs.push(() => k); hs.map(f => f()))~", R"~(["a","b"])~"},
    {R"~(function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); } fib(20))~", R"~(6765)~"},
    {R"~(const fact = n => n <= 1 ? 1 : n * fact(n - 1); fact(20))~", R"~(2432902008176640000)~"},
    {R"~(function even(n) { return n == 0 ? true : odd(n - 1); } function odd(n) { return n == 0 ? false : even(n - 1); } [even(10), odd(7)])~",
     R"~([true,true])~"},
    {R"~(var adders = [1, 2, 3].map(function (n) { return function (x) { return x + n; }; }); adders[2](10))~", R"~(13)~"},
    {R"~((function f(n) { return n ? n + f(n - 1) : 0; })(4))~", R"~(10)~"},
    // Functions: defaults, rest, spread, arguments, this, call/apply/bind.
    {R"~(function d(a, b = 2, ...r) { return [a, b, r]; } [d(1), d(1, undefined, 3, 4)])~", R"~([[1,2,[]],[1,2,[3,4]]])~"},
    {R"~(Math.max(...[1, 5, 3], 2))~", R"~(5)~"},
    {R"~([..."ab", ...[1, [2]]])~", R"~(["a","b",1,[2]])~"},
    {R"~(function cnt() { return arguments.length; } cnt(1, 2, 3))~", R"~(3)~"},
    {R"~(var obj = {v: 4, get() { return this.v; }}; [obj.get(), obj.get.call({v: 9})])~", R"~([4,9])~"},
    {R"~(function sum3(a, b, c) { return a + b + c; } [sum3.apply(null, [1, 2, 3]), sum3.bind(null, 10)(1, 2)])~", R"~([6,13])~"},
    {R"~(var named = function () {}; var arrow = () => 1; [named.name, arrow.name, (function (a, b) {}).length])~",
     R"~(["named","arrow",2])~"},
    // Control flow.
    {R"~(var r = []; for (var i = 0; i < 10; i++) { if (i % 2) continue; if (i > 6) break; r.push(i); } r)~", R"~([0,2,4,6])~"},
    {R"~(var w = 0, n = 0; while (true) { if (++w > 5) break; n += w; } n)~", R"~(15)~"},
    {R"~(var dw = 10; do { dw++; } while (dw < 5); dw)~", R"~(11)~"},
    {R"~(function sw(v) { var r = []; switch (v) { case 1: r.push("one"); case 2: r.push("two"); break; case "3": r.push("three"); break; default: r.push("other"); } return r; } [sw(1), sw(2), sw(3), sw("3")])~",
     R"~([["one","two"],["two"],["other"],["three"]])~"},
    {R"~(if (0) "a"; else if ("") "b"; else "c")~", R"~('c')~"},
    // for-in and property order: integer keys ascending, then insertion order.
    {R"~(var keys = []; for (var k in {b: 1, 2: 1, a: 1, 1: 1, "-1": 1}) keys.push(k); keys)~", R"~(["1","2","b","a","-1"])~"},
    {R"~(Object.keys({z: 1, 10: 2, 9: 3, y: 4}))~", R"~(["9","10","z","y"])~"},
    {R"~(var ks = []; for (var k in [7, 8]) ks.push(k); for (var k2 in "hi") ks.push(k2); ks)~", R"~(["0","1","0","1"])~"},
    {R"~(function P() { this.own = 1; } P.prototype.inherited = 2; var pk = []; for (var k in new P()) pk.push(k); pk)~",
     R"~(["own","inherited"])~"},
    {R"~(var dk = {a: 1, b: 2, c: 3}, seen = []; for (var k in dk) { delete dk.b; seen.push(k); } seen)~", R"~(["a","c"])~"},
    // for-of.
    {R"~(var fo = []; for (const x of [1, "two", null]) fo.push(x); for (const ch of "h\u00e9\u{1F600}") fo.push(ch.length); fo)~",
     R"~([1,"two",null,1,1,2])~"},
    // try / catch / finally / throw.
    {R"~(var t = []; try { t.push(1); null.x; t.push(2); } catch (e) { t.push(e.name); } finally { t.push("f"); } t)~",
     R"~([1,"TypeError","f"])~"},
    {R"~((function () { try { return "try"; } finally { return "finally"; } })())~", R"~('finally')~"},
    {R"~((function () { try { throw 42; } catch (e) { return e + 1; } })())~", R"~(43)~"},
    {R"~(var e1; try { throw new RangeError("bad"); } catch (e) { e1 = [e.name, e.message, String(e), e instanceof RangeError, e instanceof Error]; } e1)~",
     R"~(["RangeError","bad","RangeError: bad",true,true])~"},
    {R"~(var ce; try { undefinedFunction(); } catch (e) { ce = e.name; } ce)~", R"~('ReferenceError')~"},
    {R"~(var nest = []; try { try { throw 1; } finally { nest.push("inner"); } } catch (e) { nest.push(e); } nest)~", R"~(["inner",1])~"},
    // Optional chaining and nullish.
    {R"~(var on = null; [on?.a.b.c, on?.[1], on?.(), ({a: {b: 5}})?.a?.b])~", R"~([null,null,null,5])~"},
    // Objects, prototypes, new, instanceof, in, delete.
    {R"~(function Pt(x, y) { this.x = x; this.y = y; } Pt.prototype.len = function () { return Math.hypot(this.x, this.y); }; var p = new Pt(3, 4); [p.len(), p instanceof Pt, "x" in p, "len" in p, p.hasOwnProperty("len")])~",
     R"~([5,true,true,true,false])~"},
    {R"~(var ob = {a: 1, b: 2}; delete ob.a; ["a" in ob, Object.keys(ob)])~", R"~([false,["b"]])~"},
    {R"~(var base = {greet() { return "hi " + this.n; }}; var child = Object.create(base); child.n = "c"; [child.greet(), Object.getPrototypeOf(child) === base])~",
     R"~(["hi c",true])~"},
    {R"~(var k2 = "dyn"; var lit = {[k2 + 1]: 1, short: 2, "quoted key": 3, 4: 4}; lit.short = k2; lit)~",
     R"~({"4":4,"dyn1":1,"short":"dyn","quoted key":3})~"},
    {R"~(Object.assign({a: 1}, {b: 2}, null, {a: 3}))~", R"~({"a":3,"b":2})~"},
    {R"~([Object.values({x: 1, y: "z"}), Object.entries({p: 1})])~", R"~([[1,"z"],[["p",1]]])~"},
    {R"~(var sp = {...{a: 1, b: 2}, b: 3}; sp)~", R"~({"a":1,"b":3})~"},
    {R"~(({valueOf() { return 7; }}) * 2 + ({toString() { return "s"; }}) + "")~", R"~('14s')~"},
    // Strings: lengths and indices in UTF-16 units.
    {R"~(["abc".length, "\u00e9".length, "\u{1F600}".length, "\uD83D\uDE00" === "\u{1F600}"])~", R"~([3,1,2,true])~"},
    {R"~(["\u00e9".charCodeAt(0), "\u{1F600}".charCodeAt(0), "\u{1F600}".codePointAt(0), "A".codePointAt(0)])~",
     R"~([233,55357,128512,65])~"},
    {R"~("hello world".substring(6) + "|" + "hello".substring(4, 1) + "|" + "hello".slice(-3) + "|" + "hello".substr(1, 3))~",
     R"~('world|ell|llo|ell')~"},
    {R"~(["abcabc".indexOf("c"), "abcabc".lastIndexOf("c"), "abc".indexOf("z"), "abc".includes("bc"), "abc".startsWith("ab"), "abc".endsWith("bc")])~",
     R"~([2,5,-1,true,true,true])~"},
    {R"~(["a,b,,c".split(","), "abc".split(""), "a-b-c".split("-", 2), "".split(","), "x".split()])~",
     R"~([["a","b","","c"],["a","b","c"],["a","b"],[""],["x"]])~"},
    {R"~(["abc".charAt(1), "abc".charAt(5), "Hello".toUpperCase(), "Hello".toLowerCase(), "  t  ".trim() + "|"])~",
     R"~(["b","","HELLO","hello","t|"])~"},
    {R"~(["5".padStart(3, "0"), "ab".padEnd(5, "xy"), "ab".repeat(3), "a-b-c".replace("-", "+"), "a-b-c".replaceAll("-", "+")])~",
     R"~(["005","abxyx","ababab","a+b-c","a+b+c"])~"},
    {R"~(["abc".replace("b", "[$&]"), "abc".replace("b", "$`$'"), "abc".replace("b", (m, i) => m.toUpperCase() + i)])~",
     R"~(["a[b]c","aacc","aB1c"])~"},
    {R"~(`a${1 + 1}b${"c"}${[1, 2]}` + `
x`)~", R"~('a2bc1,2
x')~"},
    {R"~("tab\there\nq\"\\\x41\u0042\103")~", R"~('tab	here
q"\ABC')~"},
    {R"~(String.fromCharCode(72, 105) + "." + ["\u00e9\u00df".toUpperCase(), "\u03a9\u0416".toLowerCase()])~",
     R"~('Hi.ÉSS,ωж')~"},
    {R"~(["b", "a", "C", "\u00e9", "B"].sort())~", R"~(["B","C","a","b","é"])~"},
    // Arrays.
    {R"~(var ar = [1, 2]; ar.push(3, 4); var popped = ar.pop(); [ar, popped, ar.length])~", R"~([[1,2,3],4,3])~"},
    {R"~(var sh = [1, 2, 3]; sh.shift(); sh.unshift(0, 0.5); sh)~", R"~([0,0.5,2,3])~"},
    {R"~(var spl = [1, 2, 3, 4, 5]; var removed = spl.splice(1, 2, "a", "b", "c"); [spl, removed])~",
     R"~([[1,"a","b","c",4,5],[2,3]])~"},
    {R"~([[1, 2, 3].slice(1), [1, 2, 3].slice(-2, -1), [1, 2].concat([3], 4, [[5]]), [3, 1, 2].reverse()])~",
     R"~([[2,3],[2],[1,2,3,4,[5]],[2,1,3]])~"},
    {R"~([[1, 2, 3].join("-"), [1, null, undefined, 2].join(), [[1, 2], [3]].toString()])~", R"~(["1-2-3","1,,,2","1,2,3"])~"},
    {R"~([[1, 2, 3].indexOf(2), [1, 2, 3].indexOf("2"), [NaN].indexOf(NaN), [NaN].includes(NaN), [1, 2, 1].lastIndexOf(1)])~",
     R"~([1,-1,-1,true,2])~"},
    {R"~([[1, 2, 3].map(x => x * 2), [1, 2, 3, 4].filter(x => x % 2), [1, 2, 3].reduce((a, b) => a + b), [1, 2, 3].reduce((a, b) => a + b, 10)])~",
     R"~([[2,4,6],[1,3],6,16])~"},
    {R"~([[5, 6].find(x => x > 5), [5, 6].findIndex(x => x > 9), [1, 2].some(x => x > 1), [1, 2].every(x => x > 1), ["a", "b"].reduceRight((a, b) => a + b)])~",
     R"~([6,-1,true,false,"ba"])~"},
    {R"~([[10, 9, 1, 100].sort(), [10, 9, 1, 100].sort((a, b) => a - b), [3, undefined, 1].sort()])~",
     R"~([[1,10,100,9],[1,9,10,100],[1,3,null]])~"},
    {R"~(var st = [{k: 1, v: "a"}, {k: 0, v: "b"}, {k: 1, v: "c"}, {k: 0, v: "d"}]; st.sort((x, y) => x.k - y.k).map(e => e.v).join(""))~",
     R"~('bdac')~"},
    {R"~(var tr = [1, 2, 3, 4]; tr.length = 2; tr.push(9); [tr, new Array(3).fill(0), Array.isArray(tr), Array.of(7), Array.from("ab")])~",
     R"~([[1,2,9],[0,0,0],true,[7],["a","b"]])~"},
    {R"~([[1, [2, [3, [4]]]].flat(), [1, [2, [3, [4]]]].flat(Infinity), [1, 2].flatMap(x => [x, x * 10]), [1, 2, 3].at(-1)])~",
     R"~([[1,2,[3,[4]]],[1,2,3,4],[1,10,2,20],3])~"},
    {R"~(var self = [1]; self.push(self); String(self))~", R"~('1,')~"},
    // Math.
    {R"~([Math.max(1, 3, 2), Math.min(), Math.max(), Math.max(1, NaN), Math.abs(-3), Math.sign(-3), Math.trunc(-4.7)])~",
     R"~([3,null,null,null,3,-1,-4])~"},
    {R"~([Math.round(2.5), Math.round(-2.5), Math.round(0.49999999999999994), Math.round(-0.4), Math.floor(-1.5), Math.ceil(1.2)])~",
     R"~([3,-2,0,0,-2,2])~"},
    {R"~(String(Math.round(-0.4)) + (1 / Math.round(-0.4)))~", R"~('0-Infinity')~"},
    {R"~([Math.sqrt(16), Math.pow(2, 10), Math.hypot(3, 4), Math.cbrt(27), Math.clz32(1), Math.imul(0xffffffff, 5), Math.fround(5.5)])~",
     R"~([4,1024,5,3,31,-5,5.5])~"},
    {R"~([Math.PI, Math.E, Math.LN2, Math.SQRT2, Math.LOG10E])~",
     R"~([3.141592653589793,2.718281828459045,0.6931471805599453,1.4142135623730951,0.4342944819032518])~"},
    {R"~([Math.pow(1, Infinity), Math.pow(NaN, 0), 2 ** -1, (-8) ** (1 / 3)])~", R"~([null,1,0.5,null])~"},
    {R"~(var rnd = Math.random(); rnd >= 0 && rnd < 1)~", R"~(true)~"},
    // Number methods and parsing.
    {R"~([(1234.5678).toFixed(2), (1.005).toFixed(2), (2.5).toFixed(0), (0.5).toFixed(0), (-1.5).toFixed(0), (-0.0001).toFixed(2), (1e21).toFixed(2)])~",
     R"~(["1234.57","1.00","3","1","-2","-0.00","1e+21"])~"},
    {R"~([(123.456).toPrecision(4), (0.00001234).toPrecision(2), (1e21).toPrecision(3), (123456).toPrecision(2), (0).toPrecision(3)])~",
     R"~(["123.5","0.000012","1.00e+21","1.2e+5","0.00"])~"},
    {R"~([(123.456).toExponential(2), (0).toExponential(), (1e-7).toExponential(), (255).toString(16), (255).toString(2), (-255).toString(36)])~",
     R"~(["1.23e+2","0e+0","1e-7","ff","11111111","-73"])~"},
    {R"~([parseInt("ff", 16), parseInt("0x1A"), parseInt("12px"), parseInt(""), parseInt("  -42"), parseInt("101", 2), parseInt("z", 37)])~",
     R"~([255,26,12,null,-42,5,null])~"},
    {R"~([parseFloat("3.14abc"), parseFloat(".5"), parseFloat("-Infinityx"), parseFloat("e5"), parseFloat("1e3x")])~",
     R"~([3.14,0.5,null,null,1000])~"},
    {R"~(String(parseFloat("-Infinityx")))~", R"~('-Infinity')~"},
    {R"~([isNaN("abc"), isNaN("12"), isFinite("1e400"), Number.isInteger(5.0), Number.isInteger("5"), Number.isNaN("x"), Number.MAX_SAFE_INTEGER])~",
     R"~([true,false,false,true,false,false,9007199254740991])~"},
    // JSON.
    {R"~(JSON.stringify({b: [1, "x", null, true], a: {c: undefined, d: function () {}}, 1: NaN}))~",
     R"~('{"1":null,"b":[1,"x",null,true],"a":{}}')~"},
    {R"~(JSON.stringify([undefined, function () {}, "\t\n\"", -0]))~", R"~('[null,null,"\t\n\"",0]')~"},
    {R"~(JSON.stringify({a: [1, {b: 2}], e: []}, null, 2))~", R"~('{
  "a": [
    1,
    {
      "b": 2
    }
  ],
  "e": []
}')~"},
    {R"~(JSON.stringify({a: 1, b: 2, c: 3}, ["c", "a"]) + JSON.stringify({a: 1, b: "x"}, (k, v) => typeof v === "number" ? v * 10 : v))~",
     R"~('{"c":3,"a":1}{"a":10,"b":"x"}')~"},
    {R"~(JSON.stringify({toJSON() { return {t: 1}; }}) + JSON.stringify("\ud800"))~", R"~('{"t":1}"\ud800"')~"},
    {R"~(var parsed = JSON.parse('{"z": 1, "a": [1, 2, {"b": null}], "s": "\\u00e9\\n", "n": -1.5e2}'); [Object.keys(parsed), parsed.a[2].b, parsed.s.length, parsed.n])~",
     R"~([["z","a","s","n"],null,2,-150])~"},
    {R"~(var jt = []; for (var bad of ["{a:1}", "[1,]", "01", "\"\\x\"", "", "[1] x"]) { try { JSON.parse(bad); jt.push("ok"); } catch (e) { jt.push(e.name); } } jt)~",
     R"~(["SyntaxError","SyntaxError","SyntaxError","SyntaxError","SyntaxError","SyntaxError"])~"},
    {R"~(var cyc = {}; cyc.me = cyc; var cy; try { JSON.stringify(cyc); } catch (e) { cy = e.name; } cy)~", R"~('TypeError')~"},
    // eval, at global scope.
    {R"~(eval("1 + 1") + eval("var evaled = 3; evaled * 2") + evaled)~", R"~(11)~"},
    {R"~(var se; try { eval("var = 1"); } catch (e) { se = e.name; } se)~", R"~('SyntaxError')~"},
    // Comments, ASI, the completion value of a program.
    {R"~(// a comment
var asi = 1 /* inline */
var asi2 = asi
+ 1
asi2)~", R"~(2)~"},
    {R"~(function noAsi() { return
  42; } noAsi())~", R"~(undefined)~"},
    {R"~(var cv = 1; if (cv) { "then"; } else { "else"; })~", R"~('then')~"},
};

// ── The reference's additions (not in ECMAScript, so not checked in node) ──

static const Case kExtensions[] = {
    {R"~([Math.sqr(3), Math.range(15, 0, 10), Math.range(-1, 0, 10), Math.toDegrees(Math.PI), Math.toRadians(180)])~",
     R"~([9,10,0,180,3.141592653589793])~"},
    {R"~(var ri = Math.randInt(3, 6); ri >= 3 && ri < 6 && ri === Math.floor(ri))~", R"~(true)~"},
    {R"~(var ca = [1, 2, 3, 2]; ca.remove(2); [ca, ca.contains(3), ca.contains(2)])~", R"~([[1,3],true,false])~"},
    {R"~(charToInt("A") + charToInt("\u00e9"))~", R"~(298)~"},
    // evaluate() reads a lone object literal as an expression, as the
    // reference's evaluate does -- not as a block, as eval would.
    {R"~({a: 1, b: [2]})~", R"~({"a":1,"b":[2]})~"},
};

static void runTable(const Case* cases, size_t count, const char* title) {
  std::printf("%s\n", title);
  int failed = 0;
  for (size_t i = 0; i < count; ++i) {
    JavascriptEngine js;
    JavascriptEngine::Result r;
    JsVar v = js.evaluate(cases[i].code, &r);
    const std::string got = r.failed() ? "ERROR " + r.getDescription() : show(v);
    if (got != cases[i].expected) {
      ++failed;
      std::printf("    script:   %s\n    expected: %s\n    got:      %s\n", cases[i].code,
                  cases[i].expected, got.c_str());
    }
  }
  char what[160];
  std::snprintf(what, sizeof(what), "%zu scripts give the values node gives (%d wrong)", count, failed);
  if (cases == kExtensions)
    std::snprintf(what, sizeof(what), "%zu scripts using the reference's additions (%d wrong)", count, failed);
  check(failed == 0, what);
}

// ── Errors, with where they happened ────────────────────────────────────────

static void errors() {
  std::printf("errors and their lines\n");
  struct ErrorCase {
    const char* code;
    const char* message; // the start of it
    int line;
  };
  const ErrorCase cases[] = {
      {"var a = 1;\nvar b = 2;\nnull.x;", "TypeError: Cannot read properties of null (reading 'x')", 3},
      {"var a = 1;\nvar = 2;", "SyntaxError: Unexpected token '='", 2},
      {"function f() {\n  return g();\n}\nf();", "ReferenceError: g is not defined", 2},
      {"\n\n\nthrow new Error('boom');", "Error: boom", 4},
      {"var o = {};\n\no.method();", "TypeError: o.method is not a function", 3},
      {"var s = 'unterminated;\nvar t = 1;", "SyntaxError: Invalid or unexpected token", 1},
      {"const c = 1;\nc = 2;", "TypeError: Assignment to constant variable.", 2},
      {"x;\nlet x = 1;", "ReferenceError: Cannot access 'x' before initialization", 1},
      {"var n = 1;\nnew n();", "TypeError: n is not a constructor", 2},
      {"[1, 2,\n 3, 4", "SyntaxError: Unexpected end of input", 2},
      {"throw 'just a string';", "Uncaught just a string", 1},
      {"-2 ** 2", "SyntaxError: Unary operator used immediately before exponentiation", 1},
      {"outer: for (;;) break outer;", "SyntaxError: labels are not supported", 1},
      {"var o = { get x() { return 1; } };", "SyntaxError: getters and setters are not supported", 1},
      {"function f() {\n  [].reduce((a, b) => a);\n}\nf();", "TypeError: Reduce of empty array with no initial value", 2},
      {"var a = [];\na.length = -1;", "RangeError: Invalid array length", 2},
      {"return 5;", "SyntaxError: Illegal return statement", 1},
  };
  int failed = 0;
  for (const ErrorCase& c : cases) {
    JavascriptEngine js;
    const JavascriptEngine::Result r = js.execute(c.code);
    const bool ok = r.failed() && r.getErrorMessage().compare(0, std::strlen(c.message), c.message) == 0 &&
                    r.getLine() == c.line;
    if (!ok) {
      ++failed;
      std::printf("    script: %s\n    expected line %d: %s\n    got: %s\n", c.code, c.line, c.message,
                  r.getDescription().c_str());
    }
  }
  check(failed == 0, "17 failing scripts report the right error at the right line");

  JavascriptEngine js;
  JavascriptEngine::Result r;
  const JsVar v = js.evaluate("1 +", &r);
  check(v.isUndefined() && r.failed() && r.getDescription().find("Line 1") == 0,
        "evaluate() of broken code returns undefined and describes the error");
  check(js.execute("var fine = 1;").wasOk() && js.evaluate("fine + 1").toInt() == 2,
        "an engine that reported an error carries on");
}

// ── Native objects, callFunction ────────────────────────────────────────────

static void natives() {
  std::printf("native objects and calls from C++\n");
  JavascriptEngine js;
  auto plugin = std::make_shared<DynamicObject>();
  plugin->setProperty("name", "Saturator");
  plugin->setProperty("gain", 0.5);
  int calls = 0;
  plugin->setMethod("add", [&calls](const JsVar::NativeFunctionArgs& a) {
    ++calls;
    return JsVar(a[0].toDouble() + a[1].toDouble());
  });
  plugin->setMethod("fail", [](const JsVar::NativeFunctionArgs&) -> JsVar {
    throw std::runtime_error("the native side said no");
  });
  JsVar stored;
  plugin->setMethod("store", [&stored](const JsVar::NativeFunctionArgs& a) {
    stored = a[0];
    return JsVar();
  });
  js.registerNativeObject("plugin", plugin);
  js.registerNativeFunction("twice", [](const JsVar::NativeFunctionArgs& a) {
    return JsVar(a[0].toDouble() * 2);
  });

  check(js.evaluate("plugin.add(2, 3) + twice(4)").toDouble() == 13.0 && calls == 1,
        "a script calls a native method and a native function");
  check(js.evaluate("plugin.name + ' at ' + plugin.gain").toString() == "Saturator at 0.5",
        "a script reads a native object's properties");
  js.execute("plugin.gain = plugin.gain * 2; plugin.mode = 'warm';");
  check(plugin->getProperty("gain").toDouble() == 1.0 && plugin->getProperty("mode").toString() == "warm",
        "what a script writes, C++ reads back");
  check(js.evaluate("var m; try { plugin.fail(); } catch (e) { m = e.message; } m").toString() ==
            "the native side said no",
        "a native function's exception reaches the script as a catchable Error");
  check(js.evaluate("typeof plugin.add + ' ' + plugin.hasOwnProperty('add')").toString() == "function true",
        "native methods are functions to the script");

  // Script functions called from C++.
  check(js.execute("function sum(a, b) { return a + b; }\n"
                   "const scale = (x) => x * plugin.gain;\n"
                   "var counter = 0; function bump() { return ++counter; }")
            .wasOk(),
        "a script defines functions");
  JavascriptEngine::Result r;
  check(js.callFunction("sum", {1, 2}, &r).toDouble() == 3.0 && r.wasOk(), "callFunction(sum, 1, 2) is 3");
  check(js.callFunction("sum", {"a", JsVar::array({1, 2})}).toString() == "a1,2",
        "callFunction passes strings and arrays");
  check(js.callFunction("scale", {10}).toDouble() == 10.0, "callFunction reaches a top-level const arrow function");
  js.callFunction("bump");
  js.callFunction("bump");
  check(js.callFunction("bump").toInt() == 3, "state persists between calls");
  js.callFunction("nothingHere", {}, &r);
  check(r.failed() && r.getErrorMessage() == "TypeError: nothingHere is not a function",
        "calling a missing function is a Result, not a crash");

  // A callback handed to C++ and called later.
  js.execute("plugin.store(function (x) { return 'got ' + x + ' via ' + this.tag; });");
  auto self = std::make_shared<DynamicObject>();
  self->setProperty("tag", "callback");
  check(stored.isFunction() &&
            js.callFunctionObject(JsVar(self), stored, {7}).toString() == "got 7 via callback",
        "a script's callback is called from C++ with a chosen this");

  // Globals and lexical bindings persist across executes.
  js.execute("let persistent = 41;");
  check(js.evaluate("persistent + 1").toInt() == 42, "a top-level let persists to the next call");
  check(js.getRootObject()->getProperty("counter").toInt() == 3, "a global var lives on the root object");

  // console.log, through the hook.
  std::vector<std::string> lines;
  js.consoleOutput = [&lines](const std::string& line) { lines.push_back(line); };
  js.execute("console.log('a', 1, [1, 'b', {c: null}], {k: 'v'}, -0, undefined); trace('t');");
  check(lines.size() == 2 && lines[0] == "a 1 [ 1, 'b', { c: null } ] { k: 'v' } -0 undefined" && lines[1] == "t",
        "console.log and trace write node-style lines to the hook");
}

// ── The C++ side of values ──────────────────────────────────────────────────

static void values() {
  std::printf("JsVar, DynamicObject and the JSON bridge\n");
  JsVar n = 3, d = 2.5, s = "text", b = true, u, z = nullptr;
  check(n.isInt() && n.isNumber() && d.isDouble() && s.isString() && b.isBool() && u.isUndefined() && z.isNull(),
        "kinds from C++ values");
  check(JsVar(1e10).toInt() == 2147483647 && JsVar(-1e10).toInt() == -2147483647 - 1 &&
            JsVar(std::nan("")).toInt() == 0 && JsVar("42").toInt() == 42,
        "toInt saturates rather than wrapping, and parses strings");
  check(JsVar(1) == JsVar(1.0) && !(JsVar("1") == JsVar(1)) && JsVar("1").looselyEquals(JsVar(1)) &&
            !(JsVar(std::nan("")) == JsVar(std::nan(""))),
        "== is strict equality; looselyEquals is JavaScript's ==");
  check(std::string(JsVar(nullptr).typeName()) == "object" && std::string(JsVar("x").typeName()) == "string",
        "typeName is typeof");

  JsVar arr = JsVar::array({1, "two", JsVar::array({3})});
  arr.append(4);
  check(arr.size() == 4 && arr[1].toString() == "two" && arr.toString() == "1,two,3,4" && arr[9].isUndefined(),
        "arrays from C++");

  auto o = std::make_shared<DynamicObject>();
  o->setProperty("b", 1);
  o->setProperty("10", 2);
  o->setProperty("a", JsVar::array({1, 2}));
  o->setProperty("2", 3);
  const std::vector<std::string> names = o->getPropertyNames();
  check(names.size() == 4 && names[0] == "2" && names[1] == "10" && names[2] == "b" && names[3] == "a",
        "DynamicObject keeps JavaScript's property order");
  check(o->removeProperty("b") && !o->hasProperty("b") && !o->removeProperty("b"), "removeProperty");

  // A deep clone keeps shared structure shared and a cycle a cycle.
  auto shared = JsVar::array({1});
  o->setProperty("x", shared);
  o->setProperty("y", shared);
  o->setProperty("me", JsVar(o));
  DynamicObject::Ptr c = o->clone();
  check(c != o && c->getProperty("x").getDynamicObject() == c->getProperty("y").getDynamicObject() &&
            c->getProperty("x").getDynamicObject() != shared.getDynamicObject() &&
            c->getProperty("me").getDynamicObject() == c.get(),
        "clone() is deep, keeps sharing, and keeps the cycle");
  o->removeProperty("me");
  c->removeProperty("me");

  // JSON both ways.
  std::string error;
  JsVar parsed = JsVar::parseJson(R"({"z":1,"a":[true,null,"\u00e9"],"n":{"m":-0.5}})", &error);
  check(error.empty() && parsed.toJsonString() == R"({"z":1,"a":[true,null,"é"],"n":{"m":-0.5}})",
        "parseJson then toJsonString round-trips, in order");
  check(JsVar::parseJson("{\"a\":", &error).isUndefined() && !error.empty(), "parseJson reports bad JSON");
  JsonValue jv = parsed.toJson();
  check(jv["a"].at(2).asString() == "\xC3\xA9" && jv["n"]["m"].asNumber() == -0.5 && jv["z"].asInt() == 1,
        "toJson gives a JsonValue");
  JsVar back = JsVar::fromJson(JsonValue::parse(R"({"b":[1,2],"a":"x"})"));
  check(back["a"].toString() == "x" && back["b"].size() == 2, "fromJson takes one");
  JavascriptEngine js;
  js.registerNativeObject("cfg", back.getObject());
  check(js.evaluate("cfg.b[1] * 10 + cfg.a.length").toInt() == 21, "JSON-built values are usable from script");

  // A value outlives the engine that made it.
  JsVar kept;
  {
    JavascriptEngine temp;
    kept = temp.evaluate("var r = ['str' + 1, {k: [1, 2]}, function () { return 1; }]; r");
  }
  check(kept.size() == 3 && kept[0].toString() == "str1" && kept[1]["k"].size() == 2 && kept[2].isFunction(),
        "values stay valid after their engine is destroyed");

  // An identifier of invalid UTF-8 bytes becomes a property key and a
  // function name. The engine stores strings canonically, so its reported
  // UTF-16 length must match what it decodes -- otherwise indexing it read out
  // of bounds. Each stray byte decodes to one U+FFFD, so eight bytes is eight
  // units, not sixteen.
  {
    JavascriptEngine jsid;
    const std::string ident(8, '\xFF');
    JavascriptEngine::Result rid;
    JsVar lenAndChar = jsid.evaluate(
        "var o = {}; o." + ident + " = 1; var k = Object.keys(o)[0]; k.length + ':' + k[12]", &rid);
    check(rid.wasOk() && lenAndChar.toString() == "8:undefined",
          "an invalid-UTF-8 identifier key has a correct length and never reads out of bounds");
    JsVar nameLen = jsid.evaluate("(function " + ident + "() {}).name.length", &rid);
    check(rid.wasOk() && nameLen.toInt() == 8,
          "an invalid-UTF-8 function name has a correct UTF-16 length");
  }
}

// ── Limits ──────────────────────────────────────────────────────────────────

static double secondsSince(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

static void limits() {
  std::printf("limits\n");
  {
    JavascriptEngine js;
    js.maximumExecutionTime = 0.2;
    const char* loops[] = {"while (true) {}", "for (;;) {}", "do {} while (1);",
                           "var i = 0; while (true) { try { for (;;) i++; } catch (e) {} }",
                           "function f() { for (;;) {} } [1, 2].map(f);",
                           "var a = [3, 1, 2]; a.sort(function () { for (;;) {} });"};
    int good = 0;
    for (const char* code : loops) {
      const auto t0 = std::chrono::steady_clock::now();
      const JavascriptEngine::Result r = js.execute(code);
      const double took = secondsSince(t0);
      if (r.failed() && r.getErrorMessage() == "Execution timed-out" && took >= 0.19 && took < 1.5) ++good;
      else std::printf("    %s -> %s in %.2f s\n", code, r.getDescription().c_str(), took);
    }
    check(good == 6, "six endless loops each fail with \"Execution timed-out\" after maximumExecutionTime, "
                     "try/catch or not");
    js.maximumExecutionTime = 15;
    check(js.evaluate("1 + 1").toInt() == 2, "and the engine runs the next script normally");
  }
  {
    JavascriptEngine js;
    JavascriptEngine::Result r;
    js.evaluate("function down(n) { return n == 0 ? 0 : 1 + down(n - 1); } down(200)", &r);
    check(r.wasOk(), "recursion 200 deep is fine");
    js.evaluate("down(1000000)", &r);
    check(r.failed() && r.getErrorMessage() == "Maximum call stack size exceeded",
          "recursion a million deep is refused with \"Maximum call stack size exceeded\"");
    js.evaluate("var c = 0; function f() { try { return f(); } catch (e) { c++; return f(); } } f()", &r);
    check(r.failed() && r.getErrorMessage() == "Maximum call stack size exceeded",
          "and a script cannot catch its way past it");
    js.maximumRecursionDepth = 50;
    js.evaluate("down(60)", &r);
    check(r.failed(), "maximumRecursionDepth is honoured");
    js.maximumRecursionDepth = 1000;
    js.evaluate("down(40)", &r);
    check(r.wasOk(), "and the engine carries on");

    // A long chain of bound functions unwraps by recursion in construct() and
    // instanceof; without a stack check it overflowed the native stack. Now it
    // ends in the stack limit, and the engine stays usable. A separate engine
    // with room to build the chain (each bind is its own object). Where the
    // thread's stack has room for the whole chain (Linux's 8 MB main thread
    // does; Windows' 1 MB does not) it is simply constructed.
    {
      JavascriptEngine jsb;
      jsb.maximumMemoryBytes = (size_t) 1 << 30;
      JavascriptEngine::Result rb;
      jsb.execute("var bf = function () {}; for (var i = 0; i < 8000; i++) bf = bf.bind(null);");
      jsb.evaluate("new bf()", &rb);
      if (rb.failed() && rb.getErrorMessage() != "Maximum call stack size exceeded")
        std::printf("    %s\n", rb.getDescription().c_str());
      check(rb.wasOk() || rb.getErrorMessage() == "Maximum call stack size exceeded",
            "constructing a deep bound chain ends in the stack limit or a result, not a crash");
      const JsVar inst = jsb.evaluate("({}) instanceof bf", &rb);
      check(rb.wasOk() && !inst.toBool(),
            "instanceof over a deep bound chain is iterative: it returns, no crash");
      jsb.execute("bf = null;");
      check(jsb.evaluate("1 + 1").toInt() == 2, "and the engine still works after a bound-chain overflow");
    }

    // String search is linear: a naive search of 'a' x 2^21 for 'a' x (2^20 - 1)
    // then 'b' compares a trillion characters (most of a minute per call,
    // which neither stop() nor the time limit could end), and splitting a
    // non-ASCII string re-decoded it for every piece.
    {
      JavascriptEngine jss;
      jss.execute("var h = 'a'.repeat(1 << 21); var n = 'a'.repeat((1 << 20) - 1) + 'b';"
                  "var p = h.slice(0, 1500000) + n + h.slice(1500000); var u = '\xC3\xA9,'.repeat(200000);");
      const auto t0 = std::chrono::steady_clock::now();
      JavascriptEngine::Result rs;
      const JsVar v = jss.evaluate(
          "[h.indexOf(n), h.includes(n), h.lastIndexOf(n), h.split(n).length, h.replaceAll(n, 'x').length,"
          " p.indexOf(n), p.lastIndexOf(n), p.split(n).length, u.split(',').length].join(' ')",
          &rs);
      const double took = secondsSince(t0);
      check(rs.wasOk() && v.toString() == "-1 false -1 1 2097152 1500000 1500000 2 200001" && took < 3,
            "worst-case indexOf, includes, lastIndexOf, split and replaceAll on 2 MB are linear and correct");
      if (!rs.wasOk() || took >= 3) std::printf("    %s in %.2f s\n", v.toString().c_str(), took);
    }

    // A replacement's $`, $' and $& each copy part of the subject, and the
    // piece was made in full before its length was looked at: "$`" x 1000
    // after a megabyte asked for 2 GB (3.3 GB at the peak) under a 64 MB
    // limit, and took 2.5 s to be refused. A $& of an empty match copies
    // nothing but costs its step at every match, with no clock read: 24 s
    // past a time limit of 1 s.
    {
      JavascriptEngine jsr;
      JavascriptEngine::Result rr;
      check(jsr.evaluate("'abc'.replace('b', '[$$|$&|$`|$\\'|$x]')", &rr).toString() == "a[$|b|a|c|$x]c",
            "replace's $$, $&, $` and $' say what they say");
      jsr.execute("var big = 'a'.repeat(1000000) + 'b';");
      auto t0 = std::chrono::steady_clock::now();
      jsr.evaluate("big.replace('b', '$`'.repeat(1000))", &rr);
      double took = secondsSince(t0);
      check(rr.failed() && rr.getErrorMessage().find("RangeError: Invalid string length") == 0 && took < 1,
            "a replacement of a thousand $` after a megabyte is refused as it grows, not after 2 GB");
      if (took >= 1) std::printf("    %s in %.2f s\n", rr.getDescription().c_str(), took);
      jsr.maximumExecutionTime = 0.3;
      t0 = std::chrono::steady_clock::now();
      jsr.evaluate("'x'.repeat(1000).replaceAll('', '$&'.repeat(2000000))", &rr);
      took = secondsSince(t0);
      check(rr.failed() && rr.getErrorMessage() == "Execution timed-out" && took < 3,
            "and a million empty $& at each of a thousand matches end at the time limit");
      if (took >= 3) std::printf("    %s in %.2f s\n", rr.getDescription().c_str(), took);
    }

    // for-in, and Object.keys, values, entries and spread, made every index
    // key of a string or an array before the first was used, as std::strings
    // no meter saw: a for-in over an 8 MB string held 500 MB of keys under a
    // 64 MB limit even when it stopped at its third, and Object.values of it
    // 745 MB before it was refused. for-in now makes each index key as it
    // reaches it; the others charge the keys before making them.
    {
      JavascriptEngine jsk;
      jsk.execute("var big = 'x'.repeat(8000000); var o = Object.create({0: 'shadowed', p: 1}); o[0] = 1;");
      const size_t before = peakResidentBytes();
      JavascriptEngine::Result rk;
      const std::string first =
          jsk.evaluate("var seen = []; for (var k in big) { seen.push(k); if (seen.length == 3) break; }"
                       "for (var k in o) seen.push(k); seen.join()",
                       &rk)
              .toString();
      jsk.evaluate("Object.values(big)", &rk);
      const bool refused = rk.failed() && rk.getErrorMessage().find("Out of memory") == 0;
      const size_t peak = peakResidentBytes();
      const size_t grew = peak > before ? peak - before : 0;
      check(first == "0,1,2,0,p" && refused && (before == 0 || grew < ((size_t) 64 << 20)),
            "for-in over an 8 MB string makes only the keys it reaches, and Object.values of it is "
            "refused before its keys are made");
      if (before && grew >= ((size_t) 64 << 20)) std::printf("    the process grew by %zu MB\n", grew >> 20);
    }

    // console.log: three objects of 300 keys, each the value of every key of
    // the next, print 27 million entries -- 223 MB, built in one native call
    // that ignored stop() for 11 s. Now one line is at most
    // maximumStringLength, and the walk checks the time limit as it goes.
    {
      JavascriptEngine jsl;
      std::string logged;
      jsl.consoleOutput = [&logged](const std::string& line) { logged = line; };
      jsl.execute("var c = {}, b = {}, a = {}; for (var i = 0; i < 300; i++) c['k' + i] = 1;"
                  "for (var i = 0; i < 300; i++) b['k' + i] = c; for (var i = 0; i < 300; i++) a['k' + i] = b;");
      auto t0 = std::chrono::steady_clock::now();
      JavascriptEngine::Result rl = jsl.execute("console.log(a);");
      check(rl.wasOk() && !logged.empty() && logged.size() <= jsl.maximumStringLength && secondsSince(t0) < 3,
            "console.log of a widely shared object writes at most maximumStringLength, promptly");
      jsl.maximumStringLength = (size_t) 1 << 30;
      jsl.maximumExecutionTime = 0.3;
      t0 = std::chrono::steady_clock::now();
      rl = jsl.execute("console.log(a);");
      const double took = secondsSince(t0);
      check(rl.failed() && rl.getErrorMessage() == "Execution timed-out" && took < 3,
            "and with the string limit raised, the time limit still ends it inside console.log");
      if (took >= 3 || rl.wasOk()) std::printf("    %s in %.2f s\n", rl.getDescription().c_str(), took);
      // An Error's message is converted by its own toString, which is script:
      // it may delete the key the walk reaches next, or free the very object
      // being walked. Both crashed (a null and a freed read).
      jsl.maximumExecutionTime = 15;
      rl = jsl.execute("var o = {a: 0, b: 1}; var e = new Error();"
                       "e.message = {toString: function () { delete o.b; return 'm'; }}; o.a = e; console.log(o);");
      check(rl.wasOk() && logged == "{ a: Error: m }", "console.log skips a key deleted while it prints");
      rl = jsl.execute("var e2 = new Error(); var arr;"
                       "e2.message = {toString: function () { arr.length = 0; return 'm'; }};"
                       "arr = [{x: e2, y: 2}]; console.log(arr);");
      check(rl.wasOk() && logged == "[ { x: Error: m, y: 2 } ]",
            "console.log keeps an object alive while it prints it, even if script lets go of it");
    }

    // The conversions without an engine -- JsVar::toString, toJsonString,
    // toJson and an uncaught throw's message -- expanded shared arrays in
    // full: 300 references to an array of 1000 references to a 1000-character
    // string were a 286 MB message from a script holding a few kilobytes,
    // and a million references to one 198-deep chain of one-element arrays
    // took 12 s to write a megabyte of commas.
    {
      JavascriptEngine jst;
      jst.maximumMemoryBytes = (size_t) 256 << 20;
      jst.execute("var c = 'x'.repeat(1000); var b = []; for (var i = 0; i < 1000; i++) b.push(c);"
                  "var a = []; for (var i = 0; i < 300; i++) a.push(b);"
                  "var ch = []; for (var i = 0; i < 197; i++) ch = [ch]; var R = new Array(1000000).fill(ch);");
      const size_t cap = (size_t) 8 << 20;
      auto t0 = std::chrono::steady_clock::now();
      JavascriptEngine::Result rt = jst.execute("throw a;");
      const std::string& message = rt.getErrorMessage();
      check(rt.failed() && message.size() <= cap + 16 && message.compare(message.size() - 3, 3, "...") == 0,
            "an uncaught throw of widely shared arrays gives a message cut at 8 MB");
      const JsVar a = jst.evaluate("a");
      const std::string text = a.toString();
      check(text.size() <= cap + 3 && text.compare(text.size() - 3, 3, "...") == 0 &&
                text.compare(0, 4, "xxxx") == 0,
            "JsVar::toString of it is cut at 8 MB and says so");
      check(a.toJsonString().empty(), "toJsonString of it is refused (empty), not 286 MB");
      const JsonValue j = a.toJson();
      check(j.size() == 300 && j.at(299).at(999).isNull(), "toJson of it cuts the rest with null");
      const JsVar R = jst.evaluate("R");
      const std::string commas = R.toString();
      check(commas.compare(commas.size() - 3, 3, "...") == 0, "a million references to a deep chain are cut too");
      jst.execute("throw R;");
      // Generous, for a sanitizer build: the cut itself is checked above.
      check(secondsSince(t0) < 60, "and all of that takes seconds at most, not minutes");
      check(JsVar::array({1, JsVar::array({2, 3}), "x"}).toString() == "1,2,3,x",
            "small arrays still convert in full");
    }

    // Growth that runs no script step between allocations used to be
    // measured only afterwards, if at all: each of these held two to three
    // times its limit and succeeded. Now each is "Out of memory" with the
    // engine inside its limit.
    {
      struct Grow {
        const char* what;
        int limitMB;
        const char* setup;
        const char* code;
      };
      const Grow grows[] = {
          {"[...a] six times", 32, "var a = []; a.length = 300000;",
           "[[...a], [...a], [...a], [...a], [...a], [...a]]"},
          {"{...a}", 32, "var a = []; a.length = 300000;", "({...a})"},
          {"Object.keys(a)", 32, "var a = []; a.length = 300000;", "Object.keys(a)"},
          {"JSON.stringify", 6, "var b = new Array(60000).fill('x'.repeat(100));", "JSON.stringify(b)"},
          {"JSON.parse", 16,
           "var t = '{' + new Array(300000).fill(0).map(function (x, i) { return '\"k' + i + '\":1'; }).join(',') + '}';"
           "var keep = [];",
           "JSON.parse(t)"},
          {"Object.assign", 8, "var src = {}; for (var i = 0; i < 60000; i++) src['k' + i] = i;",
           "Object.assign({}, src)"}};
      int held = 0;
      for (const Grow& g : grows) {
        JavascriptEngine jsg;
        jsg.maximumExecutionTime = 600; // a sanitizer build is many times slower
        jsg.maximumMemoryBytes = (size_t) g.limitMB << 20;
        // JSON.parse's text is made where the limit allows it to be.
        if (std::string(g.what) == "JSON.parse") jsg.maximumMemoryBytes = (size_t) 256 << 20;
        JavascriptEngine::Result rg = jsg.execute(g.setup);
        jsg.maximumMemoryBytes = (size_t) g.limitMB << 20; // read again by the next call
        rg = jsg.execute(std::string("var keep2 = ") + g.code + ";");
        const bool ok = rg.failed() && rg.getErrorMessage().find("Out of memory") == 0 &&
                        jsg.getMemoryUsage() <= ((size_t) g.limitMB << 20);
        if (ok) ++held;
        else std::printf("    %s: %s, engine holds %zu MB\n", g.what, rg.getDescription().c_str(), jsg.getMemoryUsage() >> 20);
      }
      check(held == 6, "array and object spread, Object.keys, JSON.stringify, JSON.parse and Object.assign "
                       "are refused at maximumMemoryBytes, not after it");
    }

    const std::string parens = std::string(200000, '(') + "1" + std::string(200000, ')');
    js.evaluate(parens, &r);
    check(r.failed() && r.getErrorMessage().find("SyntaxError") == 0,
          "200,000 nested brackets are a SyntaxError, not a stack overflow");
    js.evaluate(std::string(100000, '[') + std::string(100000, ']'), &r);
    check(r.failed(), "100,000 nested array literals likewise");
    js.evaluate("JSON.parse('" + std::string(100000, '[') + "')", &r);
    check(r.failed() && r.getErrorMessage().find("SyntaxError") == 0, "and 100,000 nested brackets in JSON.parse");
    std::string chain = "var o = {a: null}; o";
    for (int i = 0; i < 50000; ++i) chain += ".a";
    js.evaluate(chain, &r);
    check(r.failed(), "a chain of 50,000 property reads ends in an error, not a crash");
    std::string sum = "1";
    for (int i = 0; i < 100000; ++i) sum += "+1";
    check(js.evaluate(sum).toInt() == 100001, "a sum of 100,001 terms evaluates (a left chain costs no stack)");
    js.maximumMemoryBytes = ((size_t) 64 << 20) * kDebugRoom; // the default, or room for Debug
    js.evaluate("var deep = []; for (var i = 0; i < 300000; i++) deep = [deep]; var depthOk = true;", &r);
    check(r.wasOk(), "an array nested 300,000 deep can be built");
    js.evaluate("JSON.stringify(deep)", &r);
    check(r.failed(), "stringifying it is refused rather than overflowing");
    js.evaluate("String(deep).length", &r);
    check(true, "converting it to a string returns (either way)");
    js.execute("deep = null;");
    check(js.evaluate("depthOk").toBool(), "and freeing it does not overflow the stack in a destructor");
  }
  {
    JavascriptEngine js;
    js.maximumMemoryBytes = ((size_t) 16 << 20) * kDebugRoom;
    JavascriptEngine::Result r;
    const auto t0 = std::chrono::steady_clock::now();
    r = js.execute("var a = []; while (true) a.push('item number ' + a.length);");
    check(r.failed() && r.getErrorMessage().find("Out of memory") == 0 && secondsSince(t0) < 10,
          "filling an array until memory runs out fails with \"Out of memory\"");
    js.execute("a = null;");
    js.collectGarbage();
    check(js.getMemoryUsage() < ((size_t) 4 << 20), "and the memory comes back when the script lets go");
    r = js.execute("var o = {}; for (var i = 0; ; i++) o['key' + i] = i;");
    check(r.failed() && r.getErrorMessage().find("Out of memory") == 0, "an object with endless keys likewise");
    js.execute("o = null;");
    js.collectGarbage();
    r = js.execute("var s = 'x'; while (true) s += s;");
    check(r.failed() && r.getErrorMessage().find("RangeError: Invalid string length") == 0,
          "doubling a string stops at maximumStringLength");
    r = js.execute("'abc'.repeat(1e9)");
    check(r.failed() && r.getErrorMessage().find("RangeError") == 0, "repeat(1e9) is refused before allocating");
    r = js.execute("var big = []; big[1e9] = 1;");
    check(r.failed() && r.getErrorMessage().find("RangeError: Invalid array length") == 0,
          "a[1e9] = 1 is refused: arrays are dense");
    r = js.execute("new Array(4294967295)");
    check(r.failed(), "new Array(2^32 - 1) is refused");
    r = js.execute("var keep = []; for (var i = 0; i < 100; i++) keep.push(new Array(1000000).fill(i));");
    check(r.failed() && r.getErrorMessage().find("Out of memory") == 0, "a hundred million-element arrays are refused");
    js.execute("keep = null; s = null; big = null;");
    js.collectGarbage();
    check(js.evaluate("[1, 2, 3].length").toInt() == 3, "and afterwards the engine still works");
  }
  {
    // Garbage cycles are collected while a script runs: this makes 400,000
    // closures, each in a cycle with its scope, under a 16 MB limit.
    JavascriptEngine js;
    js.maximumMemoryBytes = (size_t) 16 << 20;
    js.maximumExecutionTime = 600; // a sanitizer build is many times slower
    JavascriptEngine::Result r = js.execute(
        "function mk(i) { var o = {i: i}; o.f = function () { return o; }; return 1; }\n"
        "for (var i = 0; i < 400000; i++) { var o = {i: i}; o.self = o; mk(i); }");
    if (r.failed()) std::printf("    %s\n", r.getDescription().c_str());
    check(r.wasOk(), "400,000 garbage cycles fit in 16 MB: the collector frees them");
    js.execute("o = null;");
    js.collectGarbage();
    const size_t after = js.getMemoryUsage();
    check(after < ((size_t) 2 << 20), "and nothing of them is left after a collection");
  }
  {
    // stop() from another thread.
    JavascriptEngine js;
    js.maximumExecutionTime = 60;
    std::atomic<bool> started{false};
    js.registerNativeFunction("started", [&started](const JsVar::NativeFunctionArgs&) {
      started = true;
      return JsVar();
    });
    JavascriptEngine::Result r;
    double took = 0;
    std::thread worker([&js, &r, &took] {
      const auto t0 = std::chrono::steady_clock::now();
      r = js.execute("started(); var n = 0; while (true) { n++; }");
      took = secondsSince(t0);
    });
    while (!started) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    js.stop();
    worker.join();
    check(r.failed() && r.getErrorMessage() == "Interrupted" && took < 5, "stop() from another thread interrupts a running script");
    check(js.evaluate("n > 0").toBool() && js.execute("1;").wasOk(), "and the engine is usable afterwards");
    js.stop(); // nothing running: nothing happens
    check(js.evaluate("2 + 2").toInt() == 4, "stop() with nothing running does not cancel the next script");
  }
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0); // a crash must not take the last lines with it
  std::printf("JavascriptEngine\n");
  runTable(kLanguage, sizeof(kLanguage) / sizeof(kLanguage[0]), "the language");
  runTable(kExtensions, sizeof(kExtensions) / sizeof(kExtensions[0]), "the reference's additions");
  errors();
  natives();
  values();
  limits();
  std::printf("%d checks, %d failed\n", g_checks, g_failures);
  std::printf(g_failures ? "JAVASCRIPT TESTS FAILED\n" : "JAVASCRIPT TESTS PASSED\n");
  return g_failures ? 1 : 0;
}
