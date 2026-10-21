// SPDX-License-Identifier: Apache-2.0
// clang's verdict on every translation unit, without a clang install.
//
//   npm run verify:clang
//
// The SDK is built with MSVC here, and MSVC accepts things gcc and clang
// refuse. The first time this ran it found a nested `Options` used as a
// default argument in five classes -- fine for MSVC, an error for gcc and
// clang, so every Linux and macOS build including those headers would have
// failed. CI has real gcc and clang legs; this is for the box where they are
// not installed, which is every Windows box with only Visual Studio on it.
//
// Visual Studio's LLVM tools ship clang-tidy (no clang-cl), and clang-tidy is
// clang's whole front end. With every tidy check off it reports exactly what
// the compiler would: errors, and the /W4 (-Wall -Wextra) warnings. The
// commands come from a compile_commands.json, which the Visual Studio
// generator cannot write, so an NMake configure in sdk/build-clangcheck
// supplies them. Nothing is built there; it is only configured.
//
// Fails on any error, and on any warning in a public header -- a user's
// clang build at -Wall should be quiet about ours. Warnings in tests are
// listed, not failed.
//
// Windows only; elsewhere the compiler is clang or gcc already and the build
// itself is the check. SONORE_CLANG_JOBS sets the parallelism (default: half
// the cores). An argument narrows the run to files whose path contains it.
import { spawn, spawnSync } from "node:child_process";
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { cpus } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const sdk = join(root, "sdk");
const buildDir = join(sdk, "build-clangcheck");
const log = (s = "") => process.stdout.write(s + "\n");
const filter = process.argv[2] || "";

if (process.platform !== "win32") {
  log("clang-check: SKIPPED -- on this platform the compiler is clang or gcc already");
  process.exit(0);
}
const vswhere = "C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe";
const vsPath = existsSync(vswhere)
  ? (spawnSync(vswhere, ["-latest", "-property", "installationPath"], { stdio: "pipe" }).stdout || "").toString().trim()
  : "";
const vcvars = vsPath ? join(vsPath, "VC", "Auxiliary", "Build", "vcvars64.bat") : "";
const tidy = vsPath ? join(vsPath, "VC", "Tools", "Llvm", "x64", "bin", "clang-tidy.exe") : "";
if (!vcvars || !existsSync(vcvars) || !existsSync(tidy)) {
  log("clang-check: SKIPPED -- needs Visual Studio's C++ workload and its LLVM tools (clang-tidy.exe)");
  process.exit(0);
}

// The vcvars environment, captured once: clang-tidy in cl mode finds the MSVC
// headers through INCLUDE, and cmake finds nmake and cl through PATH.
mkdirSync(buildDir, { recursive: true });
const cmd = join(buildDir, "configure.cmd");
writeFileSync(cmd, [
  "@echo off",
  `call "${vcvars}" >nul`,
  `cmake -S "${sdk}" -B "${buildDir}" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON > "${join(buildDir, "configure.log")}" 2>&1`,
  "if errorlevel 1 exit /b 1",
  "set",
  "",
].join("\r\n"), "utf8");
const configured = spawnSync("cmd", ["/c", cmd], { stdio: "pipe", maxBuffer: 64 * 1024 * 1024 });
if (configured.status !== 0) {
  log(`clang-check: the NMake configure failed -- see ${join(buildDir, "configure.log")}`);
  process.exit(1);
}
const env = {};
for (const line of configured.stdout.toString().split(/\r?\n/)) {
  const eq = line.indexOf("=");
  if (eq > 0) env[line.slice(0, eq)] = line.slice(eq + 1);
}

const db = JSON.parse(readFileSync(join(buildDir, "compile_commands.json"), "utf8"));
const files = [...new Set(db.map((e) => e.file))]
  .filter((f) => !f.includes("_deps") && f.includes(filter))
  .sort();
const jobs = Math.max(1, +process.env.SONORE_CLANG_JOBS || Math.floor(cpus().length / 2));
log(`clang-check: ${files.length} translation units, ${jobs} at a time (clang-tidy from ${dirname(tidy)})`);

// No tidy check is wanted, but clang-tidy will not run with none, and turning
// all of them off also hides the compiler's own warnings. So the compiler's
// diagnostics are asked for by name, beside one check that is all but silent
// and whose reports are dropped below.
const args = (file) => [
  "-p", buildDir, "--quiet",
  "--checks=-*,clang-diagnostic-*,readability-avoid-unconditional-preprocessor-if",
  "--header-filter=sonore",
  "--extra-arg=/W4", "--extra-arg=-Wno-unknown-pragmas",
  // Two-phase lookup, as gcc and clang on Linux and macOS do it. clang-cl
  // defaults to MSVC's delayed template parsing, which never looks at a
  // template body that is not instantiated: a branch of an `if constexpr`
  // that one TU never takes went unchecked here and failed both of CI's
  // non-Windows compilers (a stb_vorbis macro, R, in resample.h).
  "--extra-arg=/Zc:twoPhase",
  file,
];

const diags = new Map(); // "path:line kind msg" -> { kind, path, line, msg, flag, tus }
let done = 0;
function runOne(file) {
  return new Promise((resolveRun) => {
    const child = spawn(tidy, args(file), { env, stdio: ["ignore", "pipe", "pipe"] });
    let text = "";
    child.stdout.on("data", (d) => (text += d));
    child.stderr.on("data", (d) => (text += d));
    child.on("close", () => {
      const re = /^(.*?):(\d+):\d+: (error|warning): (.*?)(?: \[([^\]]+)\])?$/gm;
      for (let m; (m = re.exec(text)); ) {
        const flag = m[5] || "";
        if (flag.startsWith("readability-")) continue; // the placeholder check
        const path = m[1].replace(/\\/g, "/");
        const key = `${path}:${m[2]} ${m[3]} ${m[4]}`;
        if (!diags.has(key)) diags.set(key, { kind: m[3], path, line: +m[2], msg: m[4], flag, tus: [] });
        diags.get(key).tus.push(file.replace(/\\/g, "/").split("/").pop());
      }
      if (++done % 25 === 0 || done === files.length) log(`  ${done}/${files.length}`);
      resolveRun();
    });
  });
}
const queue = [...files];
await Promise.all(Array.from({ length: jobs }, async () => {
  while (queue.length) await runOne(queue.shift());
}));

const all = [...diags.values()].sort((a, b) => (a.path + a.line).localeCompare(b.path + b.line));
const errors = all.filter((d) => d.kind === "error");
const inHeaders = all.filter((d) => d.kind === "warning" && d.path.includes("/sdk/include/"));
const elsewhere = all.filter((d) => d.kind === "warning" && !d.path.includes("/sdk/include/"));
const show = (d) => `  ${d.path.replace(/^.*\/sdk\//, "sdk/")}:${d.line}: ${d.msg}${d.flag ? ` [${d.flag}]` : ""} (${d.tus.length} TU, e.g. ${d.tus[0]})`;
log(`\nerrors: ${errors.length}`);
errors.forEach((d) => log(show(d)));
log(`warnings in public headers: ${inHeaders.length}`);
inHeaders.forEach((d) => log(show(d)));
log(`warnings in tests and examples (listed, not failed): ${elsewhere.length}`);
elsewhere.forEach((d) => log(show(d)));
const ok = errors.length === 0 && inHeaders.length === 0;
log(ok ? "\nCLANG CHECK PASSED" : "\nCLANG CHECK FAILED");
process.exit(ok ? 0 : 1);
