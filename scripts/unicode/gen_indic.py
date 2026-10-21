# SPDX-License-Identifier: Apache-2.0
"""Writes the Indic, Khmer and Myanmar character categories of
gfx/text_shaper.h, as HarfBuzz's gen-indic-table.py derives them (MIT; see
THIRD_PARTY_NOTICES) from the UCD's IndicSyllabicCategory.txt,
IndicPositionalCategory.txt and Blocks.txt, found in $SONORE_UCD.

Each character in the blocks the three shapers read gets a category (the
syllable machines' numbering) and a position (where reordering puts it);
the pairs are numbered in a palette and the characters given as ranges.
"""
import os

UCD = os.environ['SONORE_UCD']

ALLOWED_SINGLES = [0x00A0, 0x25CC]
ALLOWED_BLOCKS = ['Basic Latin', 'Latin-1 Supplement', 'Devanagari', 'Bengali', 'Gurmukhi', 'Gujarati',
                  'Oriya', 'Tamil', 'Telugu', 'Kannada', 'Malayalam', 'Myanmar', 'Khmer', 'Vedic Extensions',
                  'General Punctuation', 'Superscripts and Subscripts', 'Devanagari Extended',
                  'Myanmar Extended-B', 'Myanmar Extended-A', 'Myanmar Extended-C']

# The syllable machines' numbers for the categories (gen_syllables.py).
CATEGORY = dict(X=0, C=1, V=2, N=3, H=4, ZWNJ=5, ZWJ=6, M=7, SM=8, A=9, VD=9, PLACEHOLDER=10,
                DOTTEDCIRCLE=11, RS=12, MPst=13, Repha=14, Ra=15, CM=16, Symbol=17, CS=18, SMPst=57,
                VAbv=20, VBlw=21, VPre=22, VPst=23, Robatic=25, Xgroup=26, Ygroup=27,
                IV=2, As=32, DB=3, GB=10, MH=35, MR=36, MW=37, MY=38, PT=39, VS=40, ML=41)
POSITION = dict(START=0, RA_TO_BECOME_REPH=1, PRE_M=2, PRE_C=3, BASE_C=4, AFTER_MAIN=5, ABOVE_C=6,
                BEFORE_SUB=7, BELOW_C=8, AFTER_SUB=9, BEFORE_POST=10, POST_C=11, AFTER_POST=12,
                SMVD=13, END=14)

category_map = {
    'Other': 'X', 'Avagraha': 'Symbol', 'Bindu': 'SM', 'Brahmi_Joining_Number': 'PLACEHOLDER',
    'Cantillation_Mark': 'A', 'Consonant': 'C', 'Consonant_Dead': 'C', 'Consonant_Final': 'CM',
    'Consonant_Head_Letter': 'C', 'Consonant_Initial_Postfixed': 'C', 'Consonant_Killer': 'M',
    'Consonant_Medial': 'CM', 'Consonant_Placeholder': 'PLACEHOLDER', 'Consonant_Preceding_Repha': 'Repha',
    'Consonant_Prefixed': 'X', 'Consonant_Subjoined': 'CM', 'Consonant_Succeeding_Repha': 'CM',
    'Consonant_With_Stacker': 'CS', 'Gemination_Mark': 'SM', 'Invisible_Stacker': 'H', 'Joiner': 'ZWJ',
    'Modifying_Letter': 'X', 'Non_Joiner': 'ZWNJ', 'Nukta': 'N', 'Number': 'PLACEHOLDER',
    'Number_Joiner': 'PLACEHOLDER', 'Pure_Killer': 'M', 'Register_Shifter': 'RS', 'Syllable_Modifier': 'SM',
    'Tone_Letter': 'X', 'Tone_Mark': 'N', 'Virama': 'H', 'Visarga': 'SM', 'Vowel': 'V',
    'Vowel_Dependent': 'M', 'Vowel_Independent': 'V',
}
position_map = {
    'Not_Applicable': 'END', 'Left': 'PRE_C', 'Top': 'ABOVE_C', 'Bottom': 'BELOW_C', 'Right': 'POST_C',
    'Bottom_And_Right': 'POST_C', 'Left_And_Right': 'POST_C', 'Top_And_Bottom': 'BELOW_C',
    'Top_And_Bottom_And_Left': 'BELOW_C', 'Top_And_Bottom_And_Right': 'POST_C', 'Top_And_Left': 'ABOVE_C',
    'Top_And_Left_And_Right': 'POST_C', 'Top_And_Right': 'POST_C', 'Overstruck': 'AFTER_MAIN',
    'Visual_order_left': 'PRE_M',
}
category_overrides = {}
for u in range(0xFE00, 0xFE10):
    category_overrides[u] = 'VS'
