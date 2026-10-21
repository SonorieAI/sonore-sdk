# SPDX-License-Identifier: Apache-2.0
"""Writes the Universal Shaping Engine's character categories for
gfx/text_shaper.h, by HarfBuzz's gen-use-table.py rules (MIT; see
THIRD_PARTY_NOTICES), from the UCD in $SONORE_UCD -- IndicSyllabicCategory,
IndicPositionalCategory, ArabicShaping, DerivedCoreProperties, Blocks and
Scripts, with Python's unicodedata for the general categories -- and
Microsoft's additions in ms-use/.

The categories are numbered as the Universal syllable machine has them
(gen_syllables.py); characters the table does not list are O.
"""
import os
import unicodedata

UCD = os.environ['SONORE_UCD']
HERE = os.path.dirname(os.path.abspath(__file__))

NUMBER = dict(O=0, B=1, N=4, GB=5, CGJ=6, SUB=11, H=12, HN=13, ZWNJ=14, WJ=16, R=18, CS=43, IS=44, Sk=48, G=49,
              J=50, SB=51, SE=52, HVM=53, HM=54, HR=55, RK=56, FAbv=24, FBlw=25, FPst=26, MAbv=27, MBlw=28,
              MPst=29, MPre=30, CMAbv=31, CMBlw=32, VAbv=33, VBlw=34, VPst=35, VPre=22, VMAbv=37, VMBlw=38,
              VMPst=39, VMPre=23, SMAbv=41, SMBlw=42, FMAbv=45, FMBlw=46, FMPst=47)

DISABLED_SCRIPTS = {'Arabic', 'Lao', 'Samaritan', 'Syriac', 'Thai'}


def read(path, field=1, only=None):
    data = {}
    for line in open(path, encoding='utf-8'):
        line = line.split('#')[0]
        fields = [x.strip() for x in line.split(';')]
        if len(fields) <= field:
            continue
        r = fields[0].split('..')
        start, end = int(r[0], 16), int(r[-1], 16)
        value = fields[field]
        if only and value != only:
            continue
        for u in range(start, end + 1):
            data[u] = value
    return data


