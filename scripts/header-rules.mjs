// SPDX-License-Identifier: Apache-2.0
// Rules a header has to obey that no compiler will tell you about until the
// day somebody adds an include somewhere else.
//
//   npm run verify:headers
//
// ── Why this exists ─────────────────────────────────────────────────────────
//
// gfx/viewport.h uses std::max. It compiled for months. Then plugin_editor.h
// gained one more include -- value_box.h, for a readout you can type into --
// and the build broke in viewport.h, a file nobody had touched, with
// "illegal token on right side of ::".
//
// The cause was three headers away: windows.h defines min and max as MACROS,
// and three of our headers included it without NOMINMAX. Whether that mattered
// depended entirely on which header the compiler happened to see first, so the
// bug had been sitting there since those headers were written and was invisible
// until an unrelated include reordered the world.
//
// That is the shape worth catching mechanically: a rule that holds by accident,
// where the accident is somebody else's include order.
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

const problems = [];
const checked = [];

for (const file of everyHeader(includeDir)) {
  const text = readFileSync(file, "utf8");
  const shown = relative(includeDir, file).replace(/\\/g, "/");

  const windowsAt = text.indexOf("#include <windows.h>");
  if (windowsAt < 0) continue;
  checked.push(shown);

  const before = text.slice(0, windowsAt);
  // BEFORE the include, not merely present: a #define after it has already
  // lost, and grep alone would call that a pass.
  if (!/#define\s+NOMINMAX/.test(before))
    problems.push(`${shown}: includes <windows.h> without defining NOMINMAX first. ` +
                  `windows.h defines min and max as macros, so any header included after this ` +
                  `one that uses std::min or std::max stops compiling -- and only for the ` +
                  `translation units where the order works out that way.`);
  if (!/#define\s+WIN32_LEAN_AND_MEAN/.test(before))
    problems.push(`${shown}: includes <windows.h> without WIN32_LEAN_AND_MEAN. Not a ` +
                  `correctness bug, but it drags winsock, RPC, OLE and the shell into every ` +
                  `translation unit that touches this header.`);
}

console.log("── every header that reaches for windows.h ──\n");
for (const name of checked.sort()) console.log(`  ok   ${name}`);

if (problems.length > 0) {
  console.log("");
  for (const p of problems) console.log(`  FAIL ${p}`);
  console.log(`\n${problems.length} header rule violation(s).`);
  process.exit(1);
}

console.log(`\n${checked.length} headers include <windows.h>, and every one of them disarms it ` +
            `first.`);
