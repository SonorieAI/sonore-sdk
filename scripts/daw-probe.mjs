// SPDX-License-Identifier: Apache-2.0
// Load the built plugins in a REAL DAW and report what it made of them.
//
//   node scripts/daw-probe.mjs
//
// Every other check in this repo is one we wrote: our host tests are our
// reading of the specs, clap-validator and pluginval are the industry's. None
// of them is a DAW. This one asks REAPER, which is on this machine, to scan
// the plugins and then reads back what its own cache says it found.
//
// ISOLATED, deliberately. REAPER is launched with -cfgfile pointing at a
// throwaway config in a scratch folder, so it uses that folder as its whole
// resource path: its own plugin caches, its own settings, nothing shared with
// the copy the user actually works in. Deleting the folder undoes all of it.
//
// What this proves and what it does not: that a real DAW's scanner loads the
// binary, calls its entry point, accepts what it declares and records it as a
// usable plugin. It does NOT prove the plugin sounds right in a session,
// that needs a person, and the point of doing this is to leave a person less
// to check by hand.
import { spawn, spawnSync } from "node:child_process";
import { copyFileSync, cpSync, existsSync, mkdirSync, readdirSync, readFileSync,
         rmSync, statSync, writeFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const built = join(root, "sdk", "build", "Release");
const log = (s) => process.stdout.write(s + "\n");

const reaper = "C:\\Program Files\\REAPER (x64)\\reaper.exe";
if (!existsSync(reaper)) {
  log("SKIPPED: REAPER is not installed at " + reaper);
  log("This probe is the only check in the repo that uses a real DAW.");
  process.exit(0);
}
if (!existsSync(built)) {
  log("SKIPPED: nothing built yet. Run: npm run verify:sdk");
  process.exit(0);
}

// ── Stage the plugins somewhere REAPER can be pointed at ────────────────────
// A clean folder with ONE copy of each plugin. The build directory holds both
// the flat Name.vst3 DLL and the Name.vst3.bundle folder, and handing a
// scanner both spellings of the same plugin is asking it to report a
// duplicate and calling that a finding.
const probe = join(root, "sdk", "build", "daw-probe");
rmSync(probe, { recursive: true, force: true });
const stage = join(probe, "plugins");
mkdirSync(stage, { recursive: true });
// CLAP cannot be pointed at. REAPER finds .clap files in the two STANDARD
// folders and nowhere else -- there is no path setting for it, so an isolated
// config cannot redirect it the way vstpath64 redirects VST3.
//
// So the per-user one is borrowed, and given back. Not the machine-wide
// folder under Program Files: this is a probe, not an install, and the
// difference is whether anything is left behind. Every file put there is
// recorded and deleted at the end, and a folder that did not exist is removed
// too.
const clapDir = join(process.env.LOCALAPPDATA || "", "Programs", "Common", "CLAP");
const borrowedClapDir = !existsSync(clapDir);
const borrowed = [];

let staged = 0;
for (const name of readdirSync(built)) {
  if (name.endsWith(".vst3.bundle")) {
    cpSync(join(built, name), join(stage, name.replace(/\.bundle$/, "")), { recursive: true });
    ++staged;
  } else if (name.endsWith(".clap")) {
    // CLAP goes to UserPlugins, not the VST path. REAPER finds .clap files in
    // the standard CLAP folders and in its own resource path -- and the
    // standard folders are SHARED, so putting ours there would install them
    // into the machine rather than into a probe.
    if (!clapDir) continue;
    mkdirSync(clapDir, { recursive: true });
    const target = join(clapDir, name);
    // Never overwrite something already there. A name clash would mean
    // deleting somebody's plugin during cleanup.
    if (existsSync(target)) {
      log(`  skipping ${name}: a file of that name is already installed`);
      continue;
    }
    copyFileSync(join(built, name), target);
    borrowed.push(target);
    ++staged;
  }
}
log(`staged ${staged} plugin(s) into ${stage}`);

// ── A REAPER that knows nothing except where to look ────────────────────────
// splashmode/tips off so it comes up without waiting for a click, and the VST
// path pointing only at the staging folder so the report is about us.
const ini = join(probe, "reaper.ini");
writeFileSync(ini, [
  "[REAPER]",
  "vstpath64=" + stage,
  "vstpath=" + stage,
  "splashmode=1",
  "tips=0",
  "showtips=0",
  "reascan=1",
  "",
].join("\r\n"), "utf8");

log("launching REAPER with an isolated config: its own caches, nothing shared");
const child = spawn(reaper, ["-cfgfile", ini, "-nonewinst"], {
  detached: true,
  stdio: "ignore",
});

// ── Wait for the scan, then stop ────────────────────────────────────────────
// The caches are written when scanning finishes. Polling for them beats a
// fixed sleep: a cold scan of eighteen plugins is quick, and a machine that
// is busy should not turn into a failure.
const vstCache = join(probe, "reaper-vstplugins64.ini");
const clapCache = join(probe, "reaper-clap-win64.ini");
// Waited out rather than timed. The two scans do not finish together and
// neither announces itself: the VST cache appears while CLAP is still going,
// so stopping as soon as both files EXIST caught it mid-scan and reported two
// plugins out of nine as though seven had been rejected. Which was a
// measurement of how fast this script is, not of the plugins.
//
// So: watch both caches and stop when neither has grown for a while. A scan
// that has genuinely finished stops writing; one that is still going does
// not.
const sizeOf = (f) => (existsSync(f) ? statSync(f).size : -1);
const sleep = (sec) =>
  spawnSync("cmd", ["/c", "timeout", "/t", String(sec), "/nobreak", ">nul"], { stdio: "ignore" });

const deadline = Date.now() + 240_000;
let lastVst = -2, lastClap = -2, quiet = 0;
while (Date.now() < deadline) {
  sleep(3);
  const v = sizeOf(vstCache), c = sizeOf(clapCache);
  if (v === lastVst && c === lastClap && v >= 0 && c >= 0) {
    if (++quiet >= 5) break; // fifteen seconds of nothing happening
  } else {
    quiet = 0;
  }
  lastVst = v;
  lastClap = c;
}
const settled = quiet >= 5;
spawnSync("taskkill", ["/PID", String(child.pid), "/T", "/F"], { stdio: "ignore" });
spawnSync("taskkill", ["/IM", "reaper.exe", "/F"], { stdio: "ignore" });

if (!settled) {
  log("\nREAPER was still writing its caches after four minutes -- the counts");
  log("below are what it had got to, not what it would have found.");
}

// ── What it made of them ────────────────────────────────────────────────────
// Read REAPER's OWN cache rather than a screenshot or a log: the cache is
// what its plugin browser reads, so a plugin in it is a plugin the user can
// actually load.
function report(path, label, parse) {
  if (!existsSync(path)) {
    log(`\n${label}: no cache written`);
    return 0;
  }
  const lines = readFileSync(path, "utf8").split(/\r?\n/);
  const found = [];
  for (const line of lines) {
    const entry = parse(line);
    if (entry) found.push(entry);
  }
  log(`\n${label}: ${found.length} plugin(s) REAPER accepted`);
  for (const f of found) log("  " + f);
  return found.length;
}

// A VST cache line is  file.vst3=<stamp>,<id>,<Name (Vendor)>  and a plugin
// REAPER rejected is recorded with an empty description, which is the
// difference between "scanned" and "usable".
const vstCount = report(vstCache, "VST3", (line) => {
  const at = line.indexOf("=");
  if (at < 0 || !line.toLowerCase().startsWith("sonore")) return null;
  const parts = line.slice(at + 1).split(",");
  const described = parts.length >= 3 && parts.slice(2).join(",").trim().length > 0;
  return line.slice(0, at) + (described ? "  " + parts.slice(2).join(",") : "  [REJECTED]");
});

// The CLAP cache is sectioned by file: [Name.clap] then one line per plugin
// the file exposes.
let clapCount = 0;
if (existsSync(clapCache)) {
  const text = readFileSync(clapCache, "utf8");
  const names = [];
  let current = "";
  for (const line of text.split(/\r?\n/)) {
    const section = line.match(/^\[(.+)\]$/);
    if (section) {
      current = section[1];
      continue;
    }
    if (!current.toLowerCase().startsWith("sonore")) continue;
    const at = line.indexOf("=");
    if (at <= 0 || line.startsWith("_")) continue;
    names.push(current + "  " + line.slice(at + 1));
  }
  clapCount = names.length;
  log(`\nCLAP: ${names.length} plugin(s) REAPER accepted`);
  for (const n of names) log("  " + n);
}

log("\n── what this means ───────────────────────────────────────────────");
if (vstCount > 0 || clapCount > 0) {
  log("A real DAW loaded these binaries, called their entry points, read what");
  log("they declare and filed them as usable. That is the step every other");
  log("check in this repo stops short of.");
} else {
  log("REAPER recorded none of our plugins. Either the scan did not run or it");
  log("rejected them: the caches in " + probe + " are the evidence either way.");
}
// ── Give the CLAP folder back ───────────────────────────────────────────────
for (const file of borrowed) rmSync(file, { force: true });
if (borrowedClapDir && existsSync(clapDir) && readdirSync(clapDir).length === 0)
  rmSync(clapDir, { recursive: true, force: true });
log(`\nremoved ${borrowed.length} file(s) from the shared CLAP folder`);

log("\nThe scratch folder is " + probe + " and deleting it undoes everything");
log("else this did. The user's own REAPER configuration was never touched.");
