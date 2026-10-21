# SPDX-License-Identifier: Apache-2.0
# Builds the syllabic shaping fixtures sdk_tests reads, from nothing: every
# outline is a box written out here, so no font's licence comes along.
#
#   python make_indic.py        (needs fontTools and uharfbuzz)
#
# tiny_indic2.ttf   Devanagari under 'dev2', the current specification:
#                   reph, half forms, a below-base Ra, a conjunct, a left
#                   matra that moves past half forms, marks above.
# tiny_indic.ttf    the same under 'deva', the old one, where a halant after
#                   the base moves after the last consonant and the
#                   below-base Ra is looked for the other way round.
# tiny_khmer.ttf    Khmer: a Coeng Ro moved before the base (pref), a
#                   subscript consonant, a split vowel, a left vowel.
# tiny_myanmar.ttf  Myanmar under 'mym2': kinzi, the medial Ra and the left
#                   vowel before the base, the asat.
# tiny_use.ttf      Javanese, through the Universal Shaping Engine: a
#                   pangkon and consonant made a subscript, the taling moved
#                   before its syllable, a repha moved after the base.
#
# <font>_expected.txt  what HarfBuzz makes of a list of strings with each:
# per line, the direction, the code points, and every glyph with its
# cluster, advance and offsets. sdk_tests shapes the same strings.
import os
import uharfbuzz as hb
from fontTools.feaLib.builder import addOpenTypeFeaturesFromString
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen

HERE = os.path.dirname(os.path.abspath(__file__))

DEVA_CMAP = {0x20: 'space', 0x25CC: 'dottedcircle', 0x200C: 'zwnj', 0x200D: 'zwj', 0x0905: 'a',
             0x0915: 'ka', 0x0916: 'kha', 0x0937: 'ssa', 0x0930: 'ra', 0x0928: 'na', 0x094D: 'virama',
             0x093F: 'imatra', 0x0940: 'iimatra', 0x0941: 'umatra', 0x0947: 'ematra', 0x093E: 'aamatra',
             0x0902: 'anusvara', 0x093C: 'nukta'}
DEVA_EXTRA = ['ka.half', 'kha.half', 'ssa.half', 'na.half', 'reph', 'rakar', 'k_ssa', 'ka_rakar',
              'imatra.init', 'ematra_reph']
DEVA_MARKS = ['virama', 'umatra', 'ematra', 'anusvara', 'nukta', 'reph', 'rakar', 'ematra_reph']


def deva_fea(script, old):
    # The old specification has the halant after the consonant it kills.
    blwf = 'sub ra virama by rakar;' if old else 'sub virama ra by rakar;'
    return '''
languagesystem DFLT dflt;
languagesystem %s dflt;
table GDEF {
  GlyphClassDef [ka kha ssa ra na a dottedcircle ka.half kha.half ssa.half na.half k_ssa ka_rakar imatra imatra.init iimatra aamatra], , [%s], ;
} GDEF;
feature rphf { sub ra virama by reph; } rphf;
feature blwf { %s } blwf;
feature half { sub ka virama by ka.half; sub kha virama by kha.half; sub ssa virama by ssa.half; sub na virama by na.half; } half;
feature pres { sub ka.half ssa by k_ssa; sub ka rakar by ka_rakar; } pres;
feature abvs { sub ematra reph by ematra_reph; } abvs;
feature init { sub imatra by imatra.init; } init;
markClass [anusvara ematra reph ematra_reph] <anchor 0 0> @TOP;
markClass [umatra nukta rakar] <anchor 0 0> @BOTTOM;
feature abvm {
  pos base [ka kha ssa ra na a dottedcircle k_ssa ka_rakar] <anchor 300 700> mark @TOP;
} abvm;
feature blwm {
  pos base [ka kha ssa ra na a dottedcircle k_ssa ka_rakar] <anchor 250 -60> mark @BOTTOM;
} blwm;
''' % (script, ' '.join(DEVA_MARKS), blwf)


DEVA_STRINGS = [
    'कि', 'कि ', 'र्क', 'र्कि', 'क्ष', 'क्षि', 'क्र', 'क्रि', 'ख्क', 'ख्कि', 'र्क्ष', 'र्क्षे', 'कें', 'कुं', 'क़',
    'क्‌ष', 'क्‍ष', 'क्‍षि', 'र्‍क', 'न्क्षि', 'अ', 'अं', 'ि', 'र्', 'क्', 'किं', 'के र्के',
    'क्षे', 'ख्ख्ख', 'र्र्क',
]

