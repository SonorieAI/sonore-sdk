# SPDX-License-Identifier: Apache-2.0
# Builds the variable-font fixture sdk_tests reads, from nothing: every
# outline below is written out here, so no font's licence comes along.
#
#   python make_variable.py        (needs fontTools)
#
# tiny_var.ttf  a TrueType font with one axis, wght 100..900 (default 400),
#               built by fontTools' varLib from three masters: gvar deltas
#               that varLib has thinned to the points interpolation cannot
#               infer, HVAR advances, and an avar map that bends the axis
#               (600 sits at 700's place).
#   A       a stroked A whose stems thicken with the weight
#   O       a ring of two quadratic contours made only of off-curve points;
#           its counter shrinks with the weight
#   acute   a slanted accent
#   Aacute  A and acute as components, the accent's offset moving with the
#           weight
#   Asmall  A as one component scaled to half (WE_HAVE_A_SCALE)
#   I       a bar with a point halfway up each side: varLib lists only the
#           corners, and the middles are inferred -- the interpolation of
#           unlisted points
#   period  a square that rises with the weight without changing shape:
#           one point listed, the rest moving with it
#
# tiny_var_expected.txt  what fontTools draws from it: for six weights, the
# normalised location as the font stores it (1/16384ths), and per glyph its
# advance and every point of its outline. sdk_tests draws the same glyphs at
# the same locations and wants the same points.
import os
import tempfile
from fontTools import varLib
from fontTools.designspaceLib import AxisDescriptor, DesignSpaceDocument, SourceDescriptor
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.basePen import BasePen
from fontTools.pens.recordingPen import DecomposingRecordingPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTFont

HERE = os.path.dirname(os.path.abspath(__file__))
ORDER = ['.notdef', 'A', 'O', 'acute', 'Aacute', 'Asmall', 'I', 'period']