category_overrides.update({
    0x2015: 'PLACEHOLDER', 0x2022: 'PLACEHOLDER', 0x25FB: 'PLACEHOLDER', 0x25FC: 'PLACEHOLDER',
    0x25FD: 'PLACEHOLDER', 0x25FE: 'PLACEHOLDER',
    # Indic
    0x0930: 'Ra', 0x09B0: 'Ra', 0x09F0: 'Ra', 0x0A30: 'Ra', 0x0AB0: 'Ra', 0x0B30: 'Ra', 0x0BB0: 'Ra',
    0x0C30: 'Ra', 0x0CB0: 'Ra', 0x0D30: 'Ra',
    0x0953: 'SM', 0x0954: 'SM', 0x0A40: 'MPst', 0x0A72: 'C', 0x0A73: 'C',
    0x1CE2: 'A', 0x1CE3: 'A', 0x1CE4: 'A', 0x1CE5: 'A', 0x1CE6: 'A', 0x1CE7: 'A', 0x1CE8: 'A', 0x1CED: 'A',
    0xA8F2: 'Symbol', 0xA8F3: 'Symbol', 0xA8F4: 'Symbol', 0xA8F5: 'Symbol', 0xA8F6: 'Symbol',
    0xA8F7: 'Symbol', 0x1CE9: 'Symbol', 0x1CEA: 'Symbol', 0x1CEB: 'Symbol', 0x1CEC: 'Symbol',
    0x1CEE: 'Symbol', 0x1CEF: 'Symbol', 0x1CF0: 'Symbol', 0x1CF1: 'Symbol',
    0x0A51: 'M',
    0x11301: 'SM', 0x11302: 'SM', 0x11303: 'SM', 0x1133B: 'N', 0x1133C: 'N',
    0x0AFB: 'N', 0x0B55: 'N', 0x09FC: 'PLACEHOLDER', 0x0C80: 'PLACEHOLDER', 0x0D04: 'PLACEHOLDER',
    0x25CC: 'DOTTEDCIRCLE',
    # Khmer
    0x179A: 'Ra', 0x17CC: 'Robatic', 0x17C9: 'Robatic', 0x17CA: 'Robatic',
    0x17C6: 'Xgroup', 0x17CB: 'Xgroup', 0x17CD: 'Xgroup', 0x17CE: 'Xgroup', 0x17CF: 'Xgroup',
    0x17D0: 'Xgroup', 0x17D1: 'Xgroup',
    0x17C7: 'Ygroup', 0x17C8: 'Ygroup', 0x17DD: 'Ygroup', 0x17D3: 'Ygroup',
    0x17D9: 'PLACEHOLDER',
    # Myanmar
    0x104E: 'C', 0x1004: 'Ra', 0x101B: 'Ra', 0x105A: 'Ra', 0x1032: 'A', 0x1036: 'A', 0x103A: 'As',
    0x103E: 'MH', 0x1060: 'ML', 0x103C: 'MR', 0x103D: 'MW', 0x1082: 'MW', 0x103B: 'MY', 0x105E: 'MY',
    0x105F: 'MY', 0x1063: 'PT', 0x1064: 'PT', 0x1069: 'PT', 0x106A: 'PT', 0x106B: 'PT', 0x106C: 'PT',
    0x106D: 'PT', 0xAA7B: 'PT', 0x1038: 'SM', 0x1087: 'SM', 0x1088: 'SM', 0x1089: 'SM', 0x108A: 'SM',
    0x108B: 'SM', 0x108C: 'SM', 0x108D: 'SM', 0x108F: 'SM', 0x109A: 'SM', 0x109B: 'SM', 0x109C: 'SM',
    0x104A: 'PLACEHOLDER',
})
position_overrides = {0x0A51: 'BELOW_C', 0x0B01: 'BEFORE_SUB'}


def matra_pos_left(u, block):
    return 'PRE_M'


def matra_pos_right(u, block):
    if block == 'Devanagari': return 'AFTER_SUB'
    if block in ('Bengali', 'Gurmukhi', 'Gujarati', 'Oriya', 'Tamil', 'Malayalam'): return 'AFTER_POST'
    if block == 'Telugu': return 'BEFORE_SUB' if u <= 0x0C42 else 'AFTER_SUB'
    if block == 'Kannada': return 'BEFORE_SUB' if u < 0x0CC3 or u > 0x0CD6 else 'AFTER_SUB'
    return 'AFTER_SUB'


def matra_pos_top(u, block):
    if block == 'Devanagari': return 'AFTER_SUB'
    if block == 'Gurmukhi': return 'AFTER_POST'
    if block == 'Gujarati': return 'AFTER_SUB'
    if block == 'Oriya': return 'AFTER_MAIN'
    if block == 'Tamil': return 'AFTER_SUB'
    if block in ('Telugu', 'Kannada'): return 'BEFORE_SUB'
    return 'AFTER_SUB'


def matra_pos_bottom(u, block):
    if block in ('Devanagari', 'Bengali', 'Oriya'): return 'AFTER_SUB'
    if block in ('Gurmukhi', 'Gujarati', 'Tamil', 'Malayalam'): return 'AFTER_POST'
    if block in ('Telugu', 'Kannada'): return 'BEFORE_SUB'
    return 'AFTER_SUB'


