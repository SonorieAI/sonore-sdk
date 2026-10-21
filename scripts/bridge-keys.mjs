// Does the UI bridge resolve the parameter you asked for?
//
//   node scripts/bridge-keys.mjs
//
// `gui.h` builds `window.sonore` as a generated script, and a generated
// interface asks for its values BY NAME -- `sonore.get(p.label)` -- because
// that is what the studio's bridge accepts and the studio is where the
// interface is written and approved.
//
// Names are normalised before lookup, and `norm` strips everything that is not
// a letter or a digit. That includes the decimal point, so "1.25k" and "12.5k"
// both come out "125k". Those are not contrived: they are what a graphic EQ
// calls its bands, along with "31.5"/"315" and "1.6k"/"16k". With only the
// normalised label in the table, both faders resolved to whichever parameter
// was written last -- two controls moving one value, and silently, because both
// names resolve to SOMETHING.
//
// This runs the real emitted script rather than a copy of it. The fragments are
// lifted out of `gui.h` between two markers and evaluated against a parameter
// list with the colliding labels in it: a copy of the logic here would pass
// forever after the header stopped agreeing with it, which is the failure this
// whole check exists to catch one level down.
import { readFileSync } from "node:fs";

let checks = 0, failures = 0;
const ok = (cond, what, extra = "") => {
  checks++;
  if (!cond) failures++;
  console.log(`  ${cond ? "ok  " : "FAIL"} ${what}${extra ? "  -- " + extra : ""}`);
};

const src = readFileSync(new URL("../sdk/include/sonore/gui.h", import.meta.url), "utf8");

// ── lift the fragments ───────────────────────────────────────────────────
//
// Every line that appends to the script looks like `js += "..."` with the
// literal possibly continued over following lines. Taking the region between
// `function norm` and `function vals` gets norm, the table and the resolver,
// which is the whole of what a lookup touches.
const from = src.indexOf('js += "function norm(');
const to = src.indexOf('js += "function vals(');
ok(from > 0 && to > from, "the bridge's lookup fragments were found in gui.h",
   from > 0 && to > from ? `${to - from} chars of gui.h` : "markers moved");
if (!(from > 0 && to > from)) process.exit(1);

/** The C++ string literals in a slice of source, concatenated and unescaped. */
function literals(text) {
  let out = "";
  // Non-greedy, and \\. first so an escaped quote does not end the match.
  for (const m of text.matchAll(/"((?:\\.|[^"\\])*)"/g))
    out += m[1].replace(/\\n/g, "\n").replace(/\\t/g, "\t")
               .replace(/\\"/g, '"').replace(/\\\\/g, "\\");
  return out;
}
const bridge = literals(src.slice(from, to));
ok(/function norm\(/.test(bridge) && /function reindex\(/.test(bridge) &&
   /function P\(/.test(bridge),
   "...and they carry norm, reindex and the resolver",
   `${bridge.length} chars of script`);

// ── run it ───────────────────────────────────────────────────────────────
//
// A graphic EQ's bands, which is where this was found. Ids are unique by
// construction; the labels are what a person reads off the faceplate.
const table = [
  ["b315", "31.5"], ["b630", "63"], ["b1250", "125"], ["b2500", "250"],
  ["b5000", "500"], ["b1000", "1k"], ["b1600", "1.6k"], ["b2000", "2k"],
  ["b12500", "12.5k"], ["b1250k", "1.25k"], ["b16000", "16k"],
  ["b315hz", "315"], ["b25", "25"], ["b2500k", "2.5k"],
];
const make = () => table.map(([id, label]) => ({ id, label, value: 0 }));

const run = (params) => {
  const scope = { params };
  // `byKey` and the functions are declared by the script itself; giving it a
  // params array and letting it build its own table is the point.
  const fn = new Function("params", bridge + "\nreindex();\nreturn {P:P,norm:norm,byKey:byKey};");
  return fn(scope.params);
};

const params = make();
const api = run(params);

const collide = [["1.25k", "12.5k"], ["31.5", "315"], ["1.6k", "16k"]];
for (const [a, b] of collide) {
  ok(api.norm(a) === api.norm(b),
     `"${a}" and "${b}" really do normalise the same`, `both -> "${api.norm(a)}"`);
  const pa = api.P(a), pb = api.P(b);
  ok(pa && pb && pa.id !== pb.id,
     `...and each still resolves to its OWN parameter`,
     pa && pb ? `${a} -> ${pa.id}, ${b} -> ${pb.id}` : "one of them resolved to nothing");
}

// The ids have to keep working, and so does an index, since the same resolver
// serves all three and a page may use any of them.
ok(api.P("b1250")?.label === "125", "an exact id still wins",
   `b1250 -> ${api.P("b1250")?.label}`);
ok(api.P(0)?.id === "b315", "...and a positional lookup still works",
   `0 -> ${api.P(0)?.id}`);
ok(api.P("B1250")?.id === "b1250", "...and a normalised id is still a fallback",
   `B1250 -> ${api.P("B1250")?.id}`);
ok(api.P("nope") === null, "...while a name nothing has is null, not a guess");

// A table with no prototype, or a control named after one of Object's members
// resolves to a function. `P` would then hand it back as a parameter.
ok(api.P("constructor") === null,
   "a label that is also an Object member does not resolve to one",
   `constructor -> ${JSON.stringify(api.P("constructor"))}`);
ok(api.P("toString") === null, "...nor does toString");

console.log(`\n${checks} checks, ${failures} failure(s)`);
process.exit(failures ? 1 : 0);
