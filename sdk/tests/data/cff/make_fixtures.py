# SPDX-License-Identifier: Apache-2.0
# Builds the CFF fixtures sdk_tests reads, from nothing: every outline below
# is written out here, so no font's licence comes along.
#
#   python make_fixtures.py        (needs fontTools)
#
# tiny.otf      name-keyed, with local and global subroutines
# tiny_cid.otf  the same glyphs CID-keyed: two Font DICTs chosen by an
#               FDSelect of format 3, each with its own local subroutines
# tiny_cid_fdmatrix.otf  the same, with the FontMatrix split as Adobe's CJK
#               fonts split it: the identity in the Top DICT, 0.001 in each
#               Font DICT
# tiny_kern.otf tiny.otf kerning through a kern table instead of GPOS
# tiny.ttf      a TrueType font with B's square: a Typeface that loaded it
#               and then a CFF font must draw the CFF font
#
# Each glyph that exercises an operator has a twin drawn with the plainest
# operators there are; sdk_tests draws both and wants the same pixels.
#   A  a square with hstemhm, an implied vstem and a hintmask, and the width
#      as the first stem argument         B  the same square, no hints
#   C  flex, hflex, hflex1, flex1         D  the same curves as rrcurveto
#   E  hvcurveto, vhcurveto, hhcurveto and vvcurveto (both with their odd
#      leading argument), rcurveline, rlinecurve
#                                         F  the same as rrcurveto/rlineto
#   G  the square of B through a local subroutine that calls a global one
#   H  a square at half-unit coordinates: 16.16 fixed operands
#                                         I  the same, made of integers
#                                            twice the size (drawn at half)
#   J  B's square with a dotsection (12 0, deprecated) in the middle:
#      ignored, as every reader ignores it
#
# Both also kern, through GPOS -- no kern table: A B -80 as a listed pair
# (PairPos format 1), [C D] [E F] -40 as a class matrix (format 2), and
# G H +25 in a lookup behind an Extension (type 9).
import os
import struct
from fontTools.feaLib.builder import addOpenTypeFeaturesFromString
from fontTools.fontBuilder import FontBuilder
from fontTools.misc.psCharStrings import T2CharString
from fontTools.ttLib import TTFont
from fontTools import cffLib

HERE = os.path.dirname(os.path.abspath(__file__))
W = 600

def flat(*curves):
    out = []
    for c in curves:
        out += list(c)
    return out

# C and D: from (100, 100), four flexes, then back down and closed.
f1 = [40, 60, 40, 80, 60, 20]            # flex: two curves, then fd
f2 = [40, -20, 40, -80, 40, -60]
hf = [30, 30, 40, 30, 30, 30, 30]        # hflex: dx1 dx2 dy2 dx3 dx4 dx5 dx6
hf1 = [20, 30, 30, 20, 40, 40, 30, -30, 20]  # hflex1
fl1 = [20, 20, 30, 30, 30, 10, 30, -10, 30, -30, 30]  # flex1: last is d6
def hflex_curves(a):
    return [a[0], 0, a[1], a[2], a[3], 0], [a[4], 0, a[5], -a[2], a[6], 0]
def hflex1_curves(a):
    return [a[0], a[1], a[2], a[3], a[4], 0], [a[5], 0, a[6], a[7], a[8], -(a[1] + a[3] + a[7])]
def flex1_curves(a):
    dx = a[0] + a[2] + a[4] + a[6] + a[8]
    dy = a[1] + a[3] + a[5] + a[7] + a[9]
    if abs(dx) > abs(dy):
        last = [a[10], -dy]
    else:
        last = [-dx, a[10]]
    return a[0:6], a[6:10] + last
glyph_c = ([W, 100, 100, 'rmoveto'] + f1 + f2 + [50, 'flex'] + hf + ['hflex'] + hf1 + ['hflex1'] +
           fl1 + ['flex1', 0, -200, 'rlineto', 'endchar'])
c1, c2 = hflex_curves(hf)
d1, d2 = hflex1_curves(hf1)
e1, e2 = flex1_curves(fl1)
glyph_d = ([W, 100, 100, 'rmoveto'] + flat(f1, f2, c1, c2, d1, d2, e1, e2) + ['rrcurveto'] +
           [0, -200, 'rlineto', 'endchar'])

