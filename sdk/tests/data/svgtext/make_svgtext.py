# SPDX-License-Identifier: Apache-2.0
# Builds the SVG text fixtures images_test reads: four small fonts written out
# here from nothing (so no font's licence comes along), SVGs that use them,
# and what resvg draws from each -- resvg is the oracle.
#
#   python make_svgtext.py        (needs fontTools, resvg-py, Pillow, numpy)
#
# The fonts:
#   SonoreText-Regular.ttf  family "Sonore Text", weight 400
#   SonoreText-Bold.ttf     the same family at 700, heavier stems
#   SonoreText-Italic.ttf   the same family at 400, italic, slanted
#   SonoreTextVar.ttf       family "Sonore Text Var", one axis, wght 100-900
# Every glyph is a stem and up to six squares beside it, the squares spelling
# the character's code in binary -- so a wrong glyph, a wrong advance or a
# wrong position shows as ink in the wrong place, which is what the test
# measures. Each glyph's advance differs from its neighbours'.
#
# resvg is given only these fonts (no system fonts), with "Sonore Text" as its
# default and serif family -- and for the variable family, STATIC instances at
# 300, 400 and 700 cut by fontTools' instancer, so what it draws is the right
# outline at each weight whether or not it applies a weight axis itself. The
# SDK draws the same SVGs from the variable font, on its axis.
#
# resvg 0.48 draws nothing for a font-size with a unit in it (font-size="18pt"),
# where a browser draws 24px; the fixtures give sizes in px, and images_test
# checks pt against px itself. It also drops a space at the start of a <tspan>
# where browsers keep it ("and<tspan> span</tspan>" draws "andspan"), so the
# whitespace fixture keeps its spaces outside its spans.
#
# svgtext_expected.txt lists each SVG with its size; <name>.png beside it is
# resvg's picture.
import io
import os
import tempfile

import numpy as np
import resvg_py
from fontTools import varLib
from fontTools.designspaceLib import AxisDescriptor, DesignSpaceDocument, SourceDescriptor
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
CHARS = ' .,!?-0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz'


def name_of(ch):
    return 'space' if ch == ' ' else 'g%02X' % ord(ch)


def rect(pen, x0, y0, x1, y1, slant):
    s = lambda x, y: (round(x + y * slant), round(y))
    pen.moveTo(s(x0, y0))
    pen.lineTo(s(x0, y1))
    pen.lineTo(s(x1, y1))
    pen.lineTo(s(x1, y0))
    pen.closePath()


