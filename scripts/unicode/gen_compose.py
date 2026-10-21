# SPDX-License-Identifier: Apache-2.0
"""Canonical decompositions (one step) and the pairs NFC composes, from
unicodedata. Prints C++."""
import unicodedata
dec = []
comp = []
for cp in range(0x110000):
    d = unicodedata.decomposition(chr(cp))
    if not d or d.startswith('<'):
        continue
    parts = [int(x, 16) for x in d.split()]
    if len(parts) == 1:
        dec.append((cp, parts[0], 0))
    elif len(parts) == 2:
        dec.append((cp, parts[0], parts[1]))
        if unicodedata.normalize('NFC', chr(parts[0]) + chr(parts[1])) == chr(cp):
            comp.append((parts[0], parts[1], cp))
comp.sort()
def emit(name, rows):
    print('static const uint32_t %s[][3] = {' % name)
    line = '   '
    for r in rows:
        item = ' {0x%X, 0x%X, 0x%X},' % r
        if len(line) + len(item) > 98:
            print(line); line = '   '
        line += item
    print(line)
    print('};')
print('// Canonical decompositions, one step: a character, and the one or two it is.')
emit('kDecompositions', dec)
print('// The pairs canonical composition joins: first, second, and the composite.')
emit('kCompositions', comp)