KHMER_CMAP = {0x20: 'space', 0x25CC: 'dottedcircle', 0x1780: 'ka', 0x1781: 'kha', 0x179A: 'ro', 0x17D2: 'coeng',
              0x17C1: 'e', 0x17B6: 'aa', 0x17BE: 'oe', 0x17C6: 'nikahit', 0x17BB: 'u'}
KHMER_EXTRA = ['ro.sub', 'ka.sub', 'kha.sub', 'oe.part', 'e_aa']
KHMER_MARKS = ['coeng', 'nikahit', 'u', 'ka.sub', 'kha.sub', 'oe']
KHMER_FEA = '''
languagesystem DFLT dflt;
languagesystem khmr dflt;
table GDEF {
  GlyphClassDef [ka kha ro dottedcircle e aa ro.sub oe.part e_aa], , [coeng nikahit u ka.sub kha.sub oe], ;
} GDEF;
feature pref { sub coeng ro by ro.sub; } pref;
feature blwf { sub coeng ka by ka.sub; sub coeng kha by kha.sub; } blwf;
feature pres { sub e aa by e_aa; } pres;
markClass [nikahit oe] <anchor 0 0> @TOP;
markClass [u ka.sub kha.sub] <anchor 0 0> @BOTTOM;
feature abvm { pos base [ka kha ro dottedcircle] <anchor 300 700> mark @TOP; } abvm;
feature blwm { pos base [ka kha ro dottedcircle] <anchor 250 -60> mark @BOTTOM; } blwm;
'''
KHMER_STRINGS = ['ក', 'កេ', 'ក្រ', 'ក្រេ', 'ក្ក', 'ក្ខេ', 'កើ', 'ក្រើ', 'កា', 'កេា', 'កំ', 'កុំ', 'ក្ក្រ', 'េ',
                 'ក្', 'ក ្រ']

MYANMAR_CMAP = {0x20: 'space', 0x25CC: 'dottedcircle', 0x1000: 'ka', 0x1001: 'kha', 0x1004: 'nga', 0x101B: 'ra',
                0x103A: 'asat', 0x1039: 'virama', 0x103C: 'medialra', 0x1031: 'e', 0x102C: 'aa', 0x102F: 'u',
                0x1036: 'anusvara', 0x103B: 'medialya', 0x1037: 'dotbelow', 0x1038: 'visarga'}
MYANMAR_EXTRA = ['kinzi', 'ka.sub', 'ka_medialra', 'e.alt']
MYANMAR_MARKS = ['asat', 'virama', 'u', 'anusvara', 'dotbelow', 'kinzi', 'ka.sub']
MYANMAR_FEA = '''
languagesystem DFLT dflt;
languagesystem mym2 dflt;
table GDEF {
  GlyphClassDef [ka kha nga ra dottedcircle medialra e aa medialya visarga ka_medialra e.alt], , [asat virama u anusvara dotbelow kinzi ka.sub], ;
} GDEF;
feature rphf { sub nga asat virama by kinzi; } rphf;
feature blwf { sub virama ka by ka.sub; } blwf;
feature pres { sub medialra ka by ka_medialra; sub e' ka_medialra by e.alt; } pres;
markClass [anusvara kinzi asat] <anchor 0 0> @TOP;
markClass [u dotbelow ka.sub] <anchor 0 0> @BOTTOM;
feature abvm { pos base [ka kha nga ra dottedcircle ka_medialra] <anchor 300 700> mark @TOP; } abvm;
feature blwm { pos base [ka kha nga ra dottedcircle ka_medialra] <anchor 250 -60> mark @BOTTOM; } blwm;
'''
MYANMAR_STRINGS = ['က', 'ကေ', 'ကြ', 'ကြေ', 'င်္က', 'င်္ကေ', 'က္က', 'ကုံ', 'ကို', 'ကျော်', 'ကော့', 'ကေး', 'ေ', 'ကြေ ကေ',
                   'ငျ်', 'ကါ']


JAVA_CMAP = {0x20: 'space', 0x25CC: 'dottedcircle', 0xA98F: 'ka', 0xA9A4: 'na', 0xA9AB: 'ra', 0xA9C0: 'pangkon',
             0xA9BA: 'taling', 0xA9B4: 'tarung', 0xA981: 'cecak', 0xA983: 'wignyan', 0xA9B8: 'suku', 0xA982: 'layar'}