def build(family, style, weight, bar, slant=0.0, italic=False):
    fb = FontBuilder(1000, isTTF=True)
    order = ['.notdef'] + [name_of(c) for c in CHARS]
    fb.setupGlyphOrder(order)
    fb.setupCharacterMap({ord(c): name_of(c) for c in CHARS})
    glyphs, metrics = {}, {}
    pen = TTGlyphPen(None)
    rect(pen, 50, 0, 450, 700, slant)
    glyphs['.notdef'] = pen.glyph()
    metrics['.notdef'] = (500, 50)
    for i, ch in enumerate(CHARS):
        pen = TTGlyphPen(None)
        if ch == ' ':
            glyphs['space'] = pen.glyph()
            metrics['space'] = (280 + bar // 4, 0)
            continue
        h = 700 if ch.isupper() else (650 if ch.isdigit() else (500 if ch.islower() else 160))
        rect(pen, 50, 0, 50 + bar, h, slant)
        code = ord(ch) - 32
        x0, sq = 50 + bar + 40, 80 + bar // 2
        for b in range(6):
            if (code >> b) & 1:
                rect(pen, x0, b * h // 6 + 10, x0 + sq, (b + 1) * h // 6 - 10, slant)
        glyphs[name_of(ch)] = pen.glyph()
        metrics[name_of(ch)] = (x0 + sq + 50 + (i % 3) * 40, 50)
    fb.setupGlyf(glyphs)
    metrics = {n: (adv, getattr(glyphs[n], 'xMin', 0) or 0) for n, (adv, _) in metrics.items()}
    fb.setupHorizontalMetrics(metrics)
    fb.setupHorizontalHeader(ascent=900, descent=-200)
    # The whole name table: resvg's font database refuses a face without
    # its full and PostScript names, and calls it nameless.
    ps = (family + '-' + style).replace(' ', '')
    fb.setupNameTable({'familyName': family, 'styleName': style, 'fullName': family + ' ' + style, 'psName': ps,
                       'uniqueFontIdentifier': ps + '-1.000', 'version': 'Version 1.000'})
    fb.font['head'].macStyle = (1 if weight >= 700 else 0) | (2 if italic else 0)
    selection = (0x01 if italic else 0) | (0x20 if weight >= 700 else 0) | (0x40 if not italic and weight < 700 else 0)
    fb.setupOS2(sTypoAscender=900, sTypoDescender=-200, sTypoLineGap=0, usWinAscent=900, usWinDescent=200,
                usWeightClass=weight, fsSelection=selection)
    fb.setupPost()
    return fb.font


SVGS = {
    'basic': (200, 40, '''<svg xmlns="http://www.w3.org/2000/svg" width="200" height="40" viewBox="0 0 200 40">
  <text x="4" y="28" font-family="Sonore Text" font-size="22">Hello, SVG 42!</text>
</svg>'''),
    'anchor': (180, 90, '''<svg xmlns="http://www.w3.org/2000/svg" width="180" height="90" viewBox="0 0 180 90">
  <g font-family="Sonore Text" font-size="18" fill="#204080">
    <text x="90" y="24" text-anchor="start">Start</text>
    <text x="90" y="50" text-anchor="middle">Middle</text>
    <text x="90" y="76" text-anchor="end">End</text>
  </g>
</svg>'''),
    'tspans': (220, 50, '''<svg xmlns="http://www.w3.org/2000/svg" width="220" height="50" viewBox="0 0 220 50">
  <text x="4" y="34" font-family="Sonore Text" font-size="20">A<tspan fill="#c00020" dy="-6">bc</tspan><tspan
    dx="8" font-size="28">DE</tspan> f<tspan x="150 166 182" fill="#008040">xyz</tspan></text>
</svg>'''),
    'whitespace': (220, 96, '''<svg xmlns="http://www.w3.org/2000/svg" width="220" height="96" viewBox="0 0 220 96">
  <text x="4" y="26" font-family="Sonore Text" font-size="18">
      Two    words	here
  </text>
  <text x="4" y="56" font-family="Sonore Text" font-size="18" xml:space="preserve">  Kept   spaces</text>
  <text x="4" y="86" font-family="Sonore Text" font-size="18">Line
break <tspan fill="#a02000">and</tspan>  span</text>
</svg>'''),
    'weights': (230, 120, '''<svg xmlns="http://www.w3.org/2000/svg" width="230" height="120" viewBox="0 0 230 120">
  <g font-size="18">
    <text x="4" y="22" font-family="Sonore Text">Regular 400</text>
    <text x="4" y="44" font-family="Sonore Text" font-weight="bold">Bold face</text>
    <text x="4" y="66" font-family="Sonore Text" font-style="italic">Italic face</text>
    <text x="120" y="22" font-family="Sonore Text Var" font-weight="300">Axis 300</text>
    <text x="120" y="44" font-family="Sonore Text Var">Axis 400</text>
    <text x="120" y="66" font-family="Sonore Text Var" font-weight="700">Axis 700</text>
    <text x="4" y="100" font-family="Sonore Text" font-weight="600">Six hundred is bold</text>
  </g>
</svg>'''),
    'styled': (220, 110, '''<svg xmlns="http://www.w3.org/2000/svg" width="220" height="110" viewBox="0 0 220 110">
  <defs>
    <linearGradient id="g" x1="0" y1="0" x2="1" y2="0">
      <stop offset="0" stop-color="#ff0000"/><stop offset="1" stop-color="#0000ff"/>
    </linearGradient>
  </defs>
  <g font-family="Sonore Text">
    <text x="4" y="26" font-size="24" fill="none" stroke="#000" stroke-width="1">Outline</text>
    <text x="110" y="26" font-size="24" fill="url(#g)">Grad</text>
    <text x="10" y="80" font-size="20" transform="rotate(-12 10 80)" opacity="0.5">Turned 12</text>
    <text x="10" y="102" style="font-size:1.1em; letter-spacing:3px; fill:#802000">Spaced out</text>
  </g>
</svg>'''),
    'fallback': (220, 70, '''<svg xmlns="http://www.w3.org/2000/svg" width="220" height="70" viewBox="0 0 220 70">
  <text x="4" y="24" font-family="NoSuchFont, 'Sonore Text', serif" font-size="18">First found</text>
  <text x="4" y="46" font-family="serif" font-size="18">Generic serif</text>
  <text x="4" y="66" font-size="18">No family</text>
</svg>'''),
    'clip': (160, 50, '''<svg xmlns="http://www.w3.org/2000/svg" width="160" height="50" viewBox="0 0 160 50">
  <clipPath id="c"><text x="6" y="38" font-family="Sonore Text" font-size="34">CLIP</text></clipPath>
  <rect width="160" height="50" fill="#106030" clip-path="url(#c)"/>
</svg>'''),
}


def main():
    fonts = {
        'SonoreText-Regular.ttf': build('Sonore Text', 'Regular', 400, 80),
        'SonoreText-Bold.ttf': build('Sonore Text', 'Bold', 700, 170),
        'SonoreText-Italic.ttf': build('Sonore Text', 'Italic', 400, 80, slant=0.2, italic=True),
    }
    for name, font in fonts.items():
        font.save(os.path.join(HERE, name))

    with tempfile.TemporaryDirectory() as tmp:
        doc = DesignSpaceDocument()
        axis = AxisDescriptor()
        axis.tag, axis.name = 'wght', 'Weight'
        axis.minimum, axis.default, axis.maximum = 100, 400, 900
        doc.addAxis(axis)
        for weight, bar in ((100, 30), (400, 80), (900, 220)):
            path = os.path.join(tmp, 'm%d.ttf' % weight)
            build('Sonore Text Var', 'W%d' % weight, 400, bar).save(path)
            source = SourceDescriptor()
            source.path = path
            source.location = {'Weight': weight}
            doc.addSource(source)
        vf, _, _ = varLib.build(doc)
        for nid, text in ((1, 'Sonore Text Var'), (2, 'Regular'), (4, 'Sonore Text Var Regular'),
                          (6, 'SonoreTextVar-Regular'), (3, 'SonoreTextVar-Regular-1.000')):
            vf['name'].setName(text, nid, 3, 1, 0x409)
        vf.save(os.path.join(HERE, 'SonoreTextVar.ttf'))

        # Static cuts of the variable font, for resvg to draw from.
        statics = []
        for weight in (300, 400, 700):
            cut = instancer.instantiateVariableFont(TTFont(os.path.join(HERE, 'SonoreTextVar.ttf')),
                                                    {'wght': weight})
            cut['OS/2'].usWeightClass = weight
            cut['OS/2'].fsSelection = 0x20 if weight >= 700 else 0x40
            cut['head'].macStyle = 1 if weight >= 700 else 0
            path = os.path.join(tmp, 'var%d.ttf' % weight)
            cut.save(path)
            statics.append(path)

        files = [os.path.join(HERE, n) for n in fonts] + statics
        lines = ['# made by make_svgtext.py: resvg %s draws each of these. Do not edit.' % resvg_py.__resvg_version__]
        for name, (w, h, svg) in SVGS.items():
            with open(os.path.join(HERE, name + '.svg'), 'w', newline='\n') as f:
                f.write(svg + '\n')
            png = resvg_py.svg_to_bytes(svg_string=svg, skip_system_fonts=True, font_files=files,
                                        font_family='Sonore Text', serif_family='Sonore Text')
            im = Image.open(io.BytesIO(bytes(png))).convert('RGBA')
            assert im.size == (w, h), (name, im.size)
            im.save(os.path.join(HERE, name + '.png'), optimize=True)
            ink = int((np.asarray(im)[..., 3] > 0).sum())
            lines.append('svg %s.svg %s.png %d %d %d' % (name, name, w, h, ink))
    with open(os.path.join(HERE, 'svgtext_expected.txt'), 'w', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')


if __name__ == '__main__':
    main()
