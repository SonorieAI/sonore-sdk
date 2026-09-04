#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// Properties every audio-thread entry point must have.
//
// ── Why a script and not a code review ──────────────────────────────────────
//
// There are five functions in this SDK that a wrapper calls to reach a
// generated DSP: four in clap_wrapper.h, one in standalone.h. Every plugin
// this project ever produces goes through one of them. They are the choke
// point, and a rule that has to hold at five separate sites is a rule that
// holds at four of them within a month.
//
// This project has watched that happen: the LV2 turtle validator exited zero
// on an error for weeks, the Linux leg had no pipefail so nothing could fail,
// and readCubic() documented exactly why a modulated delay needed it while
// having no callers at all. Each was one place out of several that quietly
// stopped matching the others.
//
// So the rules live here, mechanically, and the gate runs them.
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..");
const inc = join(root, "sdk", "include", "sonore");
const read = (name) => readFileSync(join(inc, name), "utf8");

let failures = 0;

// ── Rule: every runDsp* declares a ScopedNoDenormals ─────────────────────────
//
// A denormal is handled in microcode rather than in the pipeline, and the
// measured cost on the machine this was written on is 2.82x for a filter
// cascade decaying through silence. It shows up when a track goes QUIET, so
// the plugin that spikes is the one doing nothing.
//
// dsp.h's per-sample flushDenormal() protects code that calls it. It does
// nothing for a DSP generated tomorrow, and the whole premise of this SDK is
// that the DSP is written by something other than a person who knows about
// denormals. The processor-wide flag protects the arithmetic nobody thought
// about.
function checkDenormalGuards() {
  console.log("\n── every DSP entry point flushes denormals ──");
  const files = ["clap_wrapper.h", "standalone.h"];
  let found = 0;

  for (const file of files) {
    const source = read(file);
    // Each entry point, and the body that follows it. Signatures wrap across
    // lines, so the body starts at the first "{" after the parameter list.
    const entry = /inline void (runDsp\w*)\s*\(/g;
    for (const m of source.matchAll(entry)) {
      const name = m[1];
      const open = source.indexOf("{", m.index);
      if (open < 0) continue;
      let depth = 0;
      let end = open;
      for (; end < source.length; ++end) {
        if (source[end] === "{") depth++;
        else if (source[end] === "}" && --depth === 0) break;
      }
      const body = source.slice(open, end);
      ++found;
      if (/ScopedNoDenormals\s+\w+\s*;/.test(body)) {
        console.log(`  ok   ${file.padEnd(18)} ${name}`);
      } else {
        console.log(`  FAIL ${file.padEnd(18)} ${name}: no ScopedNoDenormals`);
        console.log(`       a plugin reaching a DSP through this one keeps the host's`);
        console.log(`       floating-point mode, and pays ~3x on a quiet track`);
        ++failures;
      }
    }
  }

  // A rule that checks nothing is worse than no rule: if the functions were
  // renamed, every check above would pass by finding none of them.
  if (found < 5) {
    console.log(`  FAIL only ${found} DSP entry point(s) found; there should be at least 5`);
    console.log(`       either they were renamed, or this script is looking in the wrong`);
    console.log(`       place and has been passing by finding nothing`);
    ++failures;
  } else {
    console.log(`  ok   ${found} entry points examined`);
  }
}

// ── Rule: the guard restores what it found ───────────────────────────────────
//
// The audio thread belongs to the host. Flush-to-zero changes results rather
// than failing, so leaving it on would silently alter arithmetic in code this
// project does not own.
function checkGuardRestores() {
  console.log("\n── the guard is scoped, not a one-way switch ──");
  const source = read("denormals.h");
  const hasDestructor = /~ScopedNoDenormals\s*\(\s*\)\s*\{\s*writeState\(saved_\)/.test(source);
  if (hasDestructor) {
    console.log("  ok   ~ScopedNoDenormals writes back the saved state");
  } else {
    console.log("  FAIL ~ScopedNoDenormals does not restore the mode it found");
    ++failures;
  }
  // Copying one would restore twice and leave the second object's saved value
  // fighting the first.
  if (/ScopedNoDenormals\(const ScopedNoDenormals&\) = delete/.test(source)) {
    console.log("  ok   and it cannot be copied");
  } else {
    console.log("  FAIL ScopedNoDenormals is copyable, so a copy would restore twice");
    ++failures;
  }
}


// ── Rule: nobody hand-rolls the MIDI status filter ───────────────────────────
//
// Five files used to carry their own copy of `status >= 0x80 && status < 0xf0`
// -- the CLAP wrapper, the LV2 wrapper, the AU wrapper and both of the
// standalone's MIDI parsers. All five said "channel messages only" and none
// said why, and what they were quietly discarding was MIDI clock, start, stop
// and continue: every plugin in every format, deaf to an external sequencer.
//
// Two of the copies had a second bug on top. LV2 also required a length of at
// least three, and a clock byte is ONE. AU masked to the high nibble before
// testing, and 0xF8 & 0xF0 is 0xF0.
//
// The policy is deliverableToDsp() in audio.h. This checks nobody has written
// their own again.
function checkMidiFilter() {
  console.log("\n── the MIDI status filter lives in one place ──");
  const files = [
    "clap_wrapper.h",
    "lv2_wrapper.h",
    "au_wrapper.h",
    "midi_input.h",
    "vst3_wrapper.h",
  ];
  // A range test against 0xf0 written out by hand, in any of the shapes the
  // five copies actually used.
  const handRolled = /(status|kind|byte)\s*[<>=!]{1,2}\s*0x[fF]0|0x[fF]0\s*[<>=!]{1,2}\s*(status|kind)/;

  for (const file of files) {
    const source = read(file);
    const offenders = source
      .split("\n")
      .map((line, i) => [i + 1, line])
      // The classification helpers in audio.h are allowed to mention 0xF0;
      // these files are not. Comments explaining the history are fine.
      .filter(([, line]) => !line.trim().startsWith("//") && !line.trim().startsWith("*"))
      .filter(([, line]) => handRolled.test(line))
      // Extracting a channel or a message KIND from the low/high nibble is a
      // different question and stays legal.
      .filter(([, line]) => !/&\s*0x[fF]0/.test(line) || /[<>=!]{1,2}\s*0x[fF]0/.test(line));

    if (offenders.length === 0) {
      console.log(`  ok   ${file}`);
    } else {
      for (const [n, line] of offenders) {
        console.log(`  FAIL ${file}:${n} hand-rolled status range test`);
        console.log(`       ${line.trim()}`);
        console.log(`       use deliverableToDsp() -- see audio.h`);
        ++failures;
      }
    }
  }

  // And the policy must actually be reached from the wire paths, or the rule
  // above would pass on files that stopped handling MIDI at all.
  let users = 0;
  for (const file of ["clap_wrapper.h", "lv2_wrapper.h", "au_wrapper.h", "midi_input.h"])
    if (/deliverableToDsp\s*\(/.test(read(file))) ++users;
  if (users === 4) {
    console.log("  ok   all four wire paths call the shared policy");
  } else {
    console.log(`  FAIL only ${users} of 4 wire paths call deliverableToDsp()`);
    ++failures;
  }
}


// ── Rule: every wrapper can receive a SysEx ─────────────────────────────
//
// midi_ci.h -- discovery, profiles, property exchange -- was in this SDK for
// weeks with no wire path at all, because MidiBuffer could not carry a SysEx
// byte and every wrapper dropped 0xF0.
//
// Closing that took four separate edits in four files, and AU was last by
// several iterations: its SysEx arrives through MusicDeviceSysEx, a different
// selector from the MIDI callback, so nothing in the MIDI code was even
// looking in the right place. Exactly the shape of gap that goes unnoticed --
// three formats work, the fourth is silent, and silence is what a plugin with
// no SysEx traffic looks like anyway.
//
// Nothing here can compile au_wrapper.h. This is not a substitute for that,
// and it is not pretending to be: it checks that the format has a SysEx path
// at all, which is the thing that was missing.
function checkSysexReach() {
  console.log("\n── every wrapper has a SysEx path ──");
  const wrappers = [
    ["clap_wrapper.h", "CLAP_EVENT_MIDI_SYSEX"],
    ["vst3_wrapper.h", "kMidiSysEx"],
    ["lv2_wrapper.h", "isSysexStart"],
    ["au_wrapper.h", "kMusicDeviceSysExSelect"],
  ];
  for (const [file, marker] of wrappers) {
    const source = read(file);
    const hasEntry = source.includes(marker);
    const hasSink = /addSysex\s*\(/.test(source);
    if (hasEntry && hasSink) {
      console.log(`  ok   ${file.padEnd(18)} ${marker}`);
    } else {
      console.log(`  FAIL ${file.padEnd(18)} no SysEx path` +
                  (hasEntry ? " -- has the entry point but never calls addSysex" : ""));
      console.log(`       a format with no SysEx path is a format where MIDI-CI silently`);
      console.log(`       does nothing, which looks exactly like a plugin nobody sent one to`);
      ++failures;
    }
  }
}

checkDenormalGuards();
checkMidiFilter();
checkSysexReach();
checkGuardRestores();

console.log();
if (failures > 0) {
  console.log(`${failures} audio-thread rule(s) broken.`);
  process.exit(1);
}
console.log("Every audio-thread entry point obeys the rules that hold across all of them.");