# E and F: from (100, 100), every curve shorthand, each with its rr twin.
glyph_e = ([W, 100, 100, 'rmoveto',
            40, 30, 50, 60, 'hvcurveto',             # (40,0) (30,50) (0,60)
            50, 30, 40, 60, 10, 'vhcurveto',         # (0,50) (30,40) (60,10) -- with final df
            15, 40, 20, 30, 40, 'hhcurveto',         # dy1=15: (40,15) (20,30) (40,0)
            -10, 30, 20, 20, 40, 'vvcurveto',        # dx1=-10: (-10,30) (20,20) (0,40)
            30, 0, 20, 20, 0, 30, 25, -40, 'rcurveline',
            10, -20, 20, -30, 10, -30, 30, -20, 20, -60, 'rlinecurve',
            'endchar'])
glyph_f = ([W, 100, 100, 'rmoveto',
            40, 0, 30, 50, 0, 60,
            0, 50, 30, 40, 60, 10,
            40, 15, 20, 30, 40, 0,
            -10, 30, 20, 20, 0, 40,
            30, 0, 20, 20, 0, 30, 'rrcurveto',
            25, -40, 10, -20, 20, -30, 'rlineto',
            10, -30, 30, -20, 20, -60, 'rrcurveto',
            'endchar'])

square = [100, 0, 'rmoveto', 400, 'hlineto', 700, 'vlineto', -400, 'hlineto']
glyph_a = [W, 0, 700, 'hstemhm', 100, 400, 'hintmask', bytes([0xC0])] + square[:3] + \
          ['hintmask', bytes([0x80])] + square[3:] + ['endchar']
glyph_b = [W] + square + ['endchar']
glyph_h = [W, 50.5, 0.5, 'rmoveto', 200.5, 'hlineto', 350.25, 'vlineto', -200.5, 'hlineto', 'endchar']
glyph_i = [W, 101, 1, 'rmoveto', 401, 'hlineto', 700.5, 'vlineto', -401, 'hlineto', 'endchar']
glyph_j = [W, 100, 0, 'rmoveto', 400, 'hlineto', 'ignore', 700, 'vlineto', -400, 'hlineto', 'endchar']

