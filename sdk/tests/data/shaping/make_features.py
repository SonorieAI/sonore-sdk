# SPDX-License-Identifier: Apache-2.0
# Builds the OpenType features fixtures sdk_tests reads, from nothing: every
# outline is a box written out here, so no font's licence comes along.
#
#   python make_features.py        (needs fontTools and uharfbuzz)
#
# tiny_features.ttf  proportional digits with tabular ones under tnum,
#                    numerators and denominators for numr and dnom and a
#                    fraction slash under frac, f and i ligated by liga, and
#                    two alternates of a under salt.
#
# tiny_repeats.ttf   the same glyphs, and a GSUB that lists things more than
#                    once: two liga features in one language system, of
#                    which only the first counts; a required feature under a
#                    tag nobody asks for, which applies anyway; and a calt
#                    feature of 4,000 lookups, listed 4,000 times over -- a
#                    plan that merged lookups pairwise took minutes on it.
#
# tiny_variants.ttf  the same glyphs, and variation sequences in the cmap
#                    (format 14): a with two selectors taking its two
#                    alternates, zero with one its default glyph, one with
#                    an ideographic selector its tabular form.
#
# tiny_*_expected.txt  what HarfBuzz makes of each string with the features
# given: per line the features (tag=value,... or -), the direction, the code
# points, and every glyph with its cluster, advance and offsets. sdk_tests
# shapes the same and wants the same.
import os
import uharfbuzz as hb
from fontTools.feaLib.builder import addOpenTypeFeaturesFromString
from fontTools.fontBuilder import FontBuilder
from fontTools.otlLib import builder as otl
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import newTable
from fontTools.ttLib.tables import otTables as ot

HERE = os.path.dirname(os.path.abspath(__file__))

DIGITS = ['zero', 'one', 'two', 'three', 'four', 'five', 'six', 'seven', 'eight', 'nine']
ORDER = (['.notdef', 'space', 'fraction', 'f', 'i', 'a', 'f_i', 'a.salt1', 'a.salt2'] + DIGITS +
         [d + '.tnum' for d in DIGITS] + [d + '.numr' for d in DIGITS] + [d + '.dnom' for d in DIGITS])
CMAP = {0x20: 'space', 0x2044: 'fraction', 0x66: 'f', 0x69: 'i', 0x61: 'a'}
CMAP.update({0x30 + k: d for k, d in enumerate(DIGITS)})

FEA = '''
languagesystem DFLT dflt;
languagesystem latn dflt;
feature tnum { sub [%(digits)s] by [%(tnum)s]; } tnum;
feature numr { sub [%(digits)s] by [%(numr)s]; } numr;
feature dnom { sub [%(digits)s] by [%(dnom)s]; } dnom;
feature frac { sub fraction by fraction; } frac;
feature liga { sub f i by f_i; } liga;
feature salt { sub a from [a.salt1 a.salt2]; } salt;
''' % {'digits': ' '.join(DIGITS), 'tnum': ' '.join(d + '.tnum' for d in DIGITS),
       'numr': ' '.join(d + '.numr' for d in DIGITS), 'dnom': ' '.join(d + '.dnom' for d in DIGITS)}

CASES = [
    ('-', '0123456789'), ('tnum=1', '0123456789'), ('tnum=1', '11.5'), ('-', '1⁄2'), ('-', '12⁄345'),
    ('frac=0', '1⁄2'), ('numr=0,dnom=0', '1⁄2'), ('-', '1⁄'), ('-', 'fi'), ('liga=0', 'fi'),
    ('salt=1', 'a'), ('salt=2', 'a'), ('salt=3', 'a'), ('salt=0', 'a'), ('tnum=1,liga=0', 'fi 10'),
    ('tnum=1,salt=2', 'a1⁄2'),
]

REPEATS = 4000
VARIANTS = [(0x61, 0xFE00, 'a.salt1'), (0x61, 0xFE01, 'a.salt2'), (0x30, 0xFE00, None),
            (0x31, 0xE0100, 'one.tnum'), (0x31, 0xFE0E, None)]
VARIANT_CASES = [
    ('-', 'a︀'), ('-', 'a︁'), ('-', 'a︎'), ('-', '0︀'), ('-', '1󠄀'),
    ('-', 'a︀︁'), ('-', 'fi︀a︁'), ('-', '1︎⁄2︀'), ('tnum=1', '1󠄀 1'),
    ('-', '︀a'), ('salt=2', 'a︀ a'),
]
REPEAT_CASES = [
    ('-', 'fia'), ('-', '0a1'), ('-', 'fi01a 10'), ('liga=0', 'fia'), ('calt=0', '01'), ('-', 'a'),
]


