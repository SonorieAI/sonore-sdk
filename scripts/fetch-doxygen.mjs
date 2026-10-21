// SPDX-License-Identifier: Apache-2.0
// Fetch Doxygen for `npm run docs` on Windows, into sdk/tools/doxygen/.
//
//   npm run docs:fetch
//
// sdk/tools/ is gitignored and holds the other fetched binaries the gates use
// (clap-validator, pluginval); this puts doxygen.exe beside them, where
// scripts/docs.mjs looks first. On Linux and macOS the package manager's
// doxygen is the one to use (`apt install doxygen`, `brew install doxygen`),
// and this script says so rather than downloading a Windows zip there.
//
// ── Why 1.9.8, and why a hash ───────────────────────────────────────────────
//
// 1.9.8 is the doxygen Ubuntu 24.04 installs with apt, which is what CI runs
// `npm run docs` with. Doxygen's warnings change between releases -- a newer
// parser finds things an older one did not, and the other way round -- so a
// different version here would make the warning gate mean one thing on this
// machine and another in CI. Bump both together.
//
// The SHA-256 is of the official release zip as downloaded from the doxygen
// GitHub releases on 2026-10-14. A version number is not integrity: a hash is.
// The download is refused, and nothing is unpacked, when it does not match.
import { spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const VERSION = "1.9.8";
const ZIP = `doxygen-${VERSION}.windows.x64.bin.zip`;
const URL_ = `https://github.com/doxygen/doxygen/releases/download/Release_${VERSION.replace(/\./g, "_")}/${ZIP}`;
const SHA256 = "03f98f585acee18df0575262feffccc8a93a5aeacd4ee8c21872aeef12532244";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const dest = join(root, "sdk", "tools", "doxygen");
const exe = join(dest, "doxygen.exe");
const stamp = join(dest, "VERSION");

if (process.platform !== "win32") {
  console.log("fetch-doxygen: this fetches the Windows build. Here, install doxygen " +
              `${VERSION} or later with the package manager (apt install doxygen, ` +
              "brew install doxygen) and `npm run docs` finds it on PATH.");
  process.exit(0);
}

if (existsSync(exe) && existsSync(stamp) && readFileSync(stamp, "utf8").trim() === VERSION) {
  console.log(`fetch-doxygen: doxygen ${VERSION} is already in ${dest}`);
  process.exit(0);
}

console.log(`fetch-doxygen: downloading ${URL_}`);
const response = await fetch(URL_, { redirect: "follow" });
if (!response.ok) {
  console.error(`fetch-doxygen: HTTP ${response.status} for ${URL_}`);
  process.exit(1);
}
const bytes = Buffer.from(await response.arrayBuffer());
const got = createHash("sha256").update(bytes).digest("hex");
if (got !== SHA256) {
  console.error(`fetch-doxygen: SHA-256 mismatch for ${ZIP}\n  expected ${SHA256}\n  got      ${got}\n` +
                "Nothing was unpacked. If doxygen re-issued this release, check it by hand " +
                "and update SHA256 in this script.");
  process.exit(1);
}

// A clean directory: an older version's files left beside the new ones would
// be loaded by the new doxygen.exe (libclang.dll ships in the same zip).
rmSync(dest, { recursive: true, force: true });
mkdirSync(dest, { recursive: true });
const zipPath = join(dest, ZIP);
writeFileSync(zipPath, bytes);

// Windows 10 and later ship bsdtar as System32\tar.exe, which reads zips.
// Named by full path: Git for Windows puts GNU tar first on PATH, and GNU
// tar does not.
const tar = join(process.env.SystemRoot || "C:\\Windows", "System32", "tar.exe");
let r = existsSync(tar) ? spawnSync(tar, ["-xf", zipPath, "-C", dest], { stdio: "inherit" }) : { status: 1 };
if (r.status !== 0) {
  r = spawnSync("powershell", ["-NoProfile", "-Command",
                               `Expand-Archive -LiteralPath '${zipPath}' -DestinationPath '${dest}' -Force`],
                { stdio: "inherit" });
}
rmSync(zipPath, { force: true });
if (r.status !== 0 || !existsSync(exe)) {
  console.error(`fetch-doxygen: unpacking ${ZIP} did not produce ${exe}`);
  process.exit(1);
}
writeFileSync(stamp, VERSION + "\n");
console.log(`fetch-doxygen: doxygen ${VERSION} is in ${dest}`);