order = ['.notdef', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J']
programs = {'.notdef': [W, 'endchar'], 'A': glyph_a, 'B': glyph_b, 'C': glyph_c, 'D': glyph_d,
            'E': glyph_e, 'F': glyph_f, 'H': glyph_h, 'I': glyph_i, 'J': glyph_j,
            # G: callsubr -> local 0, which draws the start and callgsubr -> global 0
            'G': [W, 0 - 107, 'callsubr', 'endchar']}

def build_plain(path):
    fb = FontBuilder(1000, isTTF=False)
    fb.setupGlyphOrder(order)
    fb.setupCharacterMap({ord(g): g for g in order if len(g) == 1})
    # G first as B's square: its subroutines do not exist until the font
    # is reopened below, and fontTools runs every charstring when it saves.
    charstrings = {g: T2CharString(program=programs['B' if g == 'G' else g]) for g in order}
    fb.setupCFF('SonoreCFFFixture', {'FullName': 'Sonore CFF Fixture'}, charstrings, {})
    fb.setupHorizontalMetrics({g: (W, 0) for g in order})
    fb.setupHorizontalHeader(ascent=800, descent=-200)
    fb.setupNameTable({'familyName': 'Sonore CFF Fixture', 'styleName': 'Regular'})
    fb.setupOS2()
    fb.setupPost()
    fb.save(path)
    font = TTFont(path)
    cff = font['CFF '].cff
    top = cff.topDictIndex[0]
    private = top.Private
    local = cffLib.SubrsIndex()
    local.append(T2CharString(program=[100, 0, 'rmoveto', 0 - 107, 'callgsubr', 'return'],
                              private=private, globalSubrs=cff.GlobalSubrs))
    cff.GlobalSubrs.append(T2CharString(program=[400, 'hlineto', 700, 'vlineto', -400, 'hlineto', 'return'],
                                        private=private, globalSubrs=cff.GlobalSubrs))
    private.Subrs = local
    g = top.CharStrings['G']
    g.decompile()
    g.program = list(programs['G'])
    addOpenTypeFeaturesFromString(font, '''
languagesystem DFLT dflt;
lookup EXT useExtension { pos G H 25; } EXT;
feature kern {
  pos A B -80;
  pos [C D] [E F] -40;
  lookup EXT;
} kern;
''')
    font.save(path)

# ── CID-keyed, written by hand: fontTools has no builder for it ──
def index(items):
    if not items:
        return struct.pack('>H', 0)
    offsets = [1]
    for it in items:
        offsets.append(offsets[-1] + len(it))
    size = 1 if offsets[-1] < 0x100 else 2 if offsets[-1] < 0x10000 else 4
    out = struct.pack('>HB', len(items), size)
    for o in offsets:
        out += o.to_bytes(size, 'big')
    return out + b''.join(items)

def num(v):
    return b'\x1d' + struct.pack('>i', v)

def op(code):
    return bytes([12, code - 1200]) if code >= 1200 else bytes([code])

def compile_cs(prog):
    t = T2CharString(program=prog)
    t.compile()
    return t.bytecode

def real(v):
    # A DICT real: decimal nibbles, f to end.
    text = repr(float(v)).replace('e-', 'c').replace('e', 'b').replace('+', '')
    nib = {'.': 0xA, 'b': 0xB, 'c': 0xC, '-': 0xE}
    out = [int(ch) if ch.isdigit() else nib[ch] for ch in text] + [0xF]
    if len(out) % 2:
        out.append(0xF)
    return bytes([30] + [(out[i] << 4) | out[i + 1] for i in range(0, len(out), 2)])

def build_cid(plain_path, path, split_matrix=False):
    n = len(order)
    half = n // 2
    # Glyph G calls local subr 0 of its Font DICT, which calls global subr 0.
    fd_of = [0 if g < half else 1 for g in range(n)]
    local_g = compile_cs([100, 0, 'rmoveto', 0 - 107, 'callgsubr', 'return'])
    gsubr = compile_cs([400, 'hlineto', 700, 'vlineto', -400, 'hlineto', 'return'])
    charstrings = [compile_cs(programs[g]) for g in order]
    header = bytes([1, 0, 4, 4])
    names = index([b'SonoreCFFFixtureCID'])
    strings = index([b'Adobe', b'Identity'])
    gsubrs = index([gsubr])
    top_matrix = (num(1) + num(0) + num(0) + num(1) + num(0) + num(0) + op(1207)) if split_matrix else b''
    fd_matrix = (real(0.001) + num(0) + num(0) + real(0.001) + num(0) + num(0) + op(1207)) if split_matrix else b''
    def top_dict(charset, fdselect, cs, fdarray):
        return (num(391) + num(392) + num(0) + op(1230) + num(n) + op(1234) + num(charset) + op(15) +
                num(fdselect) + op(1237) + num(cs) + op(17) + num(fdarray) + op(1236) + top_matrix)
    charset = bytes([2]) + struct.pack('>HH', 1, n - 2)
    fdselect = bytes([3]) + struct.pack('>H', 2) + struct.pack('>HB', 0, 0) + \
               struct.pack('>HB', half, 1) + struct.pack('>H', n)
    cs_index = index(charstrings)
    top_len = len(index([top_dict(0, 0, 0, 0)]))
    charset_off = len(header) + len(names) + top_len + len(strings) + len(gsubrs)
    fdselect_off = charset_off + len(charset)
    cs_off = fdselect_off + len(fdselect)
    fdarray_off = cs_off + len(cs_index)
    def font_dict(size, at):
        return num(size) + num(at) + op(18) + fd_matrix
    priv_off = fdarray_off + len(index([font_dict(0, 0), font_dict(0, 0)]))
    font_dicts, privates, cursor = [], [], priv_off
    for fd in range(2):
        pd = num(len(num(0) + op(19))) + op(19)
        subs = index([local_g])
        font_dicts.append(font_dict(len(pd), cursor))
        privates.append(pd + subs)
        cursor += len(pd) + len(subs)
    data = (header + names + index([top_dict(charset_off, fdselect_off, cs_off, fdarray_off)]) +
            strings + gsubrs + charset + fdselect + cs_index + index(font_dicts) + b''.join(privates))
    # Into plain's sfnt in place of its CFF table.
    raw = open(plain_path, 'rb').read()
    version, count = struct.unpack('>IH', raw[:6])
    tables = {}
    for i in range(count):
        tag, _, off, length = struct.unpack('>4sIII', raw[12 + 16 * i:28 + 16 * i])
        tables[tag] = raw[off:off + length]
    tables[b'CFF '] = data
    tags = sorted(tables)
    entry = 1
    while entry * 2 <= len(tags):
        entry *= 2
    head = struct.pack('>IHHHH', version, len(tags), entry * 16, entry.bit_length() - 1,
                       len(tags) * 16 - entry * 16)
    def checksum(b):
        b = b + b'\0' * ((4 - len(b) % 4) % 4)
        return sum(struct.unpack('>%dI' % (len(b) // 4), b)) & 0xFFFFFFFF
    directory, body, offset = b'', b'', 12 + 16 * len(tags)
    for tag in tags:
        t = tables[tag]
        directory += struct.pack('>4sIII', tag, checksum(t), offset + len(body), len(t))
        body += t + b'\0' * ((4 - len(t) % 4) % 4)
    open(path, 'wb').write(head + directory + body)

def build_kern(plain_path, path):
    from fontTools.ttLib import newTable
    from fontTools.ttLib.tables._k_e_r_n import KernTable_format_0
    font = TTFont(plain_path)
    del font['GPOS']
    kern = newTable('kern')
    kern.version = 0
    sub = KernTable_format_0()
    sub.version = 0
    sub.coverage = 1
    sub.format = 0
    sub.kernTable = {('A', 'B'): -50, ('C', 'E'): 30, ('J', 'A'): -20}
    kern.kernTables = [sub]
    font['kern'] = kern
    font.save(path)

def build_ttf(path):
    from fontTools.pens.ttGlyphPen import TTGlyphPen
    def square():
        pen = TTGlyphPen(None)
        pen.moveTo((100, 0))
        pen.lineTo((100, 700))
        pen.lineTo((500, 700))
        pen.lineTo((500, 0))
        pen.closePath()
        return pen.glyph()
    fb = FontBuilder(1000, isTTF=True)
    fb.setupGlyphOrder(['.notdef', 'B'])
    fb.setupCharacterMap({ord('B'): 'B'})
    fb.setupGlyf({'.notdef': TTGlyphPen(None).glyph(), 'B': square()})
    fb.setupHorizontalMetrics({'.notdef': (W, 0), 'B': (W, 100)})
    fb.setupHorizontalHeader(ascent=800, descent=-200)
    fb.setupNameTable({'familyName': 'Sonore TTF Fixture', 'styleName': 'Regular'})
    fb.setupOS2()
    fb.setupPost()
    fb.save(path)

plain = os.path.join(HERE, 'tiny.otf')
build_plain(plain)
build_cid(plain, os.path.join(HERE, 'tiny_cid.otf'))
build_cid(plain, os.path.join(HERE, 'tiny_cid_fdmatrix.otf'), split_matrix=True)
build_kern(plain, os.path.join(HERE, 'tiny_kern.otf'))
build_ttf(os.path.join(HERE, 'tiny.ttf'))
check = TTFont(os.path.join(HERE, 'tiny_cid.otf'))
assert check['CFF '].cff.topDictIndex[0].ROS == ('Adobe', 'Identity', 0)
check = TTFont(os.path.join(HERE, 'tiny_cid_fdmatrix.otf'))
top = check['CFF '].cff.topDictIndex[0]
assert list(top.FontMatrix) == [1, 0, 0, 1, 0, 0], top.FontMatrix
assert abs(top.FDArray[0].FontMatrix[0] - 0.001) < 1e-12, top.FDArray[0].FontMatrix
print('wrote tiny.otf, tiny_cid.otf, tiny_cid_fdmatrix.otf, tiny_kern.otf and tiny.ttf')
