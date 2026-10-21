# SPDX-License-Identifier: Apache-2.0
"""Generates the Unicode tables bidi.h needs, from Python's unicodedata (the
Bidi_Class of every assigned code point), the UCD's defaults for unassigned
ones, BidiBrackets.txt and BidiMirroring.txt. Prints C++."""
import os, unicodedata

HERE = os.path.dirname(os.path.abspath(__file__))
UCD = os.environ.get('SONORE_UCD', os.path.join(HERE, 'ucd'))

CLASSES = ['L', 'R', 'AL', 'EN', 'ES', 'ET', 'AN', 'CS', 'NSM', 'BN', 'B', 'S', 'WS', 'ON',
           'LRE', 'LRO', 'RLE', 'RLO', 'PDF', 'LRI', 'RLI', 'FSI', 'PDI']

# DerivedBidiClass.txt's @missing defaults for code points with no character:
# the blocks that are right-to-left or Arabic by default, and the
# noncharacters and default-ignorables that are BN.
DEFAULTS = [
    (0x0590, 0x05FF, 'R'), (0x0600, 0x07BF, 'AL'), (0x07C0, 0x085F, 'R'), (0x0860, 0x08FF, 'AL'),
    (0x20A0, 0x20CF, 'ET'), (0xFB1D, 0xFB4F, 'R'), (0xFB50, 0xFDCF, 'AL'), (0xFDF0, 0xFDFF, 'AL'),
    (0xFE70, 0xFEFF, 'AL'), (0x10800, 0x10CFF, 'R'), (0x10D00, 0x10D3F, 'AL'),
    (0x10D40, 0x10EBF, 'R'), (0x10EC0, 0x10EFF, 'AL'), (0x10F00, 0x10F2F, 'R'),
    (0x10F30, 0x10F6F, 'AL'), (0x10F70, 0x10FFF, 'R'), (0x1E800, 0x1EC6F, 'R'),
    (0x1EC70, 0x1ECBF, 'AL'), (0x1ECC0, 0x1ECFF, 'R'), (0x1ED00, 0x1ED4F, 'AL'),
    (0x1ED50, 0x1EDFF, 'R'), (0x1EE00, 0x1EEFF, 'AL'), (0x1EF00, 0x1EFFF, 'R'),
]


def default_class(cp):
    if (0xFDD0 <= cp <= 0xFDEF) or (cp & 0xFFFE) == 0xFFFE:
        return 'BN'
    if 0xE0000 <= cp <= 0xE0FFF or 0x2060 <= cp <= 0x206F or 0xFFF0 <= cp <= 0xFFF8 or \
       0x1BCA0 <= cp <= 0x1BCA3 or 0x1D173 <= cp <= 0x1D17A:
        return 'BN'
    for lo, hi, c in DEFAULTS:
        if lo <= cp <= hi:
            return c
    return 'L'


def bidi_class(cp):
    c = unicodedata.bidirectional(chr(cp))
    return c if c else default_class(cp)


def ranges():
    out = []
    prev = None
    for cp in range(0x110000):
        c = bidi_class(cp)
        if c != prev:
            out.append((cp, c))
            prev = c
    return out


def brackets():
    pairs = []
    for line in open(os.path.join(UCD, 'BidiBrackets.txt'), encoding='utf-8'):
        line = line.split('#')[0].strip()
        if not line:
            continue
        cp, pair, kind = [f.strip() for f in line.split(';')]
        pairs.append((int(cp, 16), int(pair, 16), kind))
    return pairs


def mirrors():
    out = []
    for line in open(os.path.join(UCD, 'BidiMirroring.txt'), encoding='utf-8'):
        line = line.split('#')[0].strip()
        if not line:
            continue
        a, b = [int(f.strip(), 16) for f in line.split(';')]
        out.append((a, b))
    return out


def main():
    r = ranges()
    print('// Bidi_Class: %d ranges, each from its first code point to the next one\'s.' % len(r))
    print('static const uint32_t kBidiRanges[] = {')
    line = '   '
    for cp, c in r:
        item = ' 0x%X | (%du << 24),' % (cp, CLASSES.index(c))
        if len(line) + len(item) > 98:
            print(line)
            line = '   '
        line += item
    print(line)
    print('};')
    b = brackets()
    print('// BidiBrackets.txt: the opening bracket, its pair, and whether it opens.')
    print('static const uint32_t kBrackets[][3] = {')
    line = '   '
    for cp, pair, kind in b:
        item = ' {0x%X, 0x%X, %d},' % (cp, pair, 1 if kind == 'o' else 0)
        if len(line) + len(item) > 98:
            print(line)
            line = '   '
        line += item
    print(line)
    print('};')
    m = mirrors()
    print('// BidiMirroring.txt: a character and its mirror image.')
    print('static const uint32_t kMirrors[][2] = {')
    line = '   '
    for a, bb in m:
        item = ' {0x%X, 0x%X},' % (a, bb)
        if len(line) + len(item) > 98:
            print(line)
            line = '   '
        line += item
    print(line)
    print('};')


main()