JAVA_EXTRA = ['na.sub', 'ka.sub', 'ka_tarung']
JAVA_MARKS = ['pangkon', 'cecak', 'suku', 'na.sub', 'ka.sub', 'layar']
JAVA_FEA = '''
languagesystem DFLT dflt;
languagesystem java dflt;
table GDEF {
  GlyphClassDef [ka na ra dottedcircle taling tarung wignyan ka_tarung], , [pangkon cecak suku na.sub ka.sub layar], ;
} GDEF;
feature blwf { sub pangkon na by na.sub; sub pangkon ka by ka.sub; } blwf;
feature pres { sub ka tarung by ka_tarung; } pres;
markClass [cecak layar] <anchor 0 0> @TOP;
markClass [suku na.sub ka.sub] <anchor 0 0> @BOTTOM;
feature abvm { pos base [ka na ra dottedcircle ka_tarung] <anchor 300 700> mark @TOP; } abvm;
feature blwm { pos base [ka na ra dottedcircle ka_tarung] <anchor 250 -60> mark @BOTTOM; } blwm;
'''
JAVA_STRINGS = ['ꦏ', 'ꦏꦺ', 'ꦏ꧀ꦤ', 'ꦏ꧀ꦤꦺ', 'ꦏꦴ',
                'ꦏꦺꦴ', 'ꦏꦁ', 'ꦏꦸꦁ', 'ꦺ', '꧀',
                'ꦏꦂ', 'ꦏ꧀ꦏꦺꦃ', 'ꦏ ꦺꦤ', 'ꦫ꧀ꦏ']


def build(cmap, extra, marks, fea):
    order = ['.notdef'] + sorted(set(cmap.values())) + extra
    fb = FontBuilder(1000, isTTF=True)
    fb.setupGlyphOrder(order)
    fb.setupCharacterMap(cmap)
    glyphs, metrics = {}, {}
    for k, name in enumerate(order):
        pen = TTGlyphPen(None)
        if name not in ('space', 'zwj', 'zwnj'):
            w = 300 + (k * 37) % 400
            h = 200 + (k * 53) % 500
            pen.moveTo((50, 0)); pen.lineTo((50, h)); pen.lineTo((w, h)); pen.lineTo((w, 0))
            pen.closePath()
        glyphs[name] = pen.glyph()
        advance = 0 if name in marks or name in ('zwj', 'zwnj') else 350 + (k * 41) % 450
        metrics[name] = (advance, 50 if name not in ('space', 'zwj', 'zwnj') else 0)
    fb.setupGlyf(glyphs)
    fb.setupHorizontalMetrics(metrics)
    fb.setupHorizontalHeader(ascent=900, descent=-200)
    fb.setupNameTable({'familyName': 'Sonore Tiny Syllables', 'styleName': 'Regular'})
    fb.setupOS2(sTypoAscender=900, sTypoDescender=-200, usWinAscent=900, usWinDescent=200)
    fb.setupPost()
    addOpenTypeFeaturesFromString(fb.font, fea)
    return fb.font


def shapes(path, strings):
    font = hb.Font(hb.Face(hb.Blob.from_file_path(path)))
    lines = []
    for text in strings:
        buf = hb.Buffer()
        buf.add_str(text)
        buf.guess_segment_properties()
        hb.shape(font, buf, {})
        rtl = 1 if buf.direction == 'rtl' else 0
        glyphs = ';'.join('%d,%d,%d,%d,%d' % (i.codepoint, i.cluster, p.x_advance, p.x_offset, p.y_offset)
                          for i, p in zip(buf.glyph_infos, buf.glyph_positions))
        lines.append('%d %s | %s' % (rtl, ' '.join('%X' % ord(c) for c in text), glyphs))
    with open(path[:-4] + '_expected.txt', 'w', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')


def main():
    fonts = [('tiny_indic2', DEVA_CMAP, DEVA_EXTRA, DEVA_MARKS, deva_fea('dev2', False), DEVA_STRINGS),
             ('tiny_indic', DEVA_CMAP, DEVA_EXTRA, DEVA_MARKS, deva_fea('deva', True), DEVA_STRINGS),
             ('tiny_khmer', KHMER_CMAP, KHMER_EXTRA, KHMER_MARKS, KHMER_FEA, KHMER_STRINGS),
             ('tiny_myanmar', MYANMAR_CMAP, MYANMAR_EXTRA, MYANMAR_MARKS, MYANMAR_FEA, MYANMAR_STRINGS),
             ('tiny_use', JAVA_CMAP, JAVA_EXTRA, JAVA_MARKS, JAVA_FEA, JAVA_STRINGS)]
    for name, cmap, extra, marks, fea, strings in fonts:
        path = os.path.join(HERE, name + '.ttf')
        build(cmap, extra, marks, fea).save(path)
        shapes(path, strings)
        print('wrote %s.ttf and %s_expected.txt' % (name, name))


main()
