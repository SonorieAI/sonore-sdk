// SPDX-License-Identifier: Apache-2.0
// Two headers must not define the same free function in the same namespace.
//
//   npm run verify:namespaces
//
// ── Why ─────────────────────────────────────────────────────────────────────
//
// files.h added `sonore::detail::widen`. file_dialog.h already had one, with
// the same signature. Both are `inline`, both are in a header, and nothing
// complained -- until sdk_tests included the two together and MSVC reported
// "function already has a body".
//
// That is luck. Two `inline` definitions of one symbol are an ODR violation:
// the compiler catches it only when both land in the same translation unit,
// and in any other arrangement the linker picks one at random and the other
// header's callers quietly get the wrong function. In a header-only SDK where
// every file drops helpers into a shared `detail` namespace, the collision is
// waiting for whichever two headers are first included together -- which is a
// property of the CONSUMER's include list, not of anything in this repository.
//
// So the rule is checked here rather than left to the next person who happens
// to include the wrong pair.
//
// ── What it does and does not catch ────────────────────────────────────────
//
// It reads namespace nesting by counting braces, which is not a C++ parser and
// does not need to be: this codebase writes `namespace x {` on its own line and
// closes with a comment. It looks at `inline` free functions, which is what a
// header-only SDK collides on. It does NOT look at class members (scoped by the
// class), templates (usually fine), or macros.
//
// Overloads are allowed -- two functions of one name with different parameter
// lists are legal and deliberate. What is reported is the same name with the
// same parameter TYPES in the same namespace from two different files.
import { readdirSync, readFileSync, statSync } from "node:fs";
import { join, relative } from "node:path";

const root = new URL("..", import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, "$1");
const includeDir = join(root, "sdk", "include", "sonore");

function everyHeader(dir) {
  const out = [];
  for (const name of readdirSync(dir)) {
    const full = join(dir, name);
    if (statSync(full).isDirectory()) out.push(...everyHeader(full));
    else if (name.endsWith(".h")) out.push(full);
  }
  return out;
}

/** Parameter types, without names or defaults, so an overload is distinguished
 *  from a redefinition. Crude on purpose: it only has to be consistent. */
function signatureOf(params) {
  return params
    .split(",")
    .map((p) =>
      p
        .replace(/=.*$/, "")            // default arguments
        .replace(/\b\w+\s*$/, "")        // the parameter's own name
        .replace(/\s+/g, " ")
        .trim())
    .filter((p) => p.length > 0)
    .join(",");
}

const defined = new Map(); // "ns::name(sig)" -> [file, ...]

for (const file of everyHeader(includeDir)) {
  const text = readFileSync(file, "utf8");
  const shown = relative(includeDir, file).replace(/\\/g, "/");

  // Strip comments and string literals first: a `{` inside either would throw
  // the brace count off, and this file's own prose is full of both.
  const code = text
    .replace(/\/\*[\s\S]*?\*\//g, "")
    .replace(/\/\/[^\n]*/g, "")
    .replace(/"(\\.|[^"\\])*"/g, '""');

  // One stack for both kinds of scope, because what matters is which is
  // INNERMOST. A function inside a class is scoped by the class and cannot
  // collide with one in another header -- and `inline float process(float)` is
  // a member in seven of these files. Tracking namespaces alone reported all
  // seven as one collision, which was the checker being wrong rather than the
  // code.
  const stack = [];       // { kind: "namespace" | "record", name, depth }
  let depth = 0;
  const lines = code.split("\n");

  for (const line of lines) {
    const namespaceOpen = line.match(/\bnamespace\s+([A-Za-z_]\w*)\s*\{/);
    if (namespaceOpen) {
      stack.push({ kind: "namespace", name: namespaceOpen[1], depth });
      depth += 1;
      continue;
    }

    // A class, struct or union BODY -- not a forward declaration, which ends
    // in a semicolon and opens nothing.
    const recordOpen = line.match(/\b(class|struct|union)\b[\w\s:<>,()]*\{/);
    if (recordOpen) {
      stack.push({ kind: "record", name: "", depth });
      depth += 1;
      continue;
    }

    // An inline free function DEFINITION -- it must open a body on this line,
    // which is what the trailing brace requires.
    const fn = line.match(/^\s*inline\s+[\w:<>,&*\s]+?([A-Za-z_]\w*)\s*\(([^)]*)\)\s*(const)?\s*\{/);
    const inNamespace = stack.length > 0 && stack[stack.length - 1].kind === "namespace";
    if (fn && inNamespace) {
      const scope = stack.map((s) => s.name).filter((n) => n.length > 0).join("::");
      const key = `${scope}::${fn[1]}(${signatureOf(fn[2])})`;
      if (!defined.has(key)) defined.set(key, []);
      const files = defined.get(key);
      if (!files.includes(shown)) files.push(shown);
    }

    for (const c of line) {
      if (c === "{") depth += 1;
      else if (c === "}") {
        depth -= 1;
        while (stack.length > 0 && stack[stack.length - 1].depth >= depth) stack.pop();
      }
    }
  }
}

const clashes = [];
for (const [key, files] of defined)
  if (files.length > 1) clashes.push(`${key} is defined in ${files.join(" AND ")}`);

console.log("── one definition per name per namespace ──\n");
console.log(`  ${defined.size} inline free functions across the headers`);

if (clashes.length > 0) {
  console.log("");
  for (const c of clashes)
    console.log(`  FAIL ${c}\n       Two inline definitions of one symbol is an ODR violation. ` +
                `It only fails to compile when both headers reach one translation unit; ` +
                `otherwise the linker picks one and the other caller gets the wrong function.`);
  console.log(`\n${clashes.length} collision(s).`);
  process.exit(1);
}

console.log("  and no two headers define the same one in the same namespace.");
