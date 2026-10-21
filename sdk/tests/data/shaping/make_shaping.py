# SPDX-License-Identifier: Apache-2.0
# Builds the shaping fixture sdk_tests reads, from nothing: every outline
# below is a box written out here, so no font's licence comes along.
#
#   python make_shaping.py        (needs fontTools and uharfbuzz)
#
# tiny_shaping.ttf  Arabic, Hebrew and Latin glyphs with the OpenType layout
#                   a real font has, each piece small enough to see:
#   isol/init/medi/fina   the four Arabic forms, chosen by joining
#   rlig                  lam + alef as one glyph, required
#   liga                  f + i as one glyph
#   calt                  A becomes A.alt before V (a chained context)
#   ccmp                  ij split into i and j (a multiple substitution)
#   kern                  A V as a listed pair, [A A.alt] [f i] as classes
#   mark                  marks on bases, and on each half of lam-alef
#   mkmk                  fatha stacked on shadda
#
# tiny_shaping_plain.ttf  the same glyphs with no GDEF, GSUB or GPOS -- only
#                   an old kern table -- so the glyph classes are guessed
#                   from the characters and every mark is placed by its
#                   combining class on the boxes of the glyphs.
# tiny_shaping_latn.ttf   GPOS for Latin alone: Arabic takes it, as the
#                   fallback script; Hebrew does not, and places its marks
#                   by class.
#
# <font>_expected.txt  what HarfBuzz makes of a list of strings with each:
# per line, the direction, the code points, and every glyph with its cluster,
# advance and offsets. sdk_tests shapes the same strings and wants the same.
import os
import uharfbuzz as hb
from fontTools.feaLib.builder import addOpenTypeFeaturesFromString
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import newTable
from fontTools.ttLib.tables._k_e_r_n import KernTable_format_0

HERE = os.path.dirname(os.path.abspath(__file__))

BASES = ['space', 'beh', 'beh.init', 'beh.medi', 'beh.fina', 'alef', 'alef.fina', 'lam',
         'lam.init', 'lam.medi', 'lam.fina', 'meem', 'meem.init', 'meem.medi', 'meem.fina',
         'hebalef', 'hebbet', 'hebshin', 'A', 'A.alt', 'V', 'f', 'i', 'j', 'ij']
LIGATURES = ['lam_alef', 'lam_alef.fina', 'f_i']
MARKS = ['fatha', 'shadda', 'kasra', 'dagesh', 'qamats', 'acute', 'dotbelow', 'cedilla', 'macron']
# Thai with SARA AM and the private-use forms old Thai fonts shift marks to,
# and Hangul jamo, syllables and a tone mark: after the rest, so that the
# glyphs above keep their numbers.
EXTRA_BASES = ['kokai', 'dodek', 'nonu', 'popla', 'saraaa', 'saraam', 'hgL', 'hgV', 'hgT', 'hgGA',
               'hgGAG', 'hgTone', 'dottedcircle']
EXTRA_MARKS = ['nikhahit', 'maiek', 'maitho', 'uniF706', 'uniF70B']
ORDER = ['.notdef'] + BASES + LIGATURES + MARKS + EXTRA_BASES + EXTRA_MARKS
CMAP = {0x20: 'space', 0x628: 'beh', 0x627: 'alef', 0x644: 'lam', 0x645: 'meem',
        0x5D0: 'hebalef', 0x5D1: 'hebbet', 0x5E9: 'hebshin', 0x41: 'A', 0x56: 'V',
        0x66: 'f', 0x69: 'i', 0x6A: 'j', 0x133: 'ij', 0x64E: 'fatha', 0x651: 'shadda',
        0x650: 'kasra', 0x5BC: 'dagesh', 0x5B8: 'qamats', 0x301: 'acute', 0x323: 'dotbelow',
        0x327: 'cedilla', 0x35E: 'macron',
        0xE01: 'kokai', 0xE14: 'dodek', 0xE19: 'nonu', 0xE1B: 'popla', 0xE32: 'saraaa', 0xE33: 'saraam',
        0xE4D: 'nikhahit', 0xE48: 'maiek', 0xE49: 'maitho', 0xF706: 'uniF706', 0xF70B: 'uniF70B',
        0x1100: 'hgL', 0x1161: 'hgV', 0x11A8: 'hgT', 0xAC00: 'hgGA', 0xAC01: 'hgGAG', 0x302E: 'hgTone',
        0x25CC: 'dottedcircle'}

