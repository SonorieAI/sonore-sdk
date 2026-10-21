// Do the TypeScript types of window.sonore say what the bridge defines?
//
//   node scripts/bridge-types.mjs
//
// sdk/include/sonore/sonore.d.ts describes the script gui.h generates for a
// web page, and the device list standalone.h pushes into it. Types written
// once by hand drift: a member added to the bridge is missing from the types
// and a page in TypeScript cannot call it, or a member renamed in the bridge
// is still in the types and a page compiles against something that is not
// there. So this reads the REAL emitted script -- the string literals of
// gui.h, as bridge-keys.mjs does -- and compares the names both ways.
import { readFileSync } from "node:fs";

let checks = 0, failures = 0;
const ok = (cond, what, extra = "") => {
  checks++;
  if (!cond) failures++;
  console.log(`  ${cond ? "ok  " : "FAIL"} ${what}${extra ? "  -- " + extra : ""}`);
};

const read = (path) => readFileSync(new URL(path, import.meta.url), "utf8");
const gui = read("../sdk/include/sonore/gui.h");
const standalone = read("../sdk/include/sonore/standalone.h");
const dts = read("../sdk/include/sonore/sonore.d.ts");

/** The C++ string literals in a slice of source, concatenated and unescaped. */
function literals(text) {
  let out = "";
  for (const m of text.matchAll(/"((?:\\.|[^"\\])*)"/g))
    out += m[1].replace(/\\n/g, "\n").replace(/\\t/g, "\t")
               .replace(/\\"/g, '"').replace(/\\\\/g, "\\");
  return out;
}

/** The keys of the object literal that starts at `open` (a '{'), at its own
 *  level only: nested objects, arrays, calls and strings are stepped over. */
function topLevelKeys(script, open) {
  const keys = new Set();
  let depth = 0, i = open, quote = null;
  for (; i < script.length; i++) {
    const c = script[i];
    if (quote) {
      if (c === "\\") i++;
      else if (c === quote) quote = null;
      continue;
    }
    if (c === "'" || c === '"') { quote = c; continue; }
    if (c === "{" || c === "[" || c === "(") { depth++; continue; }
    if (c === "}" || c === "]" || c === ")") { if (--depth === 0) break; continue; }
    if (depth !== 1) continue;
    const m = /^[A-Za-z_$][\w$]*(?=\s*:)/.exec(script.slice(i));
    if (m && !/[\w$]/.test(script[i - 1] || "")) {
      keys.add(m[0]);
      i += m[0].length - 1;
    }
  }
  return keys;
}

/** The member names of `interface Name { ... }` in the .d.ts. */
function interfaceMembers(name) {
  const at = dts.indexOf(`interface ${name} {`);
  if (at < 0) return null;
  const body = dts.slice(at, dts.indexOf("\n}", at));
  const names = new Set();
  for (const line of body.split("\n").slice(1)) {
    const m = /^\s{2}(?:readonly\s+)?([A-Za-z_$][\w$]*)\s*[?]?\s*[(:]/.exec(line);
    if (m) names.add(m[1]);
  }
  return names;
}

const compare = (bridge, types, what) => {
  const missing = [...bridge].filter((k) => !types.has(k));
  const stale = [...types].filter((k) => !bridge.has(k));
  ok(missing.length === 0, `every ${what} the bridge defines is typed`,
     missing.length ? `not in sonore.d.ts: ${missing.join(", ")}` : `${bridge.size} of them`);
  ok(stale.length === 0, `and every ${what} typed is one the bridge defines`,
     stale.length ? `not in the bridge: ${stale.join(", ")}` : "");
};

// ── window.sonore ─────────────────────────────────────────────────────────
const apiFrom = gui.indexOf('js += "var api={');
const apiTo = gui.indexOf('js += "};\\n";', apiFrom);
ok(apiFrom > 0 && apiTo > apiFrom, "the bridge's api object was found in gui.h");
if (!(apiFrom > 0 && apiTo > apiFrom)) process.exit(1);
const apiScript = literals(gui.slice(apiFrom, apiTo + 20));
const api = [...topLevelKeys(apiScript, apiScript.indexOf("{"))].filter((k) => !k.startsWith("__"));
const sonore = interfaceMembers("Sonore");
ok(sonore !== null, "sonore.d.ts declares interface Sonore");
compare(new Set(api), sonore || new Set(), "member of window.sonore");

// ── a parameter ───────────────────────────────────────────────────────────
const paramsFrom = gui.indexOf('js += "var params=[";');
const paramsTo = gui.indexOf('js += "];\\n";', paramsFrom);
const paramScript = literals(gui.slice(paramsFrom, paramsTo));
const paramKeys = topLevelKeys(paramScript, paramScript.indexOf("{"));
compare(paramKeys, interfaceMembers("SonoreParam") || new Set(), "field of a parameter");

// ── the device list ───────────────────────────────────────────────────────
// What the standalone pushes, and what a plugin's page starts with and keeps:
// the two must be the same shape, or a page written against the standalone
// reads `undefined.length` in a plugin and loses its whole interface.
const jsonFrom = standalone.indexOf("inline std::string audioDevicesJson(");
const jsonTo = standalone.indexOf("return json;", jsonFrom);
const pushed = literals(standalone.slice(jsonFrom, jsonTo));
const pushedKeys = topLevelKeys(pushed, pushed.indexOf("{"));
const initialAt = apiScript.indexOf("audioDevices:{");
const initialKeys = topLevelKeys(apiScript, initialAt + "audioDevices:".length);
compare(pushedKeys, interfaceMembers("SonoreAudioDevices") || new Set(), "device-list field");
const notInitial = [...pushedKeys].filter((k) => !initialKeys.has(k));
ok(notInitial.length === 0, "a plugin's page starts with every device-list field the standalone sends",
   notInitial.length ? `missing from gui.h's initial object: ${notInitial.join(", ")}` : "");

// ── the event ─────────────────────────────────────────────────────────────
ok(literals(gui).includes("new Event('sonore:ready')") && dts.includes('"sonore:ready": Event'),
   "the ready event the bridge fires is the one typed");

console.log(`\n${checks} checks, ${failures} failure(s)`);
if (failures) {
  console.log("BRIDGE TYPES OUT OF STEP");
  process.exit(1);
}
console.log("sonore.d.ts says what the bridge defines.");
