# SPDX-License-Identifier: Apache-2.0
# Builds the colour-glyph fixtures sdk_tests reads, from nothing: every
# outline is a box and every bitmap a flat square written out here, so no
# font's licence comes along.
#
#   python make_colour.py        (needs fontTools)
#
# tiny_colr.ttf  COLR version 0 and CPAL, as Windows' Segoe UI Emoji has
#                them: A is three layers -- a red box, a box in the text's
#                own colour, a half-transparent blue box -- over a black
#                silhouette nothing should draw; B has no layers. Two
#                palettes, the second green and yellow.
# tiny_sbix.ttf  sbix, as macOS's Apple Color Emoji has it: A is a PNG in
#                two strikes, red at 20 pixels per em and green at 40,
#                placed 2 pixels right of the pen and 3 below the baseline.
# tiny_cbdt.ttf  CBDT and CBLC, as Linux's Noto Color Emoji has them: A is a
#                blue PNG at 32 pixels per em, its top 12 pixels above the
#                baseline and 1 right of the pen.
import os, struct, zlib
from fontTools.colorLib.builder import buildCOLR, buildCPAL
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib.tables.DefaultTable import DefaultTable
from fontTools.ttLib.tables import _s_b_i_x, sbixGlyph, sbixStrike
from fontTools.ttLib import newTable

HERE = os.path.dirname(os.path.abspath(__file__))


def png(width, height, rgba):
    """A flat square of one colour, as PNG."""
    raw = b''.join(b'\x00' + bytes(rgba) * width for _ in range(height))

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xFFFFFFFF)

    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def box(x0, y0, x1, y1):
    pen = TTGlyphPen(None)
    if x1 > x0:
        pen.moveTo((x0, y0)); pen.lineTo((x0, y1)); pen.lineTo((x1, y1)); pen.lineTo((x1, y0))
        pen.closePath()
    return pen.glyph()


def font(order, outlines, family, lsb=None):
    fb = FontBuilder(1000, isTTF=True)
    fb.setupGlyphOrder(order)
    fb.setupCharacterMap({0x20: 'space', 0x41: 'A', 0x42: 'B'})
    fb.setupGlyf(outlines)
    # Each glyph's left side bearing is where its box begins: a simple
    # glyph is drawn from its bearing.
    fb.setupHorizontalMetrics({name: (1000 if name != 'space' else 500, (lsb or {}).get(name, 0)) for name in order})
    fb.setupHorizontalHeader(ascent=800, descent=-200)
    fb.setupNameTable({'familyName': family, 'styleName': 'Regular'})
    fb.setupOS2(sTypoAscender=800, sTypoDescender=-200, usWinAscent=800, usWinDescent=200)
    fb.setupPost()
    return fb.font


def colr():
    order = ['.notdef', 'space', 'A', 'B', 'red', 'ink', 'blue']
    outlines = {'.notdef': box(0, 0, 0, 0), 'space': box(0, 0, 0, 0), 'A': box(0, 0, 1000, 800),
                'B': box(100, 0, 900, 700), 'red': box(0, 0, 500, 800), 'ink': box(500, 0, 1000, 400),
                'blue': box(500, 400, 1000, 800)}
    f = font(order, outlines, 'Sonore Tiny COLR', {'B': 100, 'ink': 500, 'blue': 500})
    f['COLR'] = buildCOLR({'A': [('red', 0), ('ink', 0xFFFF), ('blue', 1)]}, version=0)
    f['CPAL'] = buildCPAL([[(1.0, 0.0, 0.0, 1.0), (0.0, 0.0, 1.0, 0.5)],
                           [(0.0, 1.0, 0.0, 1.0), (1.0, 1.0, 0.0, 1.0)]])
    return f


def sbix():
    order = ['.notdef', 'space', 'A', 'B']
    f = font(order, {name: box(0, 0, 0, 0) for name in order}, 'Sonore Tiny sbix')
    table = newTable('sbix')
    for ppem, side, colour in ((20, 8, (255, 0, 0, 255)), (40, 16, (0, 255, 0, 255))):
        strike = sbixStrike.Strike(ppem=ppem, resolution=72)
        strike.glyphs['A'] = sbixGlyph.Glyph(glyphName='A', graphicType='png ', originOffsetX=2,
                                              originOffsetY=-3, imageData=png(side, side, colour))
        table.strikes[ppem] = strike
    f['sbix'] = table
    return f


def cbdt():
    order = ['.notdef', 'space', 'A', 'B']
    f = font(order, {name: box(0, 0, 0, 0) for name in order}, 'Sonore Tiny CBDT')
    image = png(10, 16, (0, 0, 255, 255))
    # CBDT 3.0: the header, then glyph A in format 17 -- small metrics
    # (height, width, bearing x, bearing y, advance), the PNG's length, the PNG.
    glyph = struct.pack('>BBbbB', 16, 10, 1, 12, 12) + struct.pack('>I', len(image)) + image
    f_cbdt = DefaultTable('CBDT')
    f_cbdt.data = struct.pack('>HH', 3, 0) + glyph
    # CBLC 3.0: one BitmapSize at 32 ppem whose one index subtable, format 1,
    # gives glyph 2 (A) its offset into CBDT.
    sub = struct.pack('>HHI', 1, 17, 4) + struct.pack('>II', 0, len(glyph))
    array = struct.pack('>HHI', 2, 2, 8)
    line = struct.pack('>bbBbbbbbbbbb', 26, -6, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0)
    size = (struct.pack('>IIII', 8 + 48, len(array) + len(sub), 1, 0) + line + line +
            struct.pack('>HHBBBb', 2, 2, 32, 32, 32, 1))
    f_cblc = DefaultTable('CBLC')
    f_cblc.data = struct.pack('>HHI', 3, 0, 1) + size + array + sub
    f['CBDT'] = f_cbdt
    f['CBLC'] = f_cblc
    return f


def main():
    colr().save(os.path.join(HERE, 'tiny_colr.ttf'))
    sbix().save(os.path.join(HERE, 'tiny_sbix.ttf'))
    cbdt().save(os.path.join(HERE, 'tiny_cbdt.ttf'))
    print('wrote tiny_colr.ttf, tiny_sbix.ttf and tiny_cbdt.ttf')


main()