FEA = '''
languagesystem DFLT dflt;
languagesystem arab dflt;
languagesystem hebr dflt;
languagesystem latn dflt;

table GDEF {
  GlyphClassDef [%s], [%s], [%s], ;
} GDEF;

feature ccmp { sub ij by i j; } ccmp;
feature init { sub beh by beh.init; sub lam by lam.init; sub meem by meem.init; } init;
feature medi { sub beh by beh.medi; sub lam by lam.medi; sub meem by meem.medi; } medi;
feature fina { sub beh by beh.fina; sub alef by alef.fina; sub lam by lam.fina; sub meem by meem.fina; } fina;
feature rlig {
  sub lam.init alef.fina by lam_alef;
  sub lam.medi alef.fina by lam_alef.fina;
  sub lam alef by lam_alef;
} rlig;
feature liga { sub f i by f_i; } liga;
feature calt { sub A' V by A.alt; } calt;
feature kern {
  pos A V -80;
  pos [A A.alt] [f i] 20;
} kern;

markClass [fatha shadda] <anchor 0 600> @TOP;
markClass [kasra qamats dagesh] <anchor 0 -100> @BOTTOM;
feature mark {
  pos base [beh beh.init beh.medi beh.fina meem meem.init meem.medi meem.fina alef alef.fina lam lam.init lam.medi lam.fina hebalef hebbet hebshin]
    <anchor 250 700> mark @TOP <anchor 250 -50> mark @BOTTOM;
  pos ligature [lam_alef lam_alef.fina]
    <anchor 450 800> mark @TOP <anchor 450 -50> mark @BOTTOM
    ligComponent
    <anchor 150 800> mark @TOP <anchor 150 -50> mark @BOTTOM;
} mark;
feature mkmk {
  lookup stack {
    markClass fatha <anchor 0 0> @ONSHADDA;
    pos mark shadda <anchor 0 300> mark @ONSHADDA;
  } stack;
} mkmk;
''' % (' '.join(BASES + EXTRA_BASES), ' '.join(LIGATURES), ' '.join(MARKS + EXTRA_MARKS))

STRINGS = [
    'بمب', 'ب', 'بب', 'مممم', 'لا', 'بلا', 'لاب', 'بَّ', 'بِّ', 'لَا', 'لاَ', 'بَب بُ',
    'لمل', 'ا ب', 'بمبلاب', 'אבש', 'בּ', 'שָ', 'אבּש',
    'fi', 'office', 'AV', 'AVi', 'AfV', 'ij', 'Aij', 'VA', 'A V',
]

# Marks of every placing class on Latin, Arabic and Hebrew letters: above and
# below, stacked, attached, a double mark, and a mark after a space.
FALLBACK_STRINGS = [
    'A\u0301', 'A\u0323', 'A\u0327', 'A\u0301\u0301', 'A\u0323\u0301\u0323', 'AV\u035EV',
    'f\u0327i', 'AV', 'fi', ' \u0301', '\u0301A',
    'بَ', 'بِ', 'بَّ', 'بِّ', 'بمَب', 'لاَ', 'بَب بُ',
    'בּ', 'שָ', 'אבּש', 'בָּ', 'שׁ',
]

# Thai SARA AM, with tone marks before it, and marks shifted to the private
# use area where the font has no Thai GSUB; Hangul composed, decomposed, a
# tone mark before its syllable and one with nothing to sit on.
THAI_HANGUL_STRINGS = [
    'กำ', 'น้ำ', 'ด่ำ', 'ป้', 'น้', 'นำ้',
    '가', '각', '각', '가〮', '가〮',
    '〮', 'A가〮',
]

LATIN_FEA = '''
languagesystem latn dflt;
feature kern { pos A V -80; } kern;
'''


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


def build(layout):
    """The glyphs, as boxes; with the features, or with none of them."""


    fb = FontBuilder(1000, isTTF=True)
    fb.setupGlyphOrder(ORDER)
    fb.setupCharacterMap(CMAP)
    glyphs, metrics = {}, {}
    for k, name in enumerate(ORDER):
        pen = TTGlyphPen(None)
        if name != 'space':
            w = 300 + (k * 37) % 400
            h = 200 + (k * 53) % 500
            pen.moveTo((50, 0)); pen.lineTo((50, h)); pen.lineTo((w, h)); pen.lineTo((w, 0))
            pen.closePath()
        glyphs[name] = pen.glyph()
        advance = 0 if name in MARKS + EXTRA_MARKS else 350 + (k * 41) % 450
        metrics[name] = (advance, 50 if name != 'space' else 0)
    fb.setupGlyf(glyphs)
    fb.setupHorizontalMetrics(metrics)
    fb.setupHorizontalHeader(ascent=900, descent=-200)
    fb.setupNameTable({'familyName': 'Sonore Tiny Shaping', 'styleName': 'Regular'})
    fb.setupOS2(sTypoAscender=900, sTypoDescender=-200, usWinAscent=900, usWinDescent=200)
    fb.setupPost()
    if layout == 'full':
        addOpenTypeFeaturesFromString(fb.font, FEA)
    elif layout == 'latn':
        addOpenTypeFeaturesFromString(fb.font, LATIN_FEA)
    else:
        kern = newTable('kern')
        kern.version = 0
        table = KernTable_format_0()
        table.version, table.coverage, table.format = 0, 1, 0
        table.kernTable = {('A', 'V'): -80, ('f', 'i'): -30}
        kern.kernTables = [table]
        fb.font['kern'] = kern
    return fb.font


def main():
    for name, layout, strings in [('tiny_shaping', 'full', STRINGS + FALLBACK_STRINGS[:6] + THAI_HANGUL_STRINGS),
                                  ('tiny_shaping_plain', 'plain', FALLBACK_STRINGS + THAI_HANGUL_STRINGS),
                                  ('tiny_shaping_latn', 'latn', FALLBACK_STRINGS + THAI_HANGUL_STRINGS)]:
        path = os.path.join(HERE, name + '.ttf')
        build(layout).save(path)
        shapes(path, strings)
        print('wrote %s.ttf and %s_expected.txt' % (name, name))


main()
