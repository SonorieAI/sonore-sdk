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

// ── Rule: no device callback allocates ──────────────────────────────────────
//
// The DSP entry points above are not the only code on an audio thread: every
// platform backend has a callback the DEVICE calls, and it runs before any of
// them. CoreAudio's capture callback grew its scratch buffer whenever the
// device's IO buffer passed 512 frames -- which any other app can set -- and
// nothing said so, because nothing looked. A body that names an allocating
// call is a body that will allocate on the day its condition comes true.
function checkDeviceCallbacks() {
  console.log("\n── no device callback allocates ──");
  const callbacks = [
    ["audio_wasapi.h", "void renderLoop()"],
    ["audio_wasapi_input.h", "void captureLoop()"],
    ["audio_coreaudio.h", "static OSStatus renderThunk("],
    ["audio_coreaudio_input.h", "static OSStatus inputProc("],
    ["audio_alsa.h", "void renderLoop()"],
    ["audio_alsa_input.h", "void captureLoop()"],
    ["audio_jack.h", "static int processTrampoline("],
    ["audio_asio.h", "onBufferSwitch("],
    // A plugin's render callback is the host's device callback by another
    // name: the AU's grew its sidechain and aux scratch inside it.
    ["au_wrapper.h", "inline OSStatus render("],
  ];
  const allocating = [
    [/\.(assign|resize|reserve|push_back|emplace_back|insert|append)\s*\(/, "a growing container call"],
    [/\bnew\s+[A-Za-z_:<]/, "new"],
    [/\b(malloc|calloc|realloc)\s*\(/, "a C allocation"],
    [/std::(make_unique|make_shared|string\s*\(|vector\s*<)/, "a standard allocation"],
  ];
  for (const [file, signature] of callbacks) {
    const source = read(file);
    // The DEFINITION: the occurrence followed by a body, not a declaration or
    // an address taken for registration.
    let at = -1, open = -1;
    for (let from = 0; ; ) {
      const i = source.indexOf(signature, from);
      if (i < 0) break;
      const brace = source.indexOf("{", i), semi = source.indexOf(";", i);
      if (brace >= 0 && (semi < 0 || brace < semi)) {
        at = i;
        open = brace;
        break;
      }
      from = i + signature.length;
    }
    if (at < 0) {
      console.log(`  FAIL ${file.padEnd(24)} ${signature} not found -- renamed, or this rule checks nothing`);
      ++failures;
      continue;
    }
    let depth = 0, end = open;
    for (; end < source.length; ++end) {
      if (source[end] === "{") depth++;
      else if (source[end] === "}" && --depth === 0) break;
    }
    // Comments say what used to be wrong; only code counts.
    const body = source.slice(open, end).replace(/\/\/.*$/gm, "").replace(/\/\*[\s\S]*?\*\//g, "");
    const hit = allocating.find(([re]) => re.test(body));
    if (hit) {
      console.log(`  FAIL ${file.padEnd(24)} ${signature} -- ${hit[1]} in a device callback`);
      console.log(`       size it when the device opens; the callback runs on the device's clock`);
      ++failures;
    } else {
      console.log(`  ok   ${file.padEnd(24)} ${signature}`);
    }
  }
}

// ── Rule: the pull-model sources keep the rule they claim ───────────────────
//
// audio_source.h is a family of sources that call each other on the audio
// thread: a player calls a transport, which calls a resampler, which calls a
// read-ahead buffer. One allocation or one lock anywhere in that chain is in
// every chain built from it. The reference takes a lock in five of these
// callbacks; the header says it takes none, and marks every function that
// runs on the audio thread "[audio thread]" at the top of its comment. This
// reads those bodies.
//
// A wait is checked as well as a lock. The family's whole design is that the
// thread making a change waits for the audio thread and never the other way
// round, and a waitForBlocksInFlight() that drifted into a callback would be
// a callback waiting for itself.
function checkPullSources() {
  console.log("\n── audio_source.h: no [audio thread] body allocates, locks or waits ──");
  const file = "audio_source.h";
  const source = read(file);
  const forbidden = [
    [/\.(assign|resize|reserve|push_back|emplace_back|insert|append|setSize)\s*\(/, "a growing container call"],
    [/\bnew\s+[A-Za-z_:<]/, "new"],
    [/\b(malloc|calloc|realloc)\s*\(/, "a C allocation"],
    [/std::(make_unique|make_shared|string\s*\(|vector\s*<|function\s*<)/, "a standard allocation"],
    [/\b(lock_guard|unique_lock|scoped_lock|shared_lock)\b|\.(lock|try_lock)\s*\(/, "a lock"],
    [/\.wait(_for|_until)?\s*\(|\b(sleep_for|sleep_until|yield)\s*\(|notify_(one|all)\s*\(/, "a wait"],
    [/\b(waitForBlocksInFlight|waitForNextAudioBlockReady|waitUntil|moveToFrontOfQueue|wakeReader|addTimeSliceClient|removeTimeSliceClient)\s*\(/,
     "a call that waits for another thread"],
    [/\b(fopen|fread|fwrite|fclose|fseek|printf|fprintf)\s*\(/, "file or console I/O"],
  ];
  // Every doc comment whose text starts with [audio thread], and the body of
  // the function it documents: the first "{" after the comment, provided the
  // text in between is a signature (it has a parameter list and no ";" -- a
  // pure virtual or a class has nothing to check).
  // The marker may open the comment or its second line, after the " * ".
  const marker = /\/\*\*[\s*]*\[audio thread\][\s\S]*?\*\//g;
  let checked = 0;
  for (const m of source.matchAll(marker)) {
    const after = m.index + m[0].length;
    const open = source.indexOf("{", after);
    if (open < 0) continue;
    const signature = source.slice(after, open);
    if (signature.includes(";") || !signature.includes("(")) continue;
    let depth = 0, end = open;
    for (; end < source.length; ++end) {
      if (source[end] === "{") depth++;
      else if (source[end] === "}" && --depth === 0) break;
    }
    const name = (signature.match(/([~\w]+)\s*\(/) || [, "?"])[1];
    const line = source.slice(0, m.index).split("\n").length;
    const body = source.slice(open, end).replace(/\/\/.*$/gm, "").replace(/\/\*[\s\S]*?\*\//g, "");
    const hit = forbidden.find(([re]) => re.test(body));
    ++checked;
    if (hit) {
      console.log(`  FAIL ${file}:${line} ${name} -- ${hit[1]} in an [audio thread] body`);
      console.log(`       size it in prepareToPlay(); hand changes over by atomic, SeqBox or BlockEpoch`);
      ++failures;
    } else {
      console.log(`  ok   ${file}:${String(line).padEnd(5)} ${name}`);
    }
  }
  // A rule that finds nothing passes by finding nothing.
  if (checked < 20) {
    console.log(`  FAIL only ${checked} [audio thread] bodies found; there should be at least 20`);
    ++failures;
  }

  // The player's device entry points are device callbacks like any other,
  // and flush denormals as the DSP entry points do.
  const player = source.slice(source.indexOf("class AudioSourcePlayer"));
  for (const entry of ["void audioDeviceIOCallback(", "void render(", "void renderWithInput("]) {
    const at = player.indexOf(entry);
    const open = at < 0 ? -1 : player.indexOf("{", at);
    if (open < 0) {
      console.log(`  FAIL AudioSourcePlayer::${entry.slice(5, -1)} not found -- renamed, or this rule checks nothing`);
      ++failures;
      continue;
    }
    let depth = 0, end = open;
    for (; end < player.length; ++end) {
      if (player[end] === "{") depth++;
      else if (player[end] === "}" && --depth === 0) break;
    }
    if (/ScopedNoDenormals\s+\w+\s*;/.test(player.slice(open, end))) {
      console.log(`  ok   AudioSourcePlayer::${entry.slice(5, -1)} declares ScopedNoDenormals`);
    } else {
      console.log(`  FAIL AudioSourcePlayer::${entry.slice(5, -1)} -- no ScopedNoDenormals in a device entry point`);
      ++failures;
    }
  }
}

checkDenormalGuards();
checkMidiFilter();
checkSysexReach();
checkGuardRestores();
checkDeviceCallbacks();
checkPullSources();

console.log();
if (failures > 0) {
  console.log(`${failures} audio-thread rule(s) broken.`);
  process.exit(1);
}
console.log("Every audio-thread entry point obeys the rules that hold across all of them.");