def matra_position(u, pos, block):
    return {'PRE_C': matra_pos_left, 'POST_C': matra_pos_right, 'ABOVE_C': matra_pos_top,
            'BELOW_C': matra_pos_bottom}[pos](u, block)


def read(name):
    data = {}
    for line in open(os.path.join(UCD, name), encoding='utf-8'):
        line = line.split('#')[0]
        fields = [x.strip() for x in line.split(';')]
        if len(fields) < 2:
            continue
        r = fields[0].split('..')
        start = int(r[0], 16)
        end = int(r[-1], 16)
        for u in range(start, end + 1):
            data[u] = fields[1]
    return data


def main():
    syllabic, positional, blocks = read('IndicSyllabicCategory.txt'), read('IndicPositionalCategory.txt'), read('Blocks.txt')
    combined = {}
    for i, d in enumerate((syllabic, positional)):
        for u, v in d.items():
            combined.setdefault(u, ['Other', 'Not_Applicable', blocks.get(u, 'No_Block')])[i] = v
    combined = {u: v for u, v in combined.items() if u in ALLOWED_SINGLES or v[2] in ALLOWED_BLOCKS}
    data = {}
    for u, (cat, pos, block) in combined.items():
        cat = category_map[cat]
        if cat == 'SM' and pos == 'Not_Applicable':
            cat = 'SMPst'
        data[u] = (cat, position_map[pos], block)
    for u, cat in category_overrides.items():
        _, pos, _ = data.get(u, ('X', 'END', ''))
        data[u] = (cat, pos, blocks.get(u, 'No_Block'))
    for u, (cat, pos, block) in data.items():
        if cat not in ('CM', 'SM', 'RS', 'H', 'M', 'MPst'):
            data[u] = (cat, 'END', block)
    for u, (cat, pos, block) in data.items():
        if cat in ('C', 'CS', 'Ra', 'CM', 'V', 'PLACEHOLDER', 'DOTTEDCIRCLE'):
            pos = 'BASE_C'
        elif cat in ('M', 'MPst'):
            if block.startswith('Khmer') or block.startswith('Myanmar'):
                cat = {'PRE_C': 'VPre', 'ABOVE_C': 'VAbv', 'BELOW_C': 'VBlw', 'POST_C': 'VPst'}[pos]
            else:
                pos = matra_position(u, pos, block)
        elif cat in ('SM', 'SMPst', 'VD', 'A', 'Symbol'):
            pos = 'SMVD'
        data[u] = (cat, pos, block)
    for u, pos in position_overrides.items():
        cat, _, _ = data.get(u, ('X', 'END', ''))
        data[u] = (cat, pos, blocks.get(u, 'No_Block'))

    values = {u: CATEGORY[c] | (POSITION[p] << 8) for u, (c, p, _) in data.items()}
    default = CATEGORY['X'] | (POSITION['END'] << 8)
    palette = [default] + sorted(set(values.values()) - {default})
    ranges, prev = [], None
    for u in range(0x110000):
        v = palette.index(values[u]) if u in values else 0
        if v != prev:
            ranges.append((u, v))
            prev = v
    print('// The Indic, Khmer and Myanmar shapers\' categories and positions, as')
    print('// HarfBuzz derives them from IndicSyllabicCategory and IndicPositionalCategory:')
    print('// a palette of category | position << 8, and each range\'s entry in it.')
    print('static const uint16_t kIndicPalette[] = {')
    line = '   '
    for v in palette:
        item = ' 0x%04X,' % v
        if len(line) + len(item) > 98:
            print(line)
            line = '   '
        line += item
    print(line)
    print('};')
    print('static const uint32_t kIndicRanges[] = {')
    line = '   '
    for u, v in ranges:
        item = ' 0x%X | (%du << 24),' % (u, v)
        if len(line) + len(item) > 98:
            print(line)
            line = '   '
        line += item
    print(line)
    print('};')

    clusters = []
    here = os.path.dirname(os.path.abspath(__file__))
    for line in open(os.path.join(here, 'ms-use', 'IndicShapingInvalidCluster.txt'), encoding='utf-8'):
        line = line.split('#')[0].strip()
        if not line:
            continue
        seq = [int(x, 16) for x in line.split(';')[0].split()]
        assert 2 <= len(seq) <= 3
        clusters.append(seq + [0] * (3 - len(seq)))
    print("// Microsoft's IndicShapingInvalidCluster.txt (MIT): sequences of a vowel")
    print('// letter and signs that look like another letter; 0 ends a pair.')
    print('static const uint32_t kInvalidClusters[][3] = {')
    line = '   '
    for seq in clusters:
        item = ' {0x%X, 0x%X, 0x%X},' % tuple(seq)
        if len(line) + len(item) > 98:
            print(line)
            line = '   '
        line += item
    print(line)
    print('};')


main()
