#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// Every audio backend offers the same surface: checked without the hardware.
//
// ── Why this exists ─────────────────────────────────────────────────────────
//
// standalone.h is written once and compiled three ways. `PlatformAudio` is
// WasapiOutput on Windows, AlsaOutput on Linux, CoreAudioOutput on macOS, and
// the shared code calls methods on it that all three must therefore have.
//
// Nobody in this project can compile the macOS one. CI is the only machine
// that could, and CI is blocked. So CoreAudioOutput drifted: it had no
// listDevices, no setDeviceIndex and no deviceName, while standalone.h called
// all three unconditionally. That is not a subtle bug: the macOS standalone
// simply did not compile, and it sat there because the only instrument that
// would have caught it was one nobody could run.
//
// This is an instrument that runs anywhere. It does not compile anything; it
// reads which methods the SHARED code calls and checks that every backend
// declares them. A cross-platform contract, checked cross-platform.
//
// ── Why the contract is derived rather than listed ──────────────────────────
//
// A hand-written list of required methods is a second copy of the truth, and
// the second copy is the one that goes stale, which is the same failure this
// script exists to catch, one level up. So the list comes from the callers:
// whatever standalone.h actually asks of `PlatformAudio` IS the contract, and
// it updates itself the moment somebody calls something new.
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const inc = join(root, 'sdk', 'include', 'sonore');
const read = (name) => readFileSync(join(inc, name), 'utf8');

const standalone = read('standalone.h');

// ── What the shared code asks of an output backend ──────────────────────────
//
// Scoped to functions that actually take a `PlatformAudio&`. The offline
// render path has its own local `std::vector<float> audio`, and counting
// audio.size() as part of a device contract would have every backend grow a
// size() it has no use for.
function outputSurface(source) {
  const wanted = new Set();

  // PlatformAudio::thing(): a static, called from anywhere.
  for (const m of source.matchAll(/\bPlatformAudio::([A-Za-z_]\w*)\s*\(/g)) wanted.add(m[1]);

  // audio.thing(), but only inside a function that was handed one, or that
  // declares one of its own. runInteractive does the latter, and scoping to
  // the parameter form alone missed its audio.stop(): an instrument that
  // under-reports the contract is how the contract drifts in the first place.
  const signature = /\bPlatformAudio&?\s+audio\b/g;
  for (const at of source.matchAll(signature)) {
    // From the signature to the end of that function, by brace depth. Crude
    // and sufficient: these are plain functions, not templates with nested
    // class definitions.
    let i = source.indexOf('{', at.index);
    if (i < 0) continue;
    let depth = 0;
    let end = i;
    for (; end < source.length; ++end) {
      if (source[end] === '{') depth++;
      else if (source[end] === '}' && --depth === 0) break;
    }
    const body = source.slice(i, end);
    for (const m of body.matchAll(/\baudio\.([A-Za-z_]\w*)\s*\(/g)) wanted.add(m[1]);
  }
  return wanted;
}

// The input backend is reached through a member with a distinctive name, so
// there is nothing to disambiguate.
function inputSurface(source) {
  const wanted = new Set();
  for (const m of source.matchAll(/\bPlatformAudioInput::([A-Za-z_]\w*)\s*\(/g)) wanted.add(m[1]);
  for (const m of source.matchAll(/\baudioInput\.([A-Za-z_]\w*)\s*\(/g)) wanted.add(m[1]);
  return wanted;
}

// A method DECLARED by a class in this header. Deliberately loose about
// return types and qualifiers: this is asking "is there something callable by
// that name", which is exactly what the compiler will ask.
function declares(source, method) {
  return new RegExp(`(^|[\\s*&])${method}\\s*\\(`, 'm').test(source);
}

const outputBackends = [
  ['WasapiOutput', 'audio_wasapi.h', 'Windows'],
  ['AlsaOutput', 'audio_alsa.h', 'Linux'],
  // JACK is what a Linux STUDIO runs: it is how applications are routed into
  // each other and how latency stays in the low milliseconds. ALSA reaches the
  // speakers; JACK reaches a DAW.
  ['JackOutput', 'audio_jack.h', 'Linux: JACK/PipeWire'],
  // The one the standalone actually uses, holding both. It has to offer the
  // whole surface too, or the shared code would not compile against it -- which
  // is exactly what this instrument is for.
  ['LinuxOutput', 'audio_linux.h', 'Linux: chooses at runtime'],
  ['CoreAudioOutput', 'audio_coreaudio.h', 'macOS: NOBODY HERE CAN COMPILE THIS'],
];

const inputBackends = [
  ['WasapiInput', 'audio_wasapi_input.h', 'Windows'],
  ['AlsaInput', 'audio_alsa_input.h', 'Linux'],
  ['CoreAudioInput', 'audio_coreaudio_input.h', 'macOS: NOBODY HERE CAN COMPILE THIS'],
  ['NullAudioInput', 'audio_input.h', 'the honest fallback for everything else'],
];

let failures = 0;

function checkGroup(title, surface, backends) {
  const methods = [...surface].sort();
  console.log(`\n── ${title} ──`);
  console.log(`  standalone.h calls: ${methods.join(', ')}`);
  for (const [name, file, platform] of backends) {
    const source = read(file);
    const missing = methods.filter((m) => !declares(source, m));
    if (missing.length === 0) {
      console.log(`  ok   ${name.padEnd(16)} ${platform}`);
    } else {
      console.log(`  FAIL ${name.padEnd(16)} ${platform}`);
      console.log(`       missing: ${missing.join(', ')}`);
      console.log(`       ${file} would not compile against standalone.h`);
      failures += missing.length;
    }
  }
}

checkGroup('audio output backends', outputSurface(standalone), outputBackends);
checkGroup('audio input backends', inputSurface(standalone), inputBackends);

console.log();
if (failures > 0) {
  console.log(`${failures} method(s) missing. A backend that does not offer the whole`);
  console.log('surface is a platform that does not build, and the platform this');
  console.log('usually happens to is the one with no machine to catch it.');
  process.exit(1);
}
console.log('Every backend offers the whole surface the shared code calls.');
