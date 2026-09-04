// SPDX-License-Identifier: Apache-2.0
// Every the reference class this project has audited must be ACCOUNTED FOR in the map.
//
// The third direction. feature-map.mjs checks the map against our headers;
// unclaimed.mjs checks our headers against the map. Both are closed loops
// between the map and our own code, and neither can see a class the reference has that
// nobody here has ever thought about.
//
// Accounted for is deliberately a low bar: the name must APPEAR in the feature
// map. Not "must be implemented" -- an honest row saying a thing is out of
// scope, with the reason, is a complete answer and often the right one. What
// this refuses to allow is silence. A the reference class that appears nowhere in the
// map is one nobody has decided about, and a map full of those reports "0 gaps"
// while meaning "0 among the rows somebody wrote down".
//
// Run: npm run verify:parity (verify:features is a different thing -- the
// product-side the reference export harness, which predates this)
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

import { AUDITED, PENDING, NOT_CAPABILITIES } from './reference-index.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const mapText = readFileSync(join(here, 'feature-map.mjs'), 'utf8');

// A word boundary on both sides, so "Toolbar" is not satisfied by
// "ToolbarButton" and "Button" is not satisfied by "ToggleButton". Getting this
// wrong would make the check pass by accident, which is the one failure mode a
// checking tool must not have.
const mentions = (name) => new RegExp(`\\b${name}\\b`).test(mapText);

let missing = [];
let accounted = 0;
let excused = 0;

console.log('\n── the reference classes, against what the map has decided about ──\n');

for (const [group, names] of Object.entries(AUDITED)) {
  const gaps = [];
  for (const name of names) {
    if (NOT_CAPABILITIES[name]) {
      ++excused;
      continue;
    }
    if (mentions(name)) {
      ++accounted;
      continue;
    }
    gaps.push(name);
  }
  const width = 34;
  if (gaps.length === 0) {
    console.log(`  ok  ${group.padEnd(width)} all ${names.length} accounted for`);
  } else {
    console.log(`  --  ${group.padEnd(width)} ${gaps.length} of ${names.length} undecided`);
    missing.push(...gaps.map((n) => [group, n]));
  }
}

console.log(
  `\n${accounted} the reference classes have a decision in the map, ` +
    `${excused} are not capabilities, ${missing.length} undecided`,
);

if (missing.length) {
  console.log('\nUndecided -- the reference has these and nothing here has said anything about them.');
  console.log('A row saying "out of scope, because..." closes one of these as well as');
  console.log('building it does. Silence does not.\n');
  for (const [group, name] of missing) console.log(`  ${name.padEnd(34)} ${group}`);
}

// Said every run, not only when something fails. The number this tool reports
// is only as good as its coverage, and a reader who does not know the coverage
// is partial will read it as complete -- which is the exact mistake this whole
// file exists to stop.
if (PENDING.length) {
  console.log(`\nNOT audited yet (${PENDING.length} areas), so no count here is a total:`);
  for (const area of PENDING) console.log(`  .. ${area}`);
} else {
  // Deliberately loud. An empty list means "nothing is left in the list",
  // which is not the same as "nothing is left" -- and the first time this
  // branch was reachable it was because ten of the reference's twenty-four modules had
  // been indexed and the other fourteen had never been added.
  console.log('\nPENDING is EMPTY. That means every area SOMEBODY WROTE DOWN is walked.');
  console.log('Before reading this as full coverage, compare AUDITED against the module');
  console.log('table in feature-map.mjs -- a module missing from both is invisible here.');
}

if (missing.length) {
  console.log('\nREFERENCE AUDIT FAILED');
  process.exit(1);
}
console.log('\nEvery audited reference class has a decision recorded against it.');
