# SPDX-License-Identifier: Apache-2.0
"""Generates the Unicode tables text_shaper.h needs: Arabic joining types
(ArabicShaping.txt, with the derivation rule for unlisted characters),
script ranges (Scripts.txt), and whether a character is a combining mark
or a default ignorable. Prints C++."""
import os, unicodedata

HERE = os.path.dirname(os.path.abspath(__file__))
UCD = os.environ.get('SONORE_UCD', os.path.join(HERE, 'ucd'))

JOINING = ['U', 'R', 'D', 'L', 'C', 'T']  # index = enum value


def joining():
    listed = {}
    for line in open(os.path.join(UCD, 'ArabicShaping.txt'), encoding='utf-8'):
        line = line.split('#')[0].strip()
        if not line:
            continue
        f = [x.strip() for x in line.split(';')]
        listed[int(f[0], 16)] = f[2]
    out = []
    prev = None
    for cp in range(0x110000):
        if cp in listed:
            j = listed[cp]
        else:
            cat = unicodedata.category(chr(cp))
            j = 'T' if cat in ('Mn', 'Me', 'Cf') else 'U'
        if j != prev:
            out.append((cp, j))
            prev = j
    return out


def scripts():
    """ISO 15924 codes, from fontTools (which reads Scripts.txt): the
    OpenType script tag is the code in lower case."""
    from fontTools import unicodedata as fu
    names = ['Zzzz']
    out = []
    prev = None
    for cp in range(0x110000):
        s = fu.script(chr(cp))
        if s not in names:
            names.append(s)
        if s != prev:
            out.append((cp, names.index(s)))
            prev = s
    return names, out


def combining():
    out = []
    prev = None
    for cp in range(0x110000):
        c = unicodedata.combining(chr(cp))
        if c != prev:
            out.append((cp, c))
            prev = c
    return out


def default_ignorable(cp):
    # DerivedCoreProperties' Default_Ignorable_Code_Point, as HarfBuzz takes
    # it: less the Hangul fillers U+115F, U+1160, U+3164 and U+FFA0, which
    # fonts draw as spacing glyphs, as Uniscribe does, and the shorthand
    # format controls U+1BCA0..1BCA3.
    if cp in (0x00AD, 0x034F, 0x061C, 0x17B4, 0x17B5, 0xFEFF):
        return True
    ranges = [(0x180B, 0x180F), (0x200B, 0x200F), (0x202A, 0x202E), (0x2060, 0x206F),
              (0xFE00, 0xFE0F), (0xFFF0, 0xFFF8), (0x1D173, 0x1D17A), (0xE0000, 0xE0FFF)]
    return any(a <= cp <= b for a, b in ranges)


def emit(name, pairs, fmt):
    print('static const uint32_t %s[] = {' % name)
    line = '   '
    for cp, v in pairs:
        item = ' ' + fmt % (cp, v) + ','
        if len(line) + len(item) > 98:
            print(line)
            line = '   '
        line += item
    print(line)
    print('};')


def main():
    j = joining()
    print('// Joining_Type: U R D L C T, from each range\'s first code point to the next one\'s.')
    emit('kJoiningRanges', [(cp, JOINING.index(t)) for cp, t in j], '0x%X | (%du << 24)')
    names, s = scripts()
    print('// Script, as its ISO 15924 code, by index into kScriptNames.')
    print('static const char* const kScriptNames[] = {')
    line = '   '
    for n in names:
        item = ' "%s",' % n
        if len(line) + len(item) > 98:
            print(line)
            line = '   '
        line += item
    print(line)
    print('};')
    emit('kScriptRanges', s, '0x%X | (%du << 24)')
    marks = []
    prev = None
    for cp in range(0x110000):
        m = {'Mn': 1, 'Mc': 2, 'Me': 3}.get(unicodedata.category(chr(cp)), 0)
        if m != prev:
            marks.append((cp, m))
            prev = m
    print('// General_Category M: a combining mark -- 1 Mn, 2 Mc, 3 Me.')
    emit('kMarkRanges', marks, '0x%X | (%du << 24)')
    ign = []
    prev = None
    for cp in range(0x110000):
        d = default_ignorable(cp)
        if d != prev:
            ign.append((cp, 1 if d else 0))
            prev = d
    print('// Default_Ignorable_Code_Point.')
    emit('kIgnorableRanges', ign, '0x%X | (%du << 24)')
    word = []
    prev = None
    for cp in range(0x110000):
        w = 1 if unicodedata.category(chr(cp))[0] in 'LM' or unicodedata.category(chr(cp)) in ('Cf', 'Cn', 'Co', 'Cs') else 0
        if w != prev:
            word.append((cp, w))
            prev = w
    digits = []
    prev = None
    for cp in range(0x110000):
        d = 1 if unicodedata.category(chr(cp)) == 'Nd' else 0
        if d != prev:
            digits.append((cp, d))
            prev = d
    print('// General_Category Nd: a decimal digit.')
    emit('kDecimalRanges', digits, '0x%X | (%du << 24)')
    pictographic = set()
    emoji = {}
    for line in open(os.path.join(UCD, 'emoji', 'emoji-data.txt'), encoding='utf-8'):
        line = line.split('#')[0].strip()
        if not line:
            continue
        cps, prop = [x.strip() for x in line.split(';')]
        a, _, b = cps.partition('..')
        span = range(int(a, 16), int(b or a, 16) + 1)
        if prop == 'Extended_Pictographic':
            pictographic.update(span)
        elif prop in ('Emoji', 'Emoji_Presentation'):
            for cp in span:
                emoji[cp] = emoji.get(cp, 0) | (1 if prop == 'Emoji' else 2)
    pict = []
    prev = None
    for cp in range(0x110000):
        e = 1 if cp in pictographic else 0
        if e != prev:
            pict.append((cp, e))
            prev = e
    print('// Extended_Pictographic, from emoji-data.txt: what a ZWJ joins in an emoji sequence.')
    emit('kPictographicRanges', pict, '0x%X | (%du << 24)')
    props = []
    prev = None
    for cp in range(0x110000):
        e = emoji.get(cp, 0)
        if e != prev:
            props.append((cp, e))
            prev = e
    print('// Emoji (1) and Emoji_Presentation (2), from emoji-data.txt: which face draws a character.')
    emit('kEmojiRanges', props, '0x%X | (%du << 24)')
    print('// General_Category L, M, Cf, Cn, Co or Cs: what a word goes on with.')
    emit('kWordRanges', word, '0x%X | (%du << 24)')
    print('// Canonical_Combining_Class, nonzero for marks that reorder.')
    emit('kCombiningRanges', combining(), '0x%X | (%du << 24)')


main()