def main():
    uisc = read(os.path.join(UCD, 'IndicSyllabicCategory.txt'))
    uipc = read(os.path.join(UCD, 'IndicPositionalCategory.txt'))
    # Microsoft's additions, for the characters this version of Unicode has.
    def assigned(u):
        return unicodedata.category(chr(u)) != 'Cn'
    for u, v in read(os.path.join(HERE, 'ms-use', 'IndicSyllabicCategory-Additional.txt')).items():
        if assigned(u):
            uisc[u] = 'Syllable_Modifier' if v == 'Consonant_Final_Modifier' else v
    for u, v in read(os.path.join(HERE, 'ms-use', 'IndicPositionalCategory-Additional.txt')).items():
        if assigned(u):
            uipc[u] = 'Not_Applicable' if v == 'NA' else v
    joining = {u: 'jt_' + v for u, v in read(os.path.join(UCD, 'ArabicShaping.txt'), 2).items()}
    di = read(os.path.join(UCD, 'DerivedCoreProperties.txt'), 1, 'Default_Ignorable_Code_Point')
    blocks = read(os.path.join(UCD, 'Blocks.txt'))
    scripts = read(os.path.join(UCD, 'Scripts.txt'))

    listed = set(uisc) | set(uipc) | set(joining) | set(di)
    data = {}
    for u in listed:
        if scripts.get(u, 'Unknown') in DISABLED_SCRIPTS:
            continue
        data[u] = (uisc.get(u, 'Other'), uipc.get(u, 'Not_Applicable'), joining.get(u, 'jt_X'), u in di,
                   unicodedata.category(chr(u)))

    def is_base(U, UISC, UDI, UGC, AJT):
        return (UISC in ('Number', 'Consonant', 'Consonant_Head_Letter', 'Tone_Letter', 'Vowel_Independent')
                or (AJT in ('jt_C', 'jt_D', 'jt_L', 'jt_R') and UISC != 'Joiner')
                or (UGC == 'Lo' and UISC in ('Avagraha', 'Bindu', 'Consonant_Final', 'Consonant_Medial',
                                             'Consonant_Subjoined', 'Vowel', 'Vowel_Dependent')))

    def is_base_other(U, UISC, UDI, UGC, AJT):
        return UISC == 'Consonant_Placeholder' or U in (0x2015, 0x2022, 0x25FB, 0x25FC, 0x25FD, 0x25FE)

    def is_cgj(U, UISC, UDI, UGC, AJT):
        return UISC == 'Joiner' or (UDI and UGC in ('Mc', 'Me', 'Mn'))

    def is_halant_or_vowel_modifier(U, UISC, UDI, UGC, AJT):
        return U == 0x0DCA

    def is_sakot(U, UISC, UDI, UGC, AJT):
        return U == 0x1A60

    def is_sym_mod(U, UISC, UDI, UGC, AJT):
        return UISC == 'Symbol_Modifier'

    def is_word_joiner(U, UISC, UDI, UGC, AJT):
        return ((UDI and U not in (0x115F, 0x1160, 0x3164, 0xFFA0, 0x1BCA0, 0x1BCA1, 0x1BCA2, 0x1BCA3)
                 and UISC == 'Other' and not is_cgj(U, UISC, UDI, UGC, AJT)) or UGC == 'Cn')

    mapping = {
        'B': is_base,
        'N': lambda U, UISC, UDI, UGC, AJT: UISC == 'Brahmi_Joining_Number',
        'GB': is_base_other,
        'CGJ': is_cgj,
        'F': lambda U, UISC, UDI, UGC, AJT: ((UISC == 'Consonant_Final' and UGC != 'Lo')
                                             or UISC == 'Consonant_Succeeding_Repha'),
        'FM': lambda U, UISC, UDI, UGC, AJT: UISC == 'Syllable_Modifier',
        'M': lambda U, UISC, UDI, UGC, AJT: ((UISC == 'Consonant_Medial' and UGC != 'Lo')
                                             or UISC == 'Consonant_Initial_Postfixed'),
        'CM': lambda U, UISC, UDI, UGC, AJT: UISC in ('Nukta', 'Gemination_Mark', 'Consonant_Killer'),
        'SUB': lambda U, UISC, UDI, UGC, AJT: UISC == 'Consonant_Subjoined' and UGC != 'Lo',
        'CS': lambda U, UISC, UDI, UGC, AJT: UISC == 'Consonant_With_Stacker',
        'H': lambda U, UISC, UDI, UGC, AJT: UISC == 'Virama' and not is_halant_or_vowel_modifier(U, UISC, UDI, UGC, AJT),
        'HVM': is_halant_or_vowel_modifier,
        'HN': lambda U, UISC, UDI, UGC, AJT: UISC == 'Number_Joiner',
        'IS': lambda U, UISC, UDI, UGC, AJT: UISC == 'Invisible_Stacker' and not is_sakot(U, UISC, UDI, UGC, AJT),
        'G': lambda U, UISC, UDI, UGC, AJT: UISC == 'Hieroglyph',
        'HM': lambda U, UISC, UDI, UGC, AJT: UISC == 'Hieroglyph_Modifier',
        'HR': lambda U, UISC, UDI, UGC, AJT: UISC == 'Hieroglyph_Mirror',
        'J': lambda U, UISC, UDI, UGC, AJT: UISC == 'Hieroglyph_Joiner',
        'SB': lambda U, UISC, UDI, UGC, AJT: UISC in ('Hieroglyph_Mark_Begin', 'Hieroglyph_Segment_Begin'),
        'SE': lambda U, UISC, UDI, UGC, AJT: UISC in ('Hieroglyph_Mark_End', 'Hieroglyph_Segment_End'),
        'ZWNJ': lambda U, UISC, UDI, UGC, AJT: UISC == 'Non_Joiner',
        'O': lambda U, UISC, UDI, UGC, AJT: ((UGC == 'Po' or UISC in ('Consonant_Dead', 'Joiner', 'Modifying_Letter', 'Other'))
                                             and not is_base(U, UISC, UDI, UGC, AJT)
                                             and not is_base_other(U, UISC, UDI, UGC, AJT)
                                             and not is_cgj(U, UISC, UDI, UGC, AJT)
                                             and not is_sym_mod(U, UISC, UDI, UGC, AJT)
                                             and not is_word_joiner(U, UISC, UDI, UGC, AJT)),
        'RK': lambda U, UISC, UDI, UGC, AJT: UISC == 'Reordering_Killer',
        'R': lambda U, UISC, UDI, UGC, AJT: UISC in ('Consonant_Preceding_Repha', 'Consonant_Prefixed'),
        'Sk': is_sakot,
        'SM': is_sym_mod,
        'V': lambda U, UISC, UDI, UGC, AJT: (UISC == 'Pure_Killer'
                                             or (UGC != 'Lo' and UISC in ('Vowel', 'Vowel_Dependent'))),
        'VM': lambda U, UISC, UDI, UGC, AJT: (UISC in ('Tone_Mark', 'Cantillation_Mark', 'Register_Shifter', 'Visarga')
                                              or (UGC != 'Lo' and UISC == 'Bindu')),
        'WJ': is_word_joiner,
    }
    positions = {
        'F': {'Abv': ['Top'], 'Blw': ['Bottom'], 'Pst': ['Right']},
        'M': {'Abv': ['Top'], 'Blw': ['Bottom', 'Bottom_And_Left', 'Bottom_And_Right'], 'Pst': ['Right'],
              'Pre': ['Left', 'Top_And_Bottom_And_Left']},
        'CM': {'Abv': ['Top'], 'Blw': ['Bottom', 'Overstruck']},
        'V': {'Abv': ['Top', 'Top_And_Bottom', 'Top_And_Bottom_And_Right', 'Top_And_Right'],
              'Blw': ['Bottom', 'Overstruck', 'Bottom_And_Right'], 'Pst': ['Right'],
              'Pre': ['Left', 'Top_And_Left', 'Top_And_Left_And_Right', 'Left_And_Right']},
        'VM': {'Abv': ['Top'], 'Blw': ['Bottom', 'Overstruck'], 'Pst': ['Right'], 'Pre': ['Left']},
        'SM': {'Abv': ['Top'], 'Blw': ['Bottom']},
        'FM': {'Abv': ['Top'], 'Blw': ['Bottom'], 'Pst': ['Not_Applicable']},
    }
    categories = {}
    for U, (UISC, UIPC, AJT, UDI, UGC) in data.items():
        if 0x1CE2 <= U <= 0x1CE8: UISC = 'Cantillation_Mark'
        if 0x0F18 <= U <= 0x0F19 or 0x0F3E <= U <= 0x0F3F: UISC = 'Vowel_Dependent'
        if U == 0x1CED: UISC = 'Tone_Mark'
        found = [k for k, f in mapping.items() if f(U, UISC, UDI, UGC, AJT)]
        assert len(found) == 1, (hex(U), UISC, UDI, UGC, AJT, found)
        use = found[0]
        if U in (0x11302, 0x11303, 0x114C1): UIPC = 'Top'
        if use in positions:
            where = [k for k, v in positions[use].items() if UIPC in v]
            assert len(where) == 1, (hex(U), UIPC, use, where)
            use += where[0]
        categories[U] = NUMBER[use]

    ranges, prev = [], None
    for u in range(0x110000):
        v = categories.get(u, 0)
        if v != prev:
            ranges.append((u, v))
            prev = v
    print('// The Universal Shaping Engine\'s categories, as HarfBuzz derives them from')
    print('// the UCD and Microsoft\'s additions, numbered as its syllable machine has them.')
    print('static const uint32_t kUseRanges[] = {')
    line = '   '
    for u, v in ranges:
        item = ' 0x%X | (%du << 24),' % (u, v)
        if len(line) + len(item) > 98:
            print(line)
            line = '   '
        line += item
    print(line)
    print('};')


main()
