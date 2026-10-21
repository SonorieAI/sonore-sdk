# SPDX-License-Identifier: Apache-2.0
# Builds the JPEG and GIF fixtures images_test reads, and what Pillow decodes
# them to -- Pillow's libjpeg-turbo and its GIF reader are the oracle.
#
#   python make_images.py        (needs Pillow and numpy)
#
# The pictures are drawn here from arithmetic: gradients, a hard-edged
# rectangle and a disc, so the files carry no licence and the edges give the
# DCT and the upsampling something to get wrong.
#
# JPEG -- most written by Pillow, and the ones Pillow cannot write by the
# small baseline encoder below, which writes any sampling factors, one scan
# per component, 16-bit quantization tables, restart intervals and YCCK:
#   baseline_444 / _422 / _420      Pillow, the three common samplings
#   progressive_420 / _444          Pillow, libjpeg's default progression:
#                                   DC first and refined, AC bands, EOB runs
#   restart_420                     Pillow, a restart marker every MCU
#   restart_progressive             Pillow, progressive with restarts
#   grey, grey_progressive          one component
#   cmyk                            Pillow's CMYK, Adobe-inverted
#   rgb                             Pillow's keep_rgb: Adobe transform 0
#   sampling_440                    Y 1x2: libjpeg's h1v2 fancy upsampling
#   sampling_411                    Y 4x1: whole-ratio replication
#   sampling_mixed                  Y 2x2, Cb 1x2, Cr 2x1: two filters at once
#   sampling_32                     Y 3x2: a ratio of three
#   separate_scans                  4:2:0, one component per scan, restarts
#   quant16                         16-bit quantization tables
#   ycck                            Adobe transform 2
#   orient_1 .. orient_8            Pillow, an EXIF orientation each
# Each has <name>.png beside it: Pillow's decode as RGB (CMYK and YCCK
# turned to RGB as (255-C)(255-K)/255, the orientation applied as
# ImageOps.exif_transpose applies it).
#
# GIF -- a small LZW encoder below, and Pillow's writer:
#   still_87a        GIF87a, a global table of 16, no extensions
#   interlaced       GIF89a, interlaced rows, minimum code size 3
#   lzw_deferred     a table that fills at 4096 and keeps going without a
#                    clear code, at twelve bits
#   local_tables     no global table; three frames with local tables of 2,
#                    16 and 256 entries, offsets, every disposal, a loop count
#   anim_pillow      Pillow's own animation: transparency, disposal 2,
#                    frames cropped to what changed
#   anim_keep        Pillow, opaque frames left in place
# images_expected.txt holds, per GIF, its size, loop count and every frame's
# delay, disposal, rectangle and an FNV-1a 64 hash of the composited canvas
# in premultiplied RGBA -- exact, there is nothing to round in a GIF.
import io
import math
import os
import struct

import numpy as np
from PIL import Image, ImageOps

HERE = os.path.dirname(os.path.abspath(__file__))