def glyphs(uvs=None):
    fb = FontBuilder(1000, isTTF=True)
    fb.setupGlyphOrder(ORDER)
    fb.setupCharacterMap(CMAP, uvs=uvs)
    outlines, metrics = {}, {}
    for k, name in enumerate(ORDER):
        pen = TTGlyphPen(None)
        if name != 'space':
            w = 200 + (k * 37) % 300
            h = 200 + (k * 53) % 500
            pen.moveTo((40, 0)); pen.lineTo((40, h)); pen.lineTo((w, h)); pen.lineTo((w, 0))
            pen.closePath()
        outlines[name] = pen.glyph()
        advance = 600 if name.endswith('.tnum') else 300 + (k * 41) % 400
        metrics[name] = (advance, 40 if name != 'space' else 0)
    fb.setupGlyf(outlines)
    fb.setupHorizontalMetrics(metrics)
    fb.setupHorizontalHeader(ascent=900, descent=-200)
    return fb


def finish(fb, family):
    fb.setupNameTable({'familyName': family, 'styleName': 'Regular'})
    fb.setupOS2(sTypoAscender=900, sTypoDescender=-200, usWinAscent=900, usWinDescent=200)
    fb.setupPost()
    return fb.font


def build():
    fb = glyphs()
    font = finish(fb, 'Sonore Tiny Features')
    addOpenTypeFeaturesFromString(font, FEA)
    return font


def build_variants():
    font = finish(glyphs(VARIANTS), 'Sonore Tiny Variants')
    addOpenTypeFeaturesFromString(font, FEA)
    return font


def build_repeats():
    font = finish(glyphs(), 'Sonore Tiny Repeats')

    def single(mapping):
        return otl.buildLookup([otl.buildSingleSubstSubtable(mapping)])

    def feature(tag, indices):
        record = ot.FeatureRecord()
        record.FeatureTag = tag
        record.Feature = ot.Feature()
        record.Feature.FeatureParams = None
        record.Feature.LookupListIndex = list(indices)
        record.Feature.LookupCount = len(record.Feature.LookupListIndex)
        return record

    # 0: f i ligated; 1: a to its second alternate; 2: zero to its
    # denominator; 3 on: one to its tabular form, the same table every time.
    tabular = single({'one': 'one.tnum'})
    lookups = ot.LookupList()
    lookups.Lookup = [otl.buildLookup([otl.buildLigatureSubstSubtable({('f', 'i'): 'f_i'})]),
                      single({'a': 'a.salt2'}), single({'zero': 'zero.dnom'})] + [tabular] * REPEATS
    lookups.LookupCount = len(lookups.Lookup)
    features = ot.FeatureList()
    features.FeatureRecord = [feature('calt', range(3, 3 + REPEATS)), feature('liga', [0]),
                              feature('liga', [1]), feature('zzzz', [2])]
    features.FeatureCount = len(features.FeatureRecord)
    system = ot.LangSys()
    system.LookupOrder = None
    system.ReqFeatureIndex = 3
    system.FeatureIndex = [1, 2] + [0] * REPEATS
    system.FeatureCount = len(system.FeatureIndex)
    script = ot.Script()
    script.DefaultLangSys = system
    script.LangSysRecord = []
    script.LangSysCount = 0
    scripts = ot.ScriptList()
    scripts.ScriptRecord = []
    for tag in ('DFLT', 'latn'):
        record = ot.ScriptRecord()
        record.ScriptTag = tag
        record.Script = script
        scripts.ScriptRecord.append(record)
    scripts.ScriptCount = len(scripts.ScriptRecord)
    table = ot.GSUB()
    table.Version = 0x00010000
    table.ScriptList = scripts
    table.FeatureList = features
    table.LookupList = lookups
    font['GSUB'] = newTable('GSUB')
    font['GSUB'].table = table
    return font


def expect(path, cases, out):
    font = hb.Font(hb.Face(hb.Blob.from_file_path(path)))
    lines = []
    for features, text in cases:
        given = {}
        if features != '-':
            for item in features.split(','):
                tag, value = item.split('=')
                given[tag] = int(value)
        buf = hb.Buffer()
        buf.add_str(text)
        buf.guess_segment_properties()
        hb.shape(font, buf, given)
        rtl = 1 if buf.direction == 'rtl' else 0
        glyphs = ';'.join('%d,%d,%d,%d,%d' % (i.codepoint, i.cluster, p.x_advance, p.x_offset, p.y_offset)
                          for i, p in zip(buf.glyph_infos, buf.glyph_positions))
        lines.append('%s %d %s | %s' % (features, rtl, ' '.join('%X' % ord(c) for c in text), glyphs))
    with open(os.path.join(HERE, out), 'w', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')


def main():
    path = os.path.join(HERE, 'tiny_features.ttf')
    build().save(path)
    expect(path, CASES, 'tiny_features_expected.txt')
    path = os.path.join(HERE, 'tiny_repeats.ttf')
    build_repeats().save(path)
    expect(path, REPEAT_CASES, 'tiny_repeats_expected.txt')
    path = os.path.join(HERE, 'tiny_variants.ttf')
    build_variants().save(path)
    expect(path, VARIANT_CASES, 'tiny_variants_expected.txt')
    print('wrote tiny_features.ttf, tiny_repeats.ttf, tiny_variants.ttf and what HarfBuzz makes of them')


main()
