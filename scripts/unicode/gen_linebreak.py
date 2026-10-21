# SPDX-License-Identifier: Apache-2.0
"""Writes the line breaking classes of gfx/line_break.h from the UCD in
$SONORE_UCD: LineBreak.txt (with its @missing defaults), EastAsianWidth.txt
for rule LB30's brackets, emoji/emoji-data.txt for LB30b, and Python's
unicodedata for the general categories LB1 resolves SA by.

LB1's default resolution is applied here: AI, SG and XX are AL; SA is CM
for a nonspacing or spacing mark and AL otherwise; CJ is NS. Each range's
value is its class, with 0x40 set for an opening or closing bracket that is
East Asian (Fullwidth, Wide or Halfwidth) and 0x80 for an unassigned
Extended_Pictographic code point.
"""
import os
import re
import unicodedata

UCD = os.environ['SONORE_UCD']

CLASSES = ['BK', 'CR', 'LF', 'NL', 'SP', 'ZW', 'ZWJ', 'CM', 'WJ', 'GL', 'AL', 'B2', 'BA', 'BB', 'CB', 'CL',
           'CP', 'EB', 'EM', 'EX', 'H2', 'H3', 'HL', 'HY', 'ID', 'IN', 'IS', 'JL', 'JT', 'JV', 'NS', 'NU', 'OP',
           'PO', 'PR', 'QU', 'RI', 'SY']


def ranges(path, field=1):
    """(start, end, value) for each line, and the @missing defaults."""
    out, missing = [], []
    for line in open(path, encoding='utf-8'):
        m = re.match(r'#\s*@missing:\s*([0-9A-F.]+)\s*;\s*(\S+)', line)
        if m:
            r = m.group(1).split('..')
            missing.append((int(r[0], 16), int(r[-1], 16), m.group(2)))
            continue
        line = line.split('#')[0]
        fields = [x.strip() for x in line.split(';')]
        if len(fields) <= field or not fields[0]:
            continue
        r = fields[0].split('..')
        out.append((int(r[0], 16), int(r[-1], 16), fields[field]))
    return out, missing


def main():
    lb = ['XX'] * 0x110000
    listed, missing = ranges(os.path.join(UCD, 'LineBreak.txt'))
    for a, b, v in missing:      # the defaults first, the explicit values over them
        for u in range(a, b + 1):
            lb[u] = v
    for a, b, v in listed:
        for u in range(a, b + 1):
            lb[u] = v
    ea = [''] * 0x110000
    eaw, eam = ranges(os.path.join(UCD, 'EastAsianWidth.txt'))
    for a, b, v in eam + eaw:
        for u in range(a, b + 1):
            ea[u] = v
    pictographic = set()
    pic, _ = ranges(os.path.join(UCD, 'emoji', 'emoji-data.txt'))
    for a, b, v in pic:
        if v == 'Extended_Pictographic':
            pictographic.update(range(a, b + 1))

    values = []
    for u in range(0x110000):
        c = lb[u]
        if c in ('AI', 'SG', 'XX'):
            c = 'AL'
        elif c == 'SA':
            c = 'CM' if unicodedata.category(chr(u)) in ('Mn', 'Mc') else 'AL'
        elif c == 'CJ':
            c = 'NS'
        v = CLASSES.index(c)
        if c in ('OP', 'CP') and ea[u] in ('F', 'W', 'H'):
            v |= 0x40
        if u in pictographic and unicodedata.category(chr(u)) == 'Cn':
            v |= 0x80
        values.append(v)

    print('// Unicode Character Database data, (c) Unicode, Inc., under the Unicode')
    print('// License v3 -- see THIRD_PARTY_NOTICES.md.')
    print('enum Class : uint8_t {')
    print('  ' + ', '.join('k' + c for c in CLASSES))
    print('};')
    print('// Each range\'s class, 0x40 for an East Asian bracket, 0x80 for an unassigned')
    print('// pictographic code point.')
    print('static const uint32_t kLineBreakRanges[] = {')
    line, prev = '   ', None
    for u in range(0x110000):
        if values[u] != prev:
            item = ' 0x%X | (%du << 24),' % (u, values[u])
            if len(line) + len(item) > 98:
                print(line)
                line = '   '
            line += item
            prev = values[u]
    print(line)
    print('};')


main()