def picture(w, h, seed=0):
    """Gradients, a hard red rectangle and a blue disc: smooth areas and edges."""
    y, x = np.mgrid[0:h, 0:w].astype(np.float64)
    r = 40 + 180 * x / max(w - 1, 1)
    g = 60 + 150 * y / max(h - 1, 1)
    b = 120 + 100 * np.sin((x + 2 * y + seed) / 5.0)
    img = np.stack([r, g, b], -1)
    img[h // 4: h // 2, w // 5: w // 2] = [235, 30, 40]
    disc = (x - 0.7 * w) ** 2 + (y - 0.6 * h) ** 2 < (min(w, h) * 0.22) ** 2
    img[disc] = [20, 40, 220]
    return np.clip(np.round(img), 0, 255).astype(np.uint8)


# ── A baseline JPEG encoder, for what Pillow cannot write ─────────────────────

STD = {
    'dc_lum': ([0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0], list(range(12))),
    'dc_chr': ([0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0], list(range(12))),
    'ac_lum': ([0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d], bytes.fromhex(
        '01020300041105122131410613516107227114328191a1082342b1c11552d1f02433627282090a161718191a25262728292a'
        '3435363738393a434445464748494a535455565758595a636465666768696a737475767778797a838485868788898a9293949596'
        '9798999aa2a3a4a5a6a7a8a9aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5d6d7d8d9dae1e2e3e4e5e6e7e8e9eaf1f2f3'
        'f4f5f6f7f8f9fa')),
    'ac_chr': ([0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77], bytes.fromhex(
        '000102031104052131061241510761711322328108144291a1b1c109233352f0156272d10a162434e125f11718191a262728292a'
        '35363738393a434445464748494a535455565758595a636465666768696a737475767778797a82838485868788898a929394959697'
        '98999aa2a3a4a5a6a7a8a9aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5d6d7d8d9dae2e3e4e5e6e7e8e9eaf2f3f4f5f6'
        'f7f8f9fa')),
}
LUMA_Q = [16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56,
          14, 17, 22, 29, 51, 87, 80, 62, 18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92,
          49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99]
CHROMA_Q = [17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99, 99, 99,
            47, 66, 99, 99, 99, 99, 99, 99] + [99] * 32
ZIGZAG = [0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,
          7, 14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31,
          39, 46, 53, 60, 61, 54, 47, 55, 62, 63]


def codes_for(counts, values):
    table, code, k = {}, 0, 0
    for length in range(1, 17):
        for _ in range(counts[length - 1]):
            table[values[k]] = (code, length)
            code += 1
            k += 1
        code <<= 1
    return table


class Bits:
    def __init__(self, out):
        self.out, self.acc, self.n = out, 0, 0

    def put(self, value, length):
        for i in range(length - 1, -1, -1):
            self.acc = (self.acc << 1) | ((value >> i) & 1)
            self.n += 1
            if self.n == 8:
                self.out.append(self.acc)
                if self.acc == 0xFF:
                    self.out.append(0)
                self.acc, self.n = 0, 0

    def flush(self):
        while self.n:
            self.put(1, 1)


def scaled(table, quality):
    s = 5000 // quality if quality < 50 else 200 - quality * 2
    return [min(max((v * s + 50) // 100, 1), 255) for v in table]


DCT = np.array([[(math.sqrt(0.125) if u == 0 else 0.5) * math.cos((2 * x + 1) * u * math.pi / 16)
                 for x in range(8)] for u in range(8)])


def segment(marker, body):
    return bytes([0xFF, marker]) + struct.pack('>H', len(body) + 2) + body


def encode_jpeg(planes, sampling, quality=85, separate=False, restart=0, adobe=None, jfif=True, quant16=False,
                ids=None, tables=None):
    """planes: full-size float arrays, one per component, already in the
    file's colour space. sampling: (h, v) per component."""
    H, W = planes[0].shape
    n = len(planes)
    hmax = max(h for h, _ in sampling)
    vmax = max(v for _, v in sampling)
    mcux, mcuy = -(-W // (8 * hmax)), -(-H // (8 * vmax))
    tables = tables or [0] + [1] * (n - 1)
    quants = [scaled(LUMA_Q, quality), scaled(CHROMA_Q, quality)]
    if quant16:
        quants = [[min(v * 3, 600) for v in q] for q in quants]  # past 255: needs the 16-bit form

    blocks = []
    for plane, (h, v) in zip(planes, sampling):
        fx, fy = hmax // h, vmax // v
        dw, dh = -(-W * h // hmax), -(-H * v // vmax)
        padded = np.pad(plane, ((0, dh * fy - H if dh * fy > H else 0), (0, dw * fx - W if dw * fx > W else 0)),
                        mode='edge')[:dh * fy, :dw * fx]
        ds = padded.reshape(dh, fy, dw, fx).mean(axis=(1, 3))
        bw, bh = mcux * h, mcuy * v
        full = np.pad(ds, ((0, bh * 8 - dh), (0, bw * 8 - dw)), mode='edge')
        blocks.append((full, dw, dh, bw, bh))

    def coded(comp, bx, by):
        full = blocks[comp][0]
        block = full[by * 8: by * 8 + 8, bx * 8: bx * 8 + 8] - 128.0
        coef = DCT @ block @ DCT.T
        q = quants[tables[comp]]
        flat = coef.flatten()
        return [int(round(flat[ZIGZAG[i]] / q[ZIGZAG[i]])) for i in range(64)]

    huff = {0: (codes_for(*STD['dc_lum']), codes_for(*STD['ac_lum'])),
            1: (codes_for(*STD['dc_chr']), codes_for(*STD['ac_chr']))}

    def write_block(bits, comp, zz, pred):
        dc_t, ac_t = huff[tables[comp]]
        diff = zz[0] - pred[comp]
        pred[comp] = zz[0]
        s = abs(diff).bit_length()
        bits.put(*dc_t[s])
        if s:
            bits.put(diff if diff > 0 else diff - 1 + (1 << s), s)
        run = 0
        for k in range(1, 64):
            if zz[k] == 0:
                run += 1
                continue
            while run > 15:
                bits.put(*ac_t[0xF0])
                run -= 16
            s = abs(zz[k]).bit_length()
            bits.put(*ac_t[(run << 4) | s])
            bits.put(zz[k] if zz[k] > 0 else zz[k] - 1 + (1 << s), s)
            run = 0
        if run:
            bits.put(*ac_t[0])

    ids = ids or list(range(1, n + 1))
    out = bytearray(b'\xff\xd8')
    if jfif:
        out += segment(0xE0, b'JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00')
    if adobe is not None:
        out += segment(0xEE, b'Adobe' + struct.pack('>HHHB', 100, 0, 0, adobe))
    for t, q in enumerate(quants[:max(tables) + 1]):
        if quant16:
            out += segment(0xDB, bytes([0x10 | t]) + b''.join(struct.pack('>H', q[ZIGZAG[i]]) for i in range(64)))
        else:
            out += segment(0xDB, bytes([t]) + bytes(q[ZIGZAG[i]] for i in range(64)))
    sof = struct.pack('>BHHB', 8, H, W, n)
    for i in range(n):
        sof += bytes([ids[i], (sampling[i][0] << 4) | sampling[i][1], tables[i]])
    out += segment(0xC0, sof)
    for t in range(max(tables) + 1):
        for tc, name in ((0, 'dc'), (1, 'ac')):
            counts, values = STD['%s_%s' % (name, 'lum' if t == 0 else 'chr')]
            out += segment(0xC4, bytes([(tc << 4) | t] + counts) + bytes(values))
    if restart:
        out += segment(0xDD, struct.pack('>H', restart))

    def scan(comps, units):
        body = bytes([len(comps)]) + b''.join(bytes([ids[c], (tables[c] << 4) | tables[c]]) for c in comps)
        data = bytearray()
        bits = Bits(data)
        pred = [0] * n
        rst = 0
        for i, unit in enumerate(units):
            if restart and i and i % restart == 0:
                bits.flush()
                data += bytes([0xFF, 0xD0 + rst])
                rst = (rst + 1) % 8
                pred = [0] * n
            for comp, bx, by in unit:
                write_block(bits, comp, coded(comp, bx, by), pred)
        bits.flush()
        return segment(0xDA, body + b'\x00\x3f\x00') + bytes(data)

    if separate:
        for c in range(n):
            _, dw, dh, _, _ = blocks[c]
            units = [[(c, bx, by)] for by in range(-(-dh // 8)) for bx in range(-(-dw // 8))]
            out += scan([c], units)
    else:
        units = []
        for my in range(mcuy):
            for mx in range(mcux):
                unit = []
                for c, (h, v) in enumerate(sampling):
                    for yy in range(v):
                        for xx in range(h):
                            unit.append((c, mx * h + xx, my * v + yy))
                units.append(unit)
        out += scan(list(range(n)), units)
    out += b'\xff\xd9'
    return bytes(out)


def ycbcr(rgb):
    r, g, b = [rgb[..., i].astype(np.float64) for i in range(3)]
    y = 0.299 * r + 0.587 * g + 0.114 * b
    cb = -0.168736 * r - 0.331264 * g + 0.5 * b + 128
    cr = 0.5 * r - 0.418688 * g - 0.081312 * b + 128
    return [np.clip(np.round(p), 0, 255) for p in (y, cb, cr)]


def mul255(a, b):
    x = a * b + 128
    return (x + (x >> 8)) >> 8


def reference_rgb(path):
    im = Image.open(path)
    im = ImageOps.exif_transpose(im)
    if im.mode == 'CMYK':
        c = np.asarray(im).astype(np.int64)
        return np.stack([mul255(255 - c[..., i], 255 - c[..., 3]) for i in range(3)], -1).astype(np.uint8)
    return np.asarray(im.convert('RGB'))


# ── GIF: an LZW encoder ─────────────────────────────────────────────────────

def lzw(indices, minimum, literal=False, deferred=False):
    """The image data's codes, as sub-blocks. `literal` emits every index as
    its own code -- valid, if useless, and the way to fill the table; a full
    table is cleared unless `deferred`."""
    clear, end = 1 << minimum, (1 << minimum) + 1
    out = []  # (code, width)
    width, nxt = minimum + 1, clear + 2
    table = {}

    def emit(code):
        nonlocal width
        out.append((code, width))

    def added():
        nonlocal width, nxt
        if nxt >= (1 << width) and width < 12:
            width += 1
        if nxt < 4096:
            nxt += 1

    emit(clear)
    if literal:
        for i in indices:
            emit(i)
            added()
            if nxt == 4096 and not deferred:
                emit(clear)
                width, nxt = minimum + 1, clear + 2
    else:
        w = (indices[0],)
        for c in indices[1:]:
            wc = w + (c,)
            if wc in table:
                w = wc
                continue
            emit(table[w] if len(w) > 1 else w[0])
            if nxt < 4096:
                table[wc] = nxt
            added()
            if nxt == 4096 and not deferred:
                emit(clear)
                table.clear()
                width, nxt = minimum + 1, clear + 2
            w = (c,)
        emit(table[w] if len(w) > 1 else w[0])
        added()
    emit(end)
    acc = n = 0
    data = bytearray()
    for code, bits in out:
        acc |= code << n
        n += bits
        while n >= 8:
            data.append(acc & 0xFF)
            acc >>= 8
            n -= 8
    if n:
        data.append(acc & 0xFF)
    blocks = bytearray([minimum])
    for i in range(0, len(data), 255):
        chunk = data[i:i + 255]
        blocks += bytes([len(chunk)]) + chunk
    return bytes(blocks + b'\x00')


def table_bits(entries):
    return max(1, (entries - 1).bit_length()) - 1


def gif(width, height, frames, palette=None, version=b'89a', loop=None):
    """frames: dicts with indices (2D array), left, top, delay (cs), disposal,
    transparent, palette (local), interlace, minimum, literal, deferred."""
    out = bytearray(b'GIF' + version)
    flags = 0
    if palette is not None:
        flags = 0x80 | 0x70 | table_bits(len(palette) // 3)
    out += struct.pack('<HHBBB', width, height, flags, 0, 0)
    if palette is not None:
        out += palette
    if loop is not None:
        out += b'\x21\xff\x0bNETSCAPE2.0\x03\x01' + struct.pack('<H', loop) + b'\x00'
    for f in frames:
        idx = np.asarray(f['indices'], dtype=np.uint8)
        h, w = idx.shape
        if version == b'89a' and ('delay' in f or 'disposal' in f or 'transparent' in f):
            t = f.get('transparent')
            packed = (f.get('disposal', 0) << 2) | (1 if t is not None else 0)
            out += b'\x21\xf9\x04' + struct.pack('<BHB', packed, f.get('delay', 0), t or 0) + b'\x00'
        local = f.get('palette')
        iflags = 0
        if local is not None:
            iflags |= 0x80 | table_bits(len(local) // 3)
        if f.get('interlace'):
            iflags |= 0x40
        out += b'\x2c' + struct.pack('<HHHHB', f.get('left', 0), f.get('top', 0), w, h, iflags)
        if local is not None:
            out += local
        rows = list(range(h))
        if f.get('interlace'):
            rows = list(range(0, h, 8)) + list(range(4, h, 8)) + list(range(2, h, 4)) + list(range(1, h, 2))
        flat = [int(v) for r in rows for v in idx[r]]
        out += lzw(flat, f.get('minimum', 8), f.get('literal', False), f.get('deferred', False))
    out += b'\x3b'
    return bytes(out)


def palette_of(colours, size):
    flat = [c for rgb in colours for c in rgb]
    return bytes(flat + [0] * (size * 3 - len(flat)))


def premultiplied(rgba):
    a = rgba[..., 3:4].astype(np.int64)
    x = rgba[..., :3].astype(np.int64) * a + 127
    rgb = (x + (x >> 8)) >> 8
    return np.concatenate([rgb, a], -1).astype(np.uint8)


def fnv64(data):
    h = 0xcbf29ce484222325
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def gif_frames_info(data):
    """Each frame's delay, disposal and rectangle, read from the file the
    simple way: the graphic control extension before each descriptor."""
    at = 13
    flags = data[10]
    if flags & 0x80:
        at += 3 * (2 << (flags & 7))
    info, delay, disposal, loop = [], 0, 0, 1
    while at < len(data):
        b = data[at]
        at += 1
        if b == 0x3B:
            break
        if b == 0x21:
            label = data[at]
            at += 1
            if label == 0xF9:
                disposal = (data[at + 1] >> 2) & 7
                delay = struct.unpack('<H', data[at + 2:at + 4])[0] * 10
            if label == 0xFF and data[at + 1:at + 12] == b'NETSCAPE2.0':
                loop = struct.unpack('<H', data[at + 14:at + 16])[0]
            while data[at]:
                at += data[at] + 1
            at += 1
            continue
        left, top, w, h, f = struct.unpack('<HHHHB', data[at:at + 9])
        at += 9
        if f & 0x80:
            at += 3 * (2 << (f & 7))
        at += 1
        while data[at]:
            at += data[at] + 1
        at += 1
        info.append((delay, disposal if disposal <= 3 else 0, left, top, w, h))
        delay, disposal = 0, 0
    return info, loop


def main():
    lines = ['# made by make_images.py: Pillow\'s decode of every fixture. Do not edit.']

    def jpeg_fixture(name, data=None, image=None, **save):
        path = os.path.join(HERE, name + '.jpg')
        if data is None:
            buf = io.BytesIO()
            image.save(buf, 'JPEG', **save)
            data = buf.getvalue()
        with open(path, 'wb') as f:
            f.write(data)
        rgb = reference_rgb(path)
        Image.fromarray(rgb, 'RGB').save(os.path.join(HERE, name + '.png'), optimize=True)
        im = Image.open(path)
        orientation = im.getexif().get(0x0112, 1)
        progressive = 1 if im.info.get('progressive') or im.info.get('progression') else 0
        comps = {'L': 1, 'RGB': 3, 'YCbCr': 3, 'CMYK': 4}[im.mode]
        lines.append('jpeg %s.jpg %s.png %d %d %d' % (name, name, orientation, progressive, comps))

    W, H = 37, 29
    rgb = picture(W, H)
    img = Image.fromarray(rgb, 'RGB')
    jpeg_fixture('baseline_444', image=img, quality=90, subsampling=0)
    jpeg_fixture('baseline_422', image=img, quality=85, subsampling=1)
    jpeg_fixture('baseline_420', image=img, quality=75, subsampling=2)
    jpeg_fixture('progressive_420', image=img, quality=80, subsampling=2, progressive=True)
    jpeg_fixture('progressive_444', image=img, quality=92, subsampling=0, progressive=True)
    jpeg_fixture('restart_420', image=img, quality=80, subsampling=2, restart_marker_blocks=1)
    jpeg_fixture('restart_progressive', image=img, quality=80, subsampling=2, progressive=True,
                 restart_marker_blocks=2)
    jpeg_fixture('grey', image=img.convert('L'), quality=85)
    jpeg_fixture('grey_progressive', image=img.convert('L'), quality=85, progressive=True)
    jpeg_fixture('cmyk', image=img.convert('CMYK'), quality=90)
    jpeg_fixture('rgb', image=img, quality=90, subsampling=0, keep_rgb=True)

    ycc = ycbcr(rgb)
    jpeg_fixture('sampling_440', encode_jpeg(ycc, [(1, 2), (1, 1), (1, 1)], 85))
    jpeg_fixture('sampling_411', encode_jpeg(ycc, [(4, 1), (1, 1), (1, 1)], 85))
    jpeg_fixture('sampling_mixed', encode_jpeg(ycc, [(2, 2), (1, 2), (2, 1)], 85))
    jpeg_fixture('sampling_32', encode_jpeg(ycc, [(3, 2), (1, 1), (1, 1)], 85))
    jpeg_fixture('separate_scans', encode_jpeg(ycc, [(2, 2), (1, 1), (1, 1)], 80, separate=True, restart=3))
    jpeg_fixture('quant16', encode_jpeg(ycc, [(1, 1), (1, 1), (1, 1)], 90, quant16=True))
    # YCCK, as Photoshop writes it: CMYK inverted, then the CMY part stored
    # as the YCbCr of its complement -- which is the true C, M and Y.
    cmyk = np.asarray(img.convert('CMYK')).astype(np.int64)
    cmyk[..., 3] = np.clip(255 - (rgb.astype(np.int64).max(axis=-1)), 0, 255) // 2
    true_cmy = np.stack([cmyk[..., 0], cmyk[..., 1], cmyk[..., 2]], -1).astype(np.uint8)
    ycck = ycbcr(true_cmy) + [255.0 - cmyk[..., 3].astype(np.float64)]
    jpeg_fixture('ycck', encode_jpeg(ycck, [(2, 2), (1, 1), (1, 1), (2, 2)], 90, jfif=False, adobe=2,
                                     tables=[0, 1, 1, 0]))

    small = Image.fromarray(picture(11, 5, seed=3), 'RGB')
    for o in range(1, 9):
        exif = Image.Exif()
        exif[0x0112] = o
        jpeg_fixture('orient_%d' % o, image=small, quality=90, subsampling=0, exif=exif.tobytes())

    # ── GIF ──
    def gif_fixture(name, data):
        path = os.path.join(HERE, name + '.gif')
        with open(path, 'wb') as f:
            f.write(data)
        info, loop = gif_frames_info(data)
        im = Image.open(path)
        lines.append('gif %s.gif %d %d %d %d' % (name, im.size[0], im.size[1], loop, len(info)))
        for i, (delay, disposal, left, top, w, h) in enumerate(info):
            im.seek(i)
            frame = premultiplied(np.asarray(im.convert('RGBA')))
            lines.append('  frame %d %d %d %d %d %d %016x' % (delay, disposal, left, top, w, h,
                                                             fnv64(frame.tobytes())))

    def quantised(arr, colours):
        im = Image.fromarray(arr, 'RGB').quantize(colours, dither=Image.Dither.NONE)
        return np.asarray(im), im.getpalette()[:colours * 3]

    idx16, pal16 = quantised(picture(30, 20, seed=1), 16)
    gif_fixture('still_87a', gif(30, 20, [dict(indices=idx16, minimum=4)], palette=bytes(pal16),
                                 version=b'87a'))
    idx8, pal8 = quantised(picture(23, 19, seed=2), 8)
    gif_fixture('interlaced', gif(23, 19, [dict(indices=idx8, minimum=3, interlace=True)],
                                  palette=palette_of([pal8[i:i + 3] for i in range(0, 24, 3)], 8)))
    rng = np.random.default_rng(7)
    noise = rng.integers(0, 4, (60, 72))
    gif_fixture('lzw_deferred', gif(72, 60, [dict(indices=noise, minimum=2, literal=True, deferred=True)],
                                    palette=palette_of([(0, 0, 0), (255, 0, 0), (0, 255, 0), (0, 0, 255)], 4)))

    two = palette_of([(250, 250, 240), (10, 20, 30)], 2)
    f0 = np.zeros((20, 26), np.uint8)
    f0[5:15, 6:20] = 1
    idx256, pal256 = quantised(picture(12, 9, seed=5), 256)
    gif_fixture('local_tables', gif(26, 20, [
        dict(indices=f0, palette=two, minimum=2, delay=30, disposal=1, transparent=0),
        dict(indices=idx16[:8, :10], palette=bytes(pal16), minimum=4, left=3, top=4, delay=50, disposal=3),
        dict(indices=idx256, palette=bytes(pal256) + bytes(768 - len(pal256)), minimum=8, left=13, top=10,
             delay=0, disposal=2, transparent=int(idx256[0, 0])),
        dict(indices=f0[:6, :7], palette=two, minimum=2, left=1, top=1, delay=20, disposal=1),
    ], loop=3))

    frames = []
    for k in range(4):
        a = np.zeros((24, 32, 4), np.uint8)
        a[4 + k * 3: 12 + k * 3, 3 + k * 5: 13 + k * 5] = [220, 40 + 50 * k, 30, 255]
        a[16:20, 2:30] = [20, 90, 200, 255]
        frames.append(Image.fromarray(a, 'RGBA'))
    buf = io.BytesIO()
    frames[0].save(buf, 'GIF', save_all=True, append_images=frames[1:], duration=[40, 80, 120, 160], loop=0,
                   disposal=2)
    gif_fixture('anim_pillow', buf.getvalue())

    frames = []
    for k in range(3):
        a = picture(28, 18, seed=k * 4).copy()
        a[2:8, 2 + k * 6: 10 + k * 6] = [255, 255, 0]
        frames.append(Image.fromarray(a, 'RGB').quantize(32, dither=Image.Dither.NONE))
    buf = io.BytesIO()
    frames[0].save(buf, 'GIF', save_all=True, append_images=frames[1:], duration=70, loop=2, disposal=1)
    gif_fixture('anim_keep', buf.getvalue())

    with open(os.path.join(HERE, 'images_expected.txt'), 'w', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')


if __name__ == '__main__':
    main()