def master(w):
    """One master, its stems w units thick."""
    fb = FontBuilder(1000, isTTF=True)
    fb.setupGlyphOrder(ORDER)
    fb.setupCharacterMap({0x41: 'A', 0x4F: 'O', 0xB4: 'acute', 0xC1: 'Aacute', 0x61: 'Asmall',
                          0x49: 'I', 0x2E: 'period'})
    glyphs, metrics = {}, {}

    pen = TTGlyphPen(None)
    for x0, y0, x1, y1 in ((50, 0, 450, 700), (100, 50, 400, 650)):
        pen.moveTo((x0, y0)); pen.lineTo((x0, y1)); pen.lineTo((x1, y1)); pen.lineTo((x1, y0))
        pen.closePath()
    glyphs['.notdef'] = pen.glyph()
    metrics['.notdef'] = (500, 50)

    # A: two slanted stems and a bar, one contour each.
    width = 600 + w
    pen = TTGlyphPen(None)
    pen.moveTo((0, 0)); pen.lineTo((width / 2 - w / 2, 700)); pen.lineTo((width / 2 + w / 2, 700))
    pen.lineTo((w, 0)); pen.closePath()
    pen.moveTo((width - w, 0)); pen.lineTo((width / 2 - w / 2, 700)); pen.lineTo((width / 2 + w / 2, 700))
    pen.lineTo((width, 0)); pen.closePath()
    pen.moveTo((150, 200)); pen.lineTo((150, 200 + w)); pen.lineTo((width - 150, 200 + w))
    pen.lineTo((width - 150, 200)); pen.closePath()
    glyphs['A'] = pen.glyph()
    metrics['A'] = (width + 40, 0)

    # O: two rings of off-curve points only, the inner one shrinking.
    r, c = 350, 400
    inner = r - w
    pen = TTGlyphPen(None)
    pen.qCurveTo((c - r, c), (c, c + r), (c + r, c), (c, c - r), None)
    pen.closePath()
    pen.qCurveTo((c - inner, c), (c, c - inner), (c + inner, c), (c, c + inner), None)
    pen.closePath()
    glyphs['O'] = pen.glyph()
    metrics['O'] = (800, 50)

    pen = TTGlyphPen(None)
    pen.moveTo((0, 0)); pen.lineTo((100, 160 + w / 2)); pen.lineTo((100 + w, 160 + w / 2))
    pen.lineTo((w, 0)); pen.closePath()
    glyphs['acute'] = pen.glyph()
    metrics['acute'] = (200 + w, 0)

    pen = TTGlyphPen(glyphs)
    pen.addComponent('A', (1, 0, 0, 1, 0, 0))
    pen.addComponent('acute', (1, 0, 0, 1, width / 2 - 50 - w / 4, 760))
    glyphs['Aacute'] = pen.glyph()
    metrics['Aacute'] = (width + 40, 0)

    pen = TTGlyphPen(glyphs)
    pen.addComponent('A', (0.5, 0, 0, 0.5, 20, 0))
    glyphs['Asmall'] = pen.glyph()
    metrics['Asmall'] = (width // 2 + 60, 20)

    pen = TTGlyphPen(None)
    pen.moveTo((60, 0)); pen.lineTo((60, 350)); pen.lineTo((60, 700))
    pen.lineTo((60 + w, 700)); pen.lineTo((60 + w, 350)); pen.lineTo((60 + w, 0))
    pen.closePath()
    glyphs['I'] = pen.glyph()
    metrics['I'] = (120 + w, 60)

    pen = TTGlyphPen(None)
    y = w // 2
    pen.moveTo((50, y)); pen.lineTo((50, y + 100)); pen.lineTo((150, y + 100)); pen.lineTo((150, y))
    pen.closePath()
    glyphs['period'] = pen.glyph()
    metrics['period'] = (200, 50)

    fb.setupGlyf(glyphs)
    # The left side bearings hmtx states, so they agree with the outlines.
    metrics = {n: (adv, getattr(glyphs[n], 'xMin', 0) or 0) for n, (adv, _) in metrics.items()}
    fb.setupHorizontalMetrics(metrics)
    fb.setupHorizontalHeader(ascent=900, descent=-200)
    fb.setupNameTable({'familyName': 'Sonore Tiny Var', 'styleName': 'W%d' % w})
    fb.setupOS2(sTypoAscender=900, sTypoDescender=-200, usWinAscent=900, usWinDescent=200)
    fb.setupPost()
    return fb.font


class Points(BasePen):
    """Every point of the outline, segment by segment, as the SDK's Path
    holds it: a contour of one point draws nothing and is left out."""

    def __init__(self, glyphSet):
        super().__init__(glyphSet)
        self.points, self.contour = [], []

    def flush(self):
        if len(self.contour) > 1:
            self.points += self.contour
        self.contour = []

    def _moveTo(self, p):
        self.flush()
        self.contour = [p]

    def _lineTo(self, p):
        self.contour.append(p)

    def _qCurveToOne(self, c, p):
        self.contour += [c, p]

    def _curveToOne(self, c1, c2, p):
        self.contour += [c1, c2, p]

    def _closePath(self):
        self.flush()

    def _endPath(self):
        self.flush()


def thin(font, names):
    """Drops every delta interpolation can infer, as a font tool tuned for
    size does -- varLib keeps them when the point list would cost more than
    it saves, and these glyphs are here to make the reader infer."""
    from fontTools.varLib.iup import iup_delta_optimize
    glyf, gvar = font['glyf'], font['gvar']
    for name in names:
        coords, control = glyf._getCoordinatesAndControls(name, font['hmtx'].metrics)
        ends = list(control[1]) # the phantom points it adds itself
        for variation in gvar.variations[name]:
            full = [c if c is not None else (0, 0) for c in variation.coordinates]
            variation.coordinates = iup_delta_optimize(full, coords, ends, tolerance=0.5)


def main():
    doc = DesignSpaceDocument()
    axis = AxisDescriptor()
    axis.tag, axis.name = 'wght', 'Weight'
    axis.minimum, axis.default, axis.maximum = 100, 400, 900
    axis.map = [(100, 100), (400, 400), (600, 700), (900, 900)]
    doc.addAxis(axis)
    with tempfile.TemporaryDirectory() as tmp:
        for design, w in ((100, 40), (400, 90), (900, 200)):
            path = os.path.join(tmp, 'm%d.ttf' % design)
            master(w).save(path)
            source = SourceDescriptor()
            source.path = path
            source.location = {'Weight': design}
            doc.addSource(source)
        vf, _, _ = varLib.build(doc)
        thin(vf, ('I', 'O'))
        out = os.path.join(HERE, 'tiny_var.ttf')
        vf.save(out)

    font = TTFont(out)
    order = font.getGlyphOrder()
    lines = []
    for user in (100, 250, 400, 600, 750, 900):
        normalised = font.normalizeLocation({'wght': user})['wght']
        q = int(round(normalised * 16384))
        lines.append('loc %d %d' % (user, q))
        glyphSet = font.getGlyphSet(location={'wght': q / 16384.0} if q else None, normalized=True)
        for name in ORDER:
            recording = DecomposingRecordingPen(glyphSet)
            glyphSet[name].draw(recording)
            points = Points(glyphSet)
            recording.replay(points)
            points.flush()
            coords = ' '.join('%.4f %.4f' % (x, y) for x, y in points.points)
            lines.append('glyph %d %.4f %d %s' % (order.index(name), glyphSet[name].width,
                                                  len(points.points), coords))
    with open(os.path.join(HERE, 'tiny_var_expected.txt'), 'w', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    print('wrote tiny_var.ttf and tiny_var_expected.txt')


main()
