// SPDX-License-Identifier: Apache-2.0
//
// JPEG, into the same premultiplied RGBA a Bitmap holds -- and back out.
//
// ── Why a plugin needs it after all ─────────────────────────────────────────
//
// png.h said no to JPEG, and for the art a plugin is BUILT from it was right:
// flat colour and hard edges are what PNG is for and what JPEG is worst at.
// What changed is everything else that arrives. A sampler shows the cover of
// the library it loaded. A preset browser shows the photograph the sound
// designer attached. A skin made by a user is a photograph of wood or of
// brushed metal, and every one of those is a JPEG because a camera or a
// photo editor made it. Refusing them sends a plugin author to vendor a
// decoder, and the vendored one is the code nobody here has read.
//
// ── What is read ────────────────────────────────────────────────────────────
//
//   - baseline and extended sequential (SOF0, SOF1) and progressive (SOF2)
//     DCT, 8 bits per sample, Huffman coded -- every JPEG a camera, a phone,
//     a browser or an image editor writes;
//   - any sampling factors from 1 to 4: 4:4:4, 4:2:2, 4:2:0, 4:4:0, 4:1:1
//     and the odd ones, interleaved or one component per scan;
//   - restart markers, at any interval, in either kind of scan;
//   - greyscale, YCbCr, RGB (Adobe transform 0, or components named R, G, B),
//     CMYK and YCCK as Photoshop writes them -- inverted, which is how the
//     browsers read every CMYK JPEG whatever its markers say;
//   - the EXIF orientation, applied by default -- a phone stores its
//     portraits on their side and says so in a tag, and an image drawn
//     without reading it is a picture of somebody lying down -- and reported
//     either way.
//
// Refused by name: arithmetic coding, lossless and hierarchical JPEG, 12-bit
// samples, and a height left for a DNL marker to give later. Each is rare
// enough that a decoder carrying it is mostly carrying risk.
//
// ── The same pixels as libjpeg ──────────────────────────────────────────────
//
// What a user compares against is how the image looks everywhere else, and
// everywhere else is libjpeg. So the arithmetic is libjpeg's, not merely a
// correct DCT: its integer IDCT (jidctint.c, 13-bit constants, two passes,
// its range limiting), its "fancy" upsampling for 2:1 chroma -- a triangle
// filter, three parts the nearer sample to one part the further, with the
// rounding biases alternating so the image does not drift -- and its
// fixed-point YCbCr to RGB. A JPEG through this and through Pillow comes out
// the same to the byte on every fixture in tests/data/images; the test
// reports the largest difference and asserts it small rather than zero, so
// a libjpeg that changes its rounding one day does not fail the build.
//
// ── Every length in the file is attacker-controlled ─────────────────────────
//
// As in png.h. Three limits matter. The pixel count is capped before anything
// is allocated, as there. The header's block count is held against the file's
// size: every block costs at least one bit of DC data, so a 200-byte file
// claiming 8000x8000 is refused rather than given 400 MB of coefficients. And
// the scans are counted, because a progressive file is a list of passes over
// the whole image and nothing in the format stops it repeating one forever.
// Entropy data that runs out is not an error: what was decoded is kept, the
// rest is grey, and the result says so in `warning` -- the same as libjpeg,
// and what a half-downloaded photograph wants.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "bitmap.h"

namespace sonore {
namespace gfx {

/**
 * A decoded JPEG, or a reason it is not one.
 *
 * `warning` is a second kind of answer PNG never needed: the picture is here
 * and usable, but part of it is not what the file meant -- the data ran out,
 * or a code in it was nonsense -- and the rest is filled as libjpeg fills it.
 */
struct JpegImage {
  /** The picture, opaque (alpha 255 everywhere), already turned upright
   *  when `orientationApplied` is set -- so for orientations 5 to 8 its
   *  width and height are the file's swapped. Empty when `error` is set. */
  Bitmap bitmap;
  /** Why there is no picture, in English; empty on success. An error that
   *  comes after some image data was decoded is not one: it becomes the
   *  `warning` and the picture is kept. */
  std::string error;
  /** Set when the picture is here but not all of it is what the file meant:
   *  the entropy data ran out or was corrupt, or the file went wrong after
   *  some of its image data. Undecoded parts are grey. Empty for a clean
   *  file. */
  std::string warning;
  /** The EXIF orientation the file carries, 1 to 8 -- 1, upright, when it
   *  carries none -- whether or not it was applied. */
  int orientation = 1;
  /** The pixels were turned by `orientation`: Options::applyOrientation was
   *  on and the orientation was not 1. */
  bool orientationApplied = false;
  /** The file is a progressive JPEG (SOF2). */
  bool progressive = false;
  /** 1 grey, 3 colour, 4 CMYK or YCCK. */
  int components = 0;
  /** True when there is a picture: `error` is empty. A picture with a
   *  `warning` is ok. */
  bool ok() const { return error.empty(); }
};

namespace detail {
namespace jpeg {

/** Zig-zag order to natural order, with sixteen extra entries of 63 past the
 *  end: a corrupt run length can push the index past the last coefficient,
 *  and libjpeg pads its table the same way rather than test each step. */
inline constexpr uint8_t kNatural[80] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33,
    40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54,
    47, 55, 62, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63};

/**
 * One Huffman table, ready to decode with.
 *
 * Codes of up to nine bits are looked up in one step -- nearly every code in
 * a real file is that short -- and the rest walk the canonical code lengths
 * from ten to sixteen, as the specification's F.2.2.3 does.
 */
struct Huffman {
  bool present = false;
  uint16_t fast[512] = {}; // (length << 8) | symbol; 0 where no short code starts
  int32_t maxCode[18] = {};
  int32_t valueOffset[18] = {};
  uint8_t values[256] = {};

  /** False for a table that is not a prefix code: more codes of a length
   *  than that length has room for. */
  bool build(const uint8_t counts[16], const uint8_t* symbols, int total) {
    *this = Huffman();
    if (total > 256) return false;
    std::memcpy(values, symbols, (size_t) total);
    int32_t code = 0;
    int k = 0;
    for (int length = 1; length <= 16; ++length) {
      const int n = counts[length - 1];
      valueOffset[length] = k - code;
      for (int i = 0; i < n; ++i) {
        // Every code of this length must fit in it. A table that overflows
        // would give two symbols one code, and the decode of the second would
        // read past the symbols the file supplied. Checked BEFORE the code is
        // entered: a short code past its length's room is also an index past
        // the end of `fast` -- two hundred one-bit codes would write 100 KB
        // beyond it.
        if (code >= (1 << length)) return false;
        if (length <= 9) {
          const int shift = 9 - length;
          const int from = code << shift;
          for (int j = 0; j < (1 << shift); ++j)
            fast[from + j] = (uint16_t) ((length << 8) | values[k]);
        }
        ++code;
        ++k;
      }
      maxCode[length] = n ? code - 1 : -1;
      code <<= 1;
    }
    maxCode[17] = 0x7FFFFFFF; // a sentinel nothing reaches: lengths stop at 16
    present = true;
    return true;
  }
};

/**
 * The entropy-coded bits, MSB first, with the byte stuffing undone.
 *
 * A 0xFF in the data is followed by 0x00, which is dropped; 0xFF followed by
 * anything else is a marker, and the data has ended. From then on -- and past
 * the end of the file -- the reader supplies zero bits, as libjpeg does, and
 * counts them: a scan still asking for bits long after its data ended is
 * decoding nothing, and stops.
 */
struct BitReader {
  const uint8_t* data = nullptr;
  size_t size = 0;
  size_t pos = 0;
  uint32_t acc = 0;
  int count = 0;
  bool ended = false;
  uint32_t phantom = 0; // zero bytes supplied after the data ended

  void reset(size_t at) {
    pos = at;
    acc = 0;
    count = 0;
    ended = false;
    phantom = 0;
  }

  void fill() {
    while (count <= 24) {
      uint32_t byte = 0;
      if (!ended && pos < size) {
        byte = data[pos];
        if (byte == 0xFF) {
          if (pos + 1 < size && data[pos + 1] == 0x00) {
            pos += 2;
          } else {
            ended = true; // a marker: pos stays on its 0xFF
            byte = 0;
            ++phantom;
          }
        } else {
          ++pos;
        }
      } else {
        ended = true;
        ++phantom;
      }
      acc |= byte << (24 - count);
      count += 8;
    }
  }

  /** Long past the end of the data: the rest of the scan would be invented. */
  bool exhausted() const { return phantom > 64; }

  uint32_t bits(int n) {
    if (n <= 0) return 0;
    fill();
    const uint32_t v = acc >> (32 - n);
    acc <<= n;
    count -= n;
    return v;
  }

  int bit() { return (int) bits(1); }

  /** A symbol, or -1 for a code the table does not have. */
  int decode(const Huffman& h) {
    fill();
    const uint16_t f = h.fast[acc >> (32 - 9)];
    if (f) {
      const int length = f >> 8;
      acc <<= length;
      count -= length;
      return f & 0xFF;
    }
    for (int length = 10; length <= 16; ++length) {
      const int32_t c = (int32_t) (acc >> (32 - length));
      if (c <= h.maxCode[length]) {
        acc <<= length;
        count -= length;
        const int32_t index = h.valueOffset[length] + c;
        return index >= 0 && index < 256 ? h.values[index] : -1;
      }
    }
    return -1;
  }

  /** The magnitude category `s` read as a signed value -- F.2.2.1's EXTEND. */
  int receiveExtend(int s) {
    if (s <= 0) return 0;
    if (s > 16) s = 16;
    const int v = (int) bits(s);
    return v < (1 << (s - 1)) ? v - (1 << s) + 1 : v;
  }
};

struct Component {
  int id = 0;
  int h = 1, v = 1; // sampling factors
  int tq = 0;       // which quantization table
  int blocksPerLine = 0, blocksPerColumn = 0; // padded to whole MCUs
  int width = 0, height = 0;                  // samples that are image, not padding
  std::vector<int16_t> coefficients;          // 64 per block, natural order
  uint16_t quant[64] = {};
  bool quantLatched = false;
  int dcPredictor = 0;
  int dcTable = 0, acTable = 0;
  std::vector<uint8_t> plane; // after the IDCT: blocksPerLine*8 wide
};

/** libjpeg's post-IDCT range limit: the value wraps in ten bits and is then
 *  clamped. For any coefficient a real encoder writes this is a plain clamp;
 *  the wrap is what libjpeg does with a hostile one, and matching it keeps
 *  the two decoders' output comparable on damaged files too. */
inline uint8_t idctLimit(int64_t v) {
  int w = (int) (v & 1023);
  if (w >= 512) w -= 1024;
  w += 128;
  return (uint8_t) (w < 0 ? 0 : (w > 255 ? 255 : w));
}

/**
 * libjpeg's jpeg_idct_islow, to the bit: dequantize, columns, then rows,
 * with the same 13-bit constants, the same two descaling shifts, and the
 * same shortcut for a column or row with no AC terms -- which is not only an
 * optimisation, it rounds differently, and leaving it out moves a pixel by
 * one here and there.
 */
inline void idctBlock(const int16_t* in, const uint16_t* q, uint8_t* out, int stride) {
  constexpr int kConstBits = 13, kPass1Bits = 2;
  constexpr int64_t k0_298631336 = 2446, k0_390180644 = 3196, k0_541196100 = 4433,
                    k0_765366865 = 6270, k0_899976223 = 7373, k1_175875602 = 9633,
                    k1_501321110 = 12299, k1_847759065 = 15137, k1_961570560 = 16069,
                    k2_053119869 = 16819, k2_562915447 = 20995, k3_072711026 = 25172;
  auto descale = [](int64_t x, int n) { return (x + ((int64_t) 1 << (n - 1))) >> n; };
  int64_t ws[64];

  for (int c = 0; c < 8; ++c) {
    const int16_t* col = in + c;
    const uint16_t* qc = q + c;
    if (col[8] == 0 && col[16] == 0 && col[24] == 0 && col[32] == 0 && col[40] == 0 &&
        col[48] == 0 && col[56] == 0) {
      const int64_t dc = ((int64_t) col[0] * qc[0]) * (1 << kPass1Bits);
      for (int r = 0; r < 8; ++r) ws[r * 8 + c] = dc;
      continue;
    }
    int64_t z2 = (int64_t) col[16] * qc[16], z3 = (int64_t) col[48] * qc[48];
    int64_t z1 = (z2 + z3) * k0_541196100;
    int64_t tmp2 = z1 + z3 * -k1_847759065;
    int64_t tmp3 = z1 + z2 * k0_765366865;
    z2 = (int64_t) col[0] * qc[0];
    z3 = (int64_t) col[32] * qc[32];
    int64_t tmp0 = (z2 + z3) * (1 << kConstBits);
    int64_t tmp1 = (z2 - z3) * (1 << kConstBits);
    const int64_t tmp10 = tmp0 + tmp3, tmp13 = tmp0 - tmp3;
    const int64_t tmp11 = tmp1 + tmp2, tmp12 = tmp1 - tmp2;

    tmp0 = (int64_t) col[56] * qc[56];
    tmp1 = (int64_t) col[40] * qc[40];
    tmp2 = (int64_t) col[24] * qc[24];
    tmp3 = (int64_t) col[8] * qc[8];
    z1 = tmp0 + tmp3;
    z2 = tmp1 + tmp2;
    z3 = tmp0 + tmp2;
    int64_t z4 = tmp1 + tmp3;
    const int64_t z5 = (z3 + z4) * k1_175875602;
    tmp0 *= k0_298631336;
    tmp1 *= k2_053119869;
    tmp2 *= k3_072711026;
    tmp3 *= k1_501321110;
    z1 *= -k0_899976223;
    z2 *= -k2_562915447;
    z3 *= -k1_961570560;
    z4 *= -k0_390180644;
    z3 += z5;
    z4 += z5;
    tmp0 += z1 + z3;
    tmp1 += z2 + z4;
    tmp2 += z2 + z3;
    tmp3 += z1 + z4;

    ws[0 * 8 + c] = descale(tmp10 + tmp3, kConstBits - kPass1Bits);
    ws[7 * 8 + c] = descale(tmp10 - tmp3, kConstBits - kPass1Bits);
    ws[1 * 8 + c] = descale(tmp11 + tmp2, kConstBits - kPass1Bits);
    ws[6 * 8 + c] = descale(tmp11 - tmp2, kConstBits - kPass1Bits);
    ws[2 * 8 + c] = descale(tmp12 + tmp1, kConstBits - kPass1Bits);
    ws[5 * 8 + c] = descale(tmp12 - tmp1, kConstBits - kPass1Bits);
    ws[3 * 8 + c] = descale(tmp13 + tmp0, kConstBits - kPass1Bits);
    ws[4 * 8 + c] = descale(tmp13 - tmp0, kConstBits - kPass1Bits);
  }

  for (int r = 0; r < 8; ++r) {
    const int64_t* w = ws + r * 8;
    uint8_t* o = out + (size_t) r * (size_t) stride;
    if (w[1] == 0 && w[2] == 0 && w[3] == 0 && w[4] == 0 && w[5] == 0 && w[6] == 0 && w[7] == 0) {
      const uint8_t dc = idctLimit(descale(w[0], kPass1Bits + 3));
      for (int i = 0; i < 8; ++i) o[i] = dc;
      continue;
    }
    int64_t z2 = w[2], z3 = w[6];
    int64_t z1 = (z2 + z3) * k0_541196100;
    int64_t tmp2 = z1 + z3 * -k1_847759065;
    int64_t tmp3 = z1 + z2 * k0_765366865;
    int64_t tmp0 = (w[0] + w[4]) * (1 << kConstBits);
    int64_t tmp1 = (w[0] - w[4]) * (1 << kConstBits);
    const int64_t tmp10 = tmp0 + tmp3, tmp13 = tmp0 - tmp3;
    const int64_t tmp11 = tmp1 + tmp2, tmp12 = tmp1 - tmp2;

    tmp0 = w[7];
    tmp1 = w[5];
    tmp2 = w[3];
    tmp3 = w[1];
    z1 = tmp0 + tmp3;
    z2 = tmp1 + tmp2;
    z3 = tmp0 + tmp2;
    int64_t z4 = tmp1 + tmp3;
    const int64_t z5 = (z3 + z4) * k1_175875602;
    tmp0 *= k0_298631336;
    tmp1 *= k2_053119869;
    tmp2 *= k3_072711026;
    tmp3 *= k1_501321110;
    z1 *= -k0_899976223;
    z2 *= -k2_562915447;
    z3 *= -k1_961570560;
    z4 *= -k0_390180644;
    z3 += z5;
    z4 += z5;
    tmp0 += z1 + z3;
    tmp1 += z2 + z4;
    tmp2 += z2 + z3;
    tmp3 += z1 + z4;

    constexpr int kShift = kConstBits + kPass1Bits + 3;
    o[0] = idctLimit(descale(tmp10 + tmp3, kShift));
    o[7] = idctLimit(descale(tmp10 - tmp3, kShift));
    o[1] = idctLimit(descale(tmp11 + tmp2, kShift));
    o[6] = idctLimit(descale(tmp11 - tmp2, kShift));
    o[2] = idctLimit(descale(tmp12 + tmp1, kShift));
    o[5] = idctLimit(descale(tmp12 - tmp1, kShift));
    o[3] = idctLimit(descale(tmp13 + tmp0, kShift));
    o[4] = idctLimit(descale(tmp13 - tmp0, kShift));
  }
}

inline uint8_t clampByte(int v) { return (uint8_t) (v < 0 ? 0 : (v > 255 ? 255 : v)); }

/** libjpeg's YCbCr to RGB, in its fixed point: tables built from the same
 *  16-bit constants and rounded the same way, so the result is the same
 *  integer and not merely close to it. */
struct ColourTables {
  int crR[256], cbB[256], crG[256], cbG[256];
  ColourTables() {
    constexpr int kScale = 16;
    constexpr int64_t kHalf = (int64_t) 1 << (kScale - 1);
    for (int i = 0; i < 256; ++i) {
      const int64_t x = i - 128;
      crR[i] = (int) ((91881 * x + kHalf) >> kScale);   // 1.40200
      cbB[i] = (int) ((116130 * x + kHalf) >> kScale);  // 1.77200
      crG[i] = (int) (-46802 * x);                      // -0.71414
      cbG[i] = (int) (-22554 * x + kHalf);              // -0.34414, with the rounding half
    }
  }
  void toRgb(int y, int cb, int cr, uint8_t& r, uint8_t& g, uint8_t& b) const {
    r = clampByte(y + crR[cr]);
    g = clampByte(y + (int) (((int64_t) cbG[cb] + crG[cr]) >> 16));
    b = clampByte(y + cbB[cb]);
  }
};

inline const ColourTables& colourTables() {
  static const ColourTables tables;
  return tables;
}

/** a * b / 255, rounded: the CMYK products. */
inline uint8_t mul255(int a, int b) {
  const int x = a * b + 128;
  return (uint8_t) ((x + (x >> 8)) >> 8);
}

class Decoder {
public:
  struct Limits {
    uint64_t maxPixels;
    bool applyOrientation;
  };

  Decoder(const uint8_t* data, size_t size, Limits limits) : data_(data), size_(size), limits_(limits) {}

  JpegImage run() {
    JpegImage result;
    if (size_ < 4 || data_[0] != 0xFF || data_[1] != 0xD8) {
      result.error = "not a JPEG file";
      return result;
    }
    size_t at = 2;
    bool sawEnd = false;
    while (at < size_) {
      const size_t marker = findMarker(at);
      if (marker >= size_) break;
      const uint8_t m = data_[marker + 1];
      at = marker + 2;
      if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) continue; // standalone, stray
      if (m == 0xD9) {
        sawEnd = true;
        break;
      }
      if (at + 2 > size_) {
        error_ = "truncated JPEG: a segment header runs past the end of the file";
        break;
      }
      const size_t length = ((size_t) data_[at] << 8) | data_[at + 1];
      if (length < 2 || at + length > size_) {
        error_ = "truncated JPEG: a segment claims more data than the file holds";
        break;
      }
      const uint8_t* body = data_ + at + 2;
      const size_t bodySize = length - 2;
      const size_t next = at + length;

      if (m == 0xC0 || m == 0xC1 || m == 0xC2) {
        if (!readFrame(body, bodySize, m == 0xC2)) break;
      } else if (m == 0xC3 || (m >= 0xC5 && m <= 0xC7) || (m >= 0xC9 && m <= 0xCB) ||
                 (m >= 0xCD && m <= 0xCF)) {
        error_ = m == 0xC3 || m == 0xC7 || m == 0xCB || m == 0xCF
                     ? "lossless JPEG is not supported"
                     : (m >= 0xC9 ? "arithmetic-coded JPEG is not supported"
                                  : "hierarchical JPEG is not supported");
        break;
      } else if (m == 0xC4) {
        if (!readHuffman(body, bodySize)) break;
      } else if (m == 0xCC) {
        error_ = "arithmetic-coded JPEG is not supported";
        break;
      } else if (m == 0xDB) {
        if (!readQuant(body, bodySize)) break;
      } else if (m == 0xDD) {
        if (bodySize < 2) {
          error_ = "malformed JPEG restart interval";
          break;
        }
        restartInterval_ = ((int) body[0] << 8) | body[1];
      } else if (m == 0xDA) {
        size_t after = next;
        if (!readScan(body, bodySize, next, after)) break;
        at = after;
        continue;
      } else if (m == 0xDC) {
        // DNL: a height given after the data, for an encoder that did not
        // know it when it wrote the header. A frame with height 0 is refused
        // at the header, so one arriving here changes nothing.
      } else if (m == 0xE0) {
        if (bodySize >= 5 && std::memcmp(body, "JFIF\0", 5) == 0) sawJfif_ = true;
      } else if (m == 0xE1) {
        readExif(body, bodySize);
      } else if (m == 0xEE) {
        if (bodySize >= 12 && std::memcmp(body, "Adobe", 5) == 0) {
          sawAdobe_ = true;
          adobeTransform_ = body[11];
        }
      }
      at = next;
    }

    result.orientation = orientation_;
    result.progressive = progressive_;
    result.components = (int) components_.size();
    if (!error_.empty()) {
      // An error after the image data is not the image's problem: a
      // decodable picture with junk after it is still a picture.
      if (scans_ == 0) {
        result.error = error_;
        return result;
      }
      warning_ = error_;
    }
    if (!haveFrame_) {
      result.error = "JPEG has no frame header";
      return result;
    }
    if (scans_ == 0) {
      result.error = "JPEG has no image data";
      return result;
    }
    (void) sawEnd;
    output(result);
    result.warning = warning_;
    return result;
  }

private:
  /** The next marker at or after `at`: an 0xFF, any fill bytes of 0xFF, and
   *  a code that is neither 0x00 (stuffing) nor 0xFF. Returns the offset of
   *  the last 0xFF before the code, or size_ for none. */
  size_t findMarker(size_t at) const {
    while (at + 1 < size_) {
      if (data_[at] != 0xFF) {
        ++at;
        continue;
      }
      size_t i = at;
      while (i + 1 < size_ && data_[i + 1] == 0xFF) ++i;
      if (i + 1 >= size_) return size_;
      if (data_[i + 1] != 0x00) return i;
      at = i + 2;
    }
    return size_;
  }

  bool fail(const char* why) {
    error_ = why;
    return false;
  }

  bool readFrame(const uint8_t* p, size_t n, bool progressive) {
    if (haveFrame_) return fail("JPEG has a second frame header");
    if (n < 6) return fail("malformed JPEG frame header");
    if (p[0] != 8) return fail(p[0] == 12 ? "12-bit JPEGs are not supported" : "unsupported JPEG sample precision");
    height_ = ((int) p[1] << 8) | p[2];
    width_ = ((int) p[3] << 8) | p[4];
    const int count = p[5];
    if (width_ == 0) return fail("JPEG has no pixels");
    if (height_ == 0) return fail("JPEG whose height arrives in a DNL marker is not supported");
    if (count != 1 && count != 3 && count != 4) return fail("unsupported number of JPEG components");
    if (n < 6 + (size_t) count * 3) return fail("malformed JPEG frame header");
    if ((uint64_t) width_ * (uint64_t) height_ > limits_.maxPixels)
      return fail("JPEG is larger than this decoder will allocate");

    components_.assign((size_t) count, Component());
    hMax_ = vMax_ = 1;
    for (int i = 0; i < count; ++i) {
      Component& c = components_[(size_t) i];
      c.id = p[6 + i * 3];
      c.h = p[7 + i * 3] >> 4;
      c.v = p[7 + i * 3] & 15;
      c.tq = p[8 + i * 3];
      if (c.h < 1 || c.h > 4 || c.v < 1 || c.v > 4) return fail("JPEG sampling factor out of range");
      if (c.tq > 3) return fail("JPEG names a quantization table that cannot exist");
      hMax_ = (std::max)(hMax_, c.h);
      vMax_ = (std::max)(vMax_, c.v);
    }
    mcusPerLine_ = (width_ + 8 * hMax_ - 1) / (8 * hMax_);
    mcusPerColumn_ = (height_ + 8 * vMax_ - 1) / (8 * vMax_);

    uint64_t blocks = 0;
    for (Component& c : components_) {
      c.blocksPerLine = mcusPerLine_ * c.h;
      c.blocksPerColumn = mcusPerColumn_ * c.v;
      c.width = (int) (((int64_t) width_ * c.h + hMax_ - 1) / hMax_);
      c.height = (int) (((int64_t) height_ * c.v + vMax_ - 1) / vMax_);
      blocks += (uint64_t) c.blocksPerLine * (uint64_t) c.blocksPerColumn;
    }
    // Every block costs at least one bit of DC data in some scan. A header
    // asking for more blocks than the file has bits is describing a picture
    // the file does not contain, and is refused before 128 bytes per block
    // of coefficients are allocated for it.
    if (blocks > (uint64_t) size_ * 8u + 64u) return fail("JPEG claims more blocks than the file could hold");
    for (Component& c : components_)
      c.coefficients.assign((size_t) c.blocksPerLine * (size_t) c.blocksPerColumn * 64u, 0);
    progressive_ = progressive;
    haveFrame_ = true;
    return true;
  }

  bool readHuffman(const uint8_t* p, size_t n) {
    size_t at = 0;
    while (at < n) {
      if (at + 17 > n) return fail("malformed JPEG Huffman table");
      const int tc = p[at] >> 4, th = p[at] & 15;
      if (tc > 1 || th > 3) return fail("malformed JPEG Huffman table");
      int total = 0;
      for (int i = 0; i < 16; ++i) total += p[at + 1 + i];
      if (at + 17 + (size_t) total > n || total > 256) return fail("malformed JPEG Huffman table");
      // A DC symbol is a magnitude category, and an 8-bit sample's DC
      // difference needs at most 11 of them; libjpeg refuses past 15.
      if (tc == 0)
        for (int i = 0; i < total; ++i)
          if (p[at + 17 + (size_t) i] > 15) return fail("JPEG DC Huffman table has an impossible symbol");
      Huffman& table = tc == 0 ? dc_[th] : ac_[th];
      if (!table.build(p + at + 1, p + at + 17, total)) return fail("JPEG Huffman table is not a prefix code");
      at += 17 + (size_t) total;
    }
    return true;
  }

  bool readQuant(const uint8_t* p, size_t n) {
    size_t at = 0;
    while (at < n) {
      const int pq = p[at] >> 4, tq = p[at] & 15;
      if (pq > 1 || tq > 3) return fail("malformed JPEG quantization table");
      const size_t need = 1 + 64u * (pq ? 2u : 1u);
      if (at + need > n) return fail("malformed JPEG quantization table");
      for (int i = 0; i < 64; ++i) {
        const uint16_t v = pq ? (uint16_t) ((p[at + 1 + i * 2] << 8) | p[at + 2 + i * 2]) : p[at + 1 + i];
        quant_[tq][kNatural[i]] = v;
      }
      haveQuant_[tq] = true;
      at += need;
    }
    return true;
  }

  // ── EXIF ───────────────────────────────────────────────────────────────

  void readExif(const uint8_t* p, size_t n) {
    if (n < 14 || std::memcmp(p, "Exif\0\0", 6) != 0) return;
    const uint8_t* t = p + 6;
    const size_t tn = n - 6;
    bool little = false;
    if (t[0] == 'I' && t[1] == 'I') little = true;
    else if (!(t[0] == 'M' && t[1] == 'M')) return;
    auto u16 = [&](size_t o) -> uint32_t {
      if (o + 2 > tn) return 0;
      return little ? (uint32_t) (t[o] | (t[o + 1] << 8)) : (uint32_t) ((t[o] << 8) | t[o + 1]);
    };
    auto u32 = [&](size_t o) -> uint32_t {
      if (o + 4 > tn) return 0;
      return little ? ((uint32_t) t[o] | ((uint32_t) t[o + 1] << 8) | ((uint32_t) t[o + 2] << 16) |
                       ((uint32_t) t[o + 3] << 24))
                    : (((uint32_t) t[o] << 24) | ((uint32_t) t[o + 1] << 16) | ((uint32_t) t[o + 2] << 8) |
                       (uint32_t) t[o + 3]);
    };
    if (u16(2) != 42) return;
    const size_t ifd = u32(4);
    if (ifd + 2 > tn) return;
    const uint32_t entries = u16(ifd);
    for (uint32_t i = 0; i < entries && i < 512; ++i) {
      const size_t e = ifd + 2 + (size_t) i * 12;
      if (e + 12 > tn) return;
      if (u16(e) != 0x0112) continue;
      // SHORT, count 1: the value sits in the first two bytes of the field
      // -- or a LONG, which a few writers use.
      const uint32_t type = u16(e + 2);
      const uint32_t value = type == 3 ? u16(e + 8) : (type == 4 ? u32(e + 8) : 0);
      if (value >= 1 && value <= 8) orientation_ = (int) value;
      return;
    }
  }

  // ── Scans ──────────────────────────────────────────────────────────────

  bool readScan(const uint8_t* p, size_t n, size_t dataStart, size_t& after) {
    if (!haveFrame_) return fail("JPEG scan before its frame header");
    // A real progressive file has ten or so; this many is a file looping.
    if (scans_ >= 256) return fail("JPEG has more scans than any encoder writes");
    if (n < 1) return fail("malformed JPEG scan header");
    const int count = p[0];
    if (count < 1 || count > 4 || n < 4 + (size_t) count * 2) return fail("malformed JPEG scan header");
    std::vector<Component*> scan;
    int blocksInMcu = 0;
    for (int i = 0; i < count; ++i) {
      const int id = p[1 + i * 2];
      Component* found = nullptr;
      for (Component& c : components_)
        if (c.id == id) {
          found = &c;
          break;
        }
      if (!found) return fail("JPEG scan names a component the frame does not have");
      for (Component* already : scan)
        if (already == found) return fail("JPEG scan names one component twice");
      found->dcTable = p[2 + i * 2] >> 4;
      found->acTable = p[2 + i * 2] & 15;
      if (found->dcTable > 3 || found->acTable > 3) return fail("JPEG scan names a Huffman table that cannot exist");
      blocksInMcu += found->h * found->v;
      scan.push_back(found);
    }
    if (count > 1 && blocksInMcu > 10) return fail("JPEG scan has more blocks per MCU than the standard allows");
    const size_t tail = 1 + (size_t) count * 2;
    int ss = p[tail], se = p[tail + 1], ah = p[tail + 2] >> 4, al = p[tail + 2] & 15;
    if (!progressive_) {
      ss = 0;
      se = 63;
      ah = al = 0;
    } else {
      if (ss > 63 || se > 63 || se < ss) return fail("JPEG progressive scan has an impossible band");
      if ((ss == 0) != (se == 0)) return fail("JPEG progressive scan mixes DC and AC");
      if (ss > 0 && count != 1) return fail("JPEG progressive AC scan has more than one component");
      if (al > 13 || ah > 13) return fail("JPEG progressive scan shifts past the coefficients");
    }

    // The quantization tables are fixed for a component at its first scan,
    // as libjpeg latches them: a DQT between scans changes only components
    // not yet seen.
    for (Component* c : scan) {
      if (!c->quantLatched) {
        if (!haveQuant_[c->tq]) return fail("JPEG uses a quantization table it never defined");
        std::memcpy(c->quant, quant_[c->tq], sizeof(c->quant));
        c->quantLatched = true;
      }
      const bool needDc = !progressive_ || (ss == 0 && ah == 0);
      const bool needAc = !progressive_ || ss > 0;
      if (needDc && !dc_[c->dcTable].present) return fail("JPEG uses a Huffman table it never defined");
      if (needAc && !ac_[c->acTable].present) return fail("JPEG uses a Huffman table it never defined");
      c->dcPredictor = 0;
    }

    ++scans_;
    reader_.data = data_;
    reader_.size = size_;
    reader_.reset(dataStart);
    eobRun_ = 0;
    ss_ = ss;
    se_ = se;
    ah_ = ah;
    al_ = al;
    damaged_ = false;

    int untilRestart = restartInterval_;
    auto restart = [&]() {
      // To the marker, wherever the reader stopped: the bits left in its
      // accumulator are padding.
      const size_t marker = findMarker(reader_.pos);
      if (marker < size_ && data_[marker + 1] >= 0xD0 && data_[marker + 1] <= 0xD7) {
        reader_.reset(marker + 2);
      } else {
        // No restart marker where one must be: the data from here on cannot
        // be trusted to start where it should. Leave the reader dry.
        reader_.reset(marker);
        reader_.ended = true;
        damaged_ = true;
      }
      for (Component* c : scan) c->dcPredictor = 0;
      eobRun_ = 0;
      untilRestart = restartInterval_;
    };

    if (count == 1) {
      Component& c = *scan[0];
      const int cols = (c.width + 7) / 8, rows = (c.height + 7) / 8;
      for (int by = 0; by < rows && !stopped(); ++by)
        for (int bx = 0; bx < cols && !stopped(); ++bx) {
          if (restartInterval_ && untilRestart == 0) restart();
          decodeBlock(c, &c.coefficients[((size_t) by * (size_t) c.blocksPerLine + (size_t) bx) * 64u]);
          if (restartInterval_) --untilRestart;
        }
    } else {
      for (int my = 0; my < mcusPerColumn_ && !stopped(); ++my)
        for (int mx = 0; mx < mcusPerLine_ && !stopped(); ++mx) {
          if (restartInterval_ && untilRestart == 0) restart();
          for (Component* cp : scan) {
            Component& c = *cp;
            for (int v = 0; v < c.v; ++v)
              for (int h = 0; h < c.h; ++h) {
                const size_t row = (size_t) my * (size_t) c.v + (size_t) v;
                const size_t col = (size_t) mx * (size_t) c.h + (size_t) h;
                decodeBlock(c, &c.coefficients[(row * (size_t) c.blocksPerLine + col) * 64u]);
              }
          }
          if (restartInterval_) --untilRestart;
        }
    }
    if (reader_.exhausted() || damaged_) {
      if (warning_.empty())
        warning_ = reader_.exhausted() ? "JPEG data ends before the image does; the rest is grey"
                                       : "JPEG data is damaged; part of the image is missing";
    }
    // Wherever the data stopped, the next segment is at the next marker.
    const size_t marker = findMarker(reader_.pos < dataStart ? dataStart : reader_.pos);
    after = marker;
    return true;
  }

  bool stopped() const { return reader_.exhausted(); }

  /** The DC predictor's sum, wrapping rather than overflowing: a hostile
   *  file can add the largest difference to it a hundred million times, and
   *  a signed overflow is undefined where a wrap is merely wrong. */
  static int addWrapping(int a, int b) { return (int) ((uint32_t) a + (uint32_t) b); }

  void corrupt() {
    damaged_ = true;
    // Nothing after a bad code means what it says: run the reader dry so the
    // scan ends here, with the coefficients so far kept.
    reader_.ended = true;
    reader_.phantom = 1000;
  }

  void decodeBlock(Component& c, int16_t* block) {
    if (!progressive_) {
      decodeSequential(c, block);
    } else if (ss_ == 0) {
      if (ah_ == 0) {
        const int t = reader_.decode(dc_[c.dcTable]);
        if (t < 0) return corrupt();
        c.dcPredictor = addWrapping(c.dcPredictor, reader_.receiveExtend(t));
        block[0] = (int16_t) ((uint32_t) c.dcPredictor << al_);
      } else if (reader_.bit()) {
        block[0] = (int16_t) (block[0] | (1 << al_));
      }
    } else if (ah_ == 0) {
      decodeAcFirst(c, block);
    } else {
      decodeAcRefine(c, block);
    }
  }

  void decodeSequential(Component& c, int16_t* block) {
    const int t = reader_.decode(dc_[c.dcTable]);
    if (t < 0) return corrupt();
    c.dcPredictor = addWrapping(c.dcPredictor, reader_.receiveExtend(t));
    block[0] = (int16_t) c.dcPredictor;
    const Huffman& ac = ac_[c.acTable];
    for (int k = 1; k < 64; ++k) {
      const int rs = reader_.decode(ac);
      if (rs < 0) return corrupt();
      const int r = rs >> 4, s = rs & 15;
      if (s) {
        k += r;
        block[kNatural[k]] = (int16_t) reader_.receiveExtend(s);
      } else {
        if (r != 15) break;
        k += 15;
      }
    }
  }

  void decodeAcFirst(Component& c, int16_t* block) {
    if (eobRun_ > 0) {
      --eobRun_;
      return;
    }
    const Huffman& ac = ac_[c.acTable];
    for (int k = ss_; k <= se_; ++k) {
      const int rs = reader_.decode(ac);
      if (rs < 0) return corrupt();
      const int r = rs >> 4, s = rs & 15;
      if (s) {
        k += r;
        block[kNatural[k]] = (int16_t) (reader_.receiveExtend(s) * (1 << al_));
      } else if (r == 15) {
        k += 15;
      } else {
        eobRun_ = 1 << r;
        if (r) eobRun_ += (int) reader_.bits(r);
        --eobRun_;
        break;
      }
    }
  }

  /** G.1.2.3, as libjpeg's decode_mcu_AC_refine has it: each coefficient
   *  already nonzero takes one correction bit; a zero one may become +-1 at
   *  this bit position. The run counts only the ZERO coefficients it
   *  passes, which is what makes this the hard one. */
  void decodeAcRefine(Component& c, int16_t* block) {
    const int p1 = 1 << al_, m1 = -p1;
    const Huffman& ac = ac_[c.acTable];
    int k = ss_;
    auto refine = [&](int16_t& coef) {
      if (reader_.bit() && (coef & p1) == 0) coef = (int16_t) (coef >= 0 ? coef + p1 : coef + m1);
    };
    if (eobRun_ == 0) {
      for (; k <= se_; ++k) {
        const int rs = reader_.decode(ac);
        if (rs < 0) return corrupt();
        int r = rs >> 4, s = rs & 15;
        if (s) {
          // The size of a newly nonzero coefficient is always 1; anything
          // else is a damaged file, read as 1 as libjpeg reads it.
          s = reader_.bit() ? p1 : m1;
        } else if (r != 15) {
          eobRun_ = 1 << r;
          if (r) eobRun_ += (int) reader_.bits(r);
          break;
        }
        do {
          int16_t& coef = block[kNatural[k]];
          if (coef != 0) {
            refine(coef);
          } else {
            if (--r < 0) break;
          }
          ++k;
        } while (k <= se_);
        if (s) block[kNatural[k]] = (int16_t) s;
        if (stopped()) return;
      }
    }
    if (eobRun_ > 0) {
      for (; k <= se_; ++k) {
        int16_t& coef = block[kNatural[k]];
        if (coef != 0) refine(coef);
      }
      --eobRun_;
    }
  }

  // ── Output ─────────────────────────────────────────────────────────────

  enum class Space { Grey, YCbCr, Rgb, Cmyk, Ycck };

  /** Which colour space the components are in, decided as libjpeg decides
   *  it: a JFIF marker means YCbCr; an Adobe marker's transform says;
   *  otherwise three components named R, G and B are RGB and any other
   *  three are YCbCr, and four without Adobe's word are plain CMYK. */
  Space colourSpace() const {
    const size_t n = components_.size();
    if (n == 1) return Space::Grey;
    if (n == 3) {
      if (sawJfif_) return Space::YCbCr;
      if (sawAdobe_) return adobeTransform_ == 0 ? Space::Rgb : Space::YCbCr;
      if (components_[0].id == 'R' && components_[1].id == 'G' && components_[2].id == 'B') return Space::Rgb;
      return Space::YCbCr;
    }
    if (sawAdobe_) return adobeTransform_ == 0 ? Space::Cmyk : Space::Ycck;
    return Space::Cmyk;
  }

  /**
   * A component at full resolution, `width_` by `height_`, from its plane,
   * resampled as libjpeg resamples it: unchanged at full size; for 2:1 in
   * either or both directions its triangle filter (jdsample.c's h2v1, h1v2
   * and h2v2 "fancy" upsamplers, rounding biases included); for any other
   * whole ratio, each sample repeated; and for a ratio that is not whole --
   * which libjpeg refuses -- the nearest sample.
   */
  void upsample(const Component& c, std::vector<uint8_t>& out) const {
    const int W = width_, H = height_;
    out.assign((size_t) W * (size_t) H, 0);
    const size_t stride = (size_t) c.blocksPerLine * 8u;
    const uint8_t* plane = c.plane.data();
    const int dw = c.width, dh = c.height;
    auto at = [&](int x, int y) -> int {
      x = x < 0 ? 0 : (x >= dw ? dw - 1 : x);
      y = y < 0 ? 0 : (y >= dh ? dh - 1 : y);
      return plane[(size_t) y * stride + (size_t) x];
    };

    if (c.h == hMax_ && c.v == vMax_) {
      for (int y = 0; y < H; ++y) std::memcpy(&out[(size_t) y * (size_t) W], plane + (size_t) y * stride, (size_t) W);
      return;
    }
    const bool whole = hMax_ % c.h == 0 && vMax_ % c.v == 0;
    const int hr = whole ? hMax_ / c.h : 0, vr = whole ? vMax_ / c.v : 0;
    std::vector<int> row((size_t) dw);

    if (hr == 2 && vr == 1 && dw > 2) {
      for (int y = 0; y < H; ++y) {
        const uint8_t* in = plane + (size_t) y * stride;
        uint8_t* o = &out[(size_t) y * (size_t) W];
        for (int x = 0; x < dw; ++x) {
          const int here = in[x] * 3;
          if (2 * x < W) o[2 * x] = x == 0 ? in[0] : (uint8_t) ((here + in[x - 1] + 1) >> 2);
          if (2 * x + 1 < W) o[2 * x + 1] = x == dw - 1 ? in[x] : (uint8_t) ((here + in[x + 1] + 2) >> 2);
        }
      }
      return;
    }
    if (hr == 1 && vr == 2) {
      for (int y = 0; y < H; ++y) {
        const int iy = y / 2;
        const int other = (y & 1) ? iy + 1 : iy - 1;
        const int bias = (y & 1) ? 2 : 1;
        uint8_t* o = &out[(size_t) y * (size_t) W];
        for (int x = 0; x < W; ++x) o[x] = (uint8_t) ((at(x, iy) * 3 + at(x, other) + bias) >> 2);
      }
      return;
    }
    if (hr == 2 && vr == 2 && dw > 2) {
      for (int y = 0; y < H; ++y) {
        const int iy = y / 2;
        const int other = (y & 1) ? iy + 1 : iy - 1;
        for (int x = 0; x < dw; ++x) row[(size_t) x] = at(x, iy) * 3 + at(x, other);
        uint8_t* o = &out[(size_t) y * (size_t) W];
        for (int x = 0; x < dw; ++x) {
          const int here = row[(size_t) x] * 3;
          const int even = x == 0 ? row[0] * 4 + 8 : here + row[(size_t) x - 1] + 8;
          const int odd = x == dw - 1 ? row[(size_t) x] * 4 + 7 : here + row[(size_t) x + 1] + 7;
          if (2 * x < W) o[2 * x] = (uint8_t) (even >> 4);
          if (2 * x + 1 < W) o[2 * x + 1] = (uint8_t) (odd >> 4);
        }
      }
      return;
    }
    // Whole ratios: repeated. Anything else: the nearest sample.
    for (int y = 0; y < H; ++y) {
      const int sy = whole ? y / vr : (int) (((int64_t) y * c.v) / vMax_);
      uint8_t* o = &out[(size_t) y * (size_t) W];
      for (int x = 0; x < W; ++x) {
        const int sx = whole ? x / hr : (int) (((int64_t) x * c.h) / hMax_);
        o[x] = (uint8_t) at(sx, sy);
      }
    }
  }

  void output(JpegImage& result) {
    // The IDCT, every block of every component, into planes.
    for (Component& c : components_) {
      if (!c.quantLatched) std::memset(c.quant, 0, sizeof(c.quant)); // never scanned: grey
      const size_t stride = (size_t) c.blocksPerLine * 8u;
      c.plane.assign(stride * (size_t) c.blocksPerColumn * 8u, 128);
      for (int by = 0; by < c.blocksPerColumn; ++by)
        for (int bx = 0; bx < c.blocksPerLine; ++bx)
          idctBlock(&c.coefficients[((size_t) by * (size_t) c.blocksPerLine + (size_t) bx) * 64u], c.quant,
                    &c.plane[(size_t) by * 8u * stride + (size_t) bx * 8u], (int) stride);
      std::vector<int16_t>().swap(c.coefficients);
    }
    std::vector<std::vector<uint8_t>> full(components_.size());
    for (size_t i = 0; i < components_.size(); ++i) {
      upsample(components_[i], full[i]);
      std::vector<uint8_t>().swap(components_[i].plane);
    }

    const int W = width_, H = height_;
    const int o = limits_.applyOrientation ? orientation_ : 1;
    const bool swaps = o >= 5;
    result.bitmap.resize(swaps ? H : W, swaps ? W : H);
    result.orientationApplied = limits_.applyOrientation && orientation_ != 1;
    uint8_t* px = result.bitmap.data();
    const int outW = result.bitmap.width();
    const Space space = colourSpace();
    const ColourTables& t = colourTables();
    // Photoshop writes CMYK inverted -- 0 is full ink -- and so nearly every
    // CMYK JPEG there is is inverted, Adobe marker or not. Pillow, Chromium
    // and Firefox all read every one that way, and so does this.
    const bool inverted = true;

    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) {
        const size_t i = (size_t) y * (size_t) W + (size_t) x;
        uint8_t r = 0, g = 0, b = 0;
        switch (space) {
          case Space::Grey: r = g = b = full[0][i]; break;
          case Space::Rgb:
            r = full[0][i];
            g = full[1][i];
            b = full[2][i];
            break;
          case Space::YCbCr: t.toRgb(full[0][i], full[1][i], full[2][i], r, g, b); break;
          case Space::Cmyk:
          case Space::Ycck: {
            int c0, c1, c2;
            if (space == Space::Ycck) {
              // YCCK is CMY stored as the YCbCr of its complement, K as is.
              uint8_t rr, gg, bb;
              t.toRgb(full[0][i], full[1][i], full[2][i], rr, gg, bb);
              c0 = 255 - rr;
              c1 = 255 - gg;
              c2 = 255 - bb;
            } else {
              c0 = full[0][i];
              c1 = full[1][i];
              c2 = full[2][i];
            }
            int k = full[3][i];
            if (inverted) {
              c0 = 255 - c0;
              c1 = 255 - c1;
              c2 = 255 - c2;
              k = 255 - k;
            }
            // Ink to light: each channel is what its own ink and the black
            // leave of white. Naive, as every decoder without a colour
            // profile is -- a press profile is a colour-management decision
            // and not a decoder's.
            r = mul255(255 - c0, 255 - k);
            g = mul255(255 - c1, 255 - k);
            b = mul255(255 - c2, 255 - k);
            break;
          }
        }
        int dx = x, dy = y;
        switch (o) {
          case 2: dx = W - 1 - x; break;
          case 3: dx = W - 1 - x; dy = H - 1 - y; break;
          case 4: dy = H - 1 - y; break;
          case 5: dx = y; dy = x; break;
          case 6: dx = H - 1 - y; dy = x; break;
          case 7: dx = H - 1 - y; dy = W - 1 - x; break;
          case 8: dx = y; dy = W - 1 - x; break;
          default: break;
        }
        uint8_t* p = px + ((size_t) dy * (size_t) outW + (size_t) dx) * 4u;
        p[0] = r;
        p[1] = g;
        p[2] = b;
        p[3] = 255; // a JPEG is opaque: premultiplied is the same bytes
      }
  }

  const uint8_t* data_;
  size_t size_;
  Limits limits_;
  std::string error_, warning_;

  bool haveFrame_ = false, progressive_ = false;
  int width_ = 0, height_ = 0;
  int hMax_ = 1, vMax_ = 1;
  int mcusPerLine_ = 0, mcusPerColumn_ = 0;
  std::vector<Component> components_;
  Huffman dc_[4], ac_[4];
  uint16_t quant_[4][64] = {};
  bool haveQuant_[4] = {};
  int restartInterval_ = 0;
  int scans_ = 0;

  bool sawJfif_ = false, sawAdobe_ = false;
  int adobeTransform_ = -1;
  int orientation_ = 1;

  BitReader reader_;
  int eobRun_ = 0;
  int ss_ = 0, se_ = 63, ah_ = 0, al_ = 0;
  bool damaged_ = false;
};

} // namespace jpeg
} // namespace detail

/**
 * Decodes a JPEG from bytes in memory into premultiplied RGBA, with libjpeg's
 * arithmetic so the pixels match what every other program shows. What is read
 * and what is refused by name is in the file comment.
 *
 * Stateless: decode() is a static function of its input, safe from any thread
 * but not the audio thread (it allocates the image). A file it cannot read
 * comes back with JpegImage::error set; one damaged partway decodes what it
 * can and says so in `warning`.
 */
class JpegDecoder {
public:
  /** As PngDecoder's: no image a plugin shows is near it, and a header
   *  claiming 60000x60000 is refused before it can ask for the memory. */
  static constexpr uint64_t kMaxPixels = 64ull * 1024ull * 1024ull;

  /** Whether to turn the picture upright, and the most pixels it may have. */
  struct Options {
    /** Turn the picture upright by its EXIF orientation. Off, the pixels are
     *  as stored and JpegImage::orientation says how to turn them. */
    bool applyOrientation = true;
    /** The most pixels (width times height) a file may claim; a larger one
     *  is refused with an error before anything is allocated. */
    uint64_t maxPixels = kMaxPixels;
  };

  // Overloads, not `= Options()`: a default argument of a nested type is
  // read while the class is still incomplete, and gcc and clang refuse
  // Options' member initializers there. MSVC accepts it, which is how
  // this went unnoticed.
  /** Decodes `size` bytes at `data` with the default Options. The bytes are
   *  only read during the call. Null data is read as an empty file, which
   *  is an error. */
  static JpegImage decode(const uint8_t* data, size_t size) { return decode(data, size, Options()); }
  /** The same, with `options`. */
  static JpegImage decode(const uint8_t* data, size_t size, const Options& options) {
    if (data == nullptr) size = 0;
    detail::jpeg::Decoder decoder(data, size, {options.maxPixels, options.applyOrientation});
    return decoder.run();
  }

  /** Decodes the whole of `bytes` -- a file read into memory. */
  static JpegImage decode(const std::vector<uint8_t>& bytes) { return decode(bytes, Options()); }
  /** The same, with `options`. */
  static JpegImage decode(const std::vector<uint8_t>& bytes, const Options& options) {
    return decode(bytes.data(), bytes.size(), options);
  }
};

/**
 * A Bitmap to a baseline JPEG: for a screenshot of an editor, a thumbnail
 * of a preset, a picture a plugin sends somewhere that wants a photograph's
 * format rather than a lossless one.
 *
 * Standard tables (the specification's Annex K) scaled by quality as the IJG
 * encoder scales them, so quality 75 here and in every photo editor are the
 * same table; 4:2:0 chroma by default, 4:4:4 for art with coloured edges.
 * JPEG has no alpha: each pixel is composited over `background` first, as a
 * browser does when it flattens a PNG -- a transparent pixel left as its
 * stored colour would come out black.
 */
class JpegEncoder {
public:
  /** Quality 1 to 100 (held to that range), whether chroma is halved both
   *  ways (4:2:0) or kept whole (4:4:4), and what transparency is flattened
   *  onto. */
  struct Options {
    /** 1 (smallest, worst) to 100 (largest, best), with the IJG encoder's
     *  scaling, so 75 is the same table as libjpeg's 75. Outside the range
     *  it is held to 1 or 100. */
    int quality = 90;        // 1 to 100
    /** True: 4:2:0, chroma averaged over 2x2 pixels, the smaller file.
     *  False: 4:4:4, for art whose coloured edges would bleed. */
    bool subsampleChroma = true;
    /** What each pixel is composited over before encoding, as JPEG has no
     *  alpha. Its own alpha is not used: it counts as opaque. */
    Colour background{255, 255, 255, 255};
  };

  // Overloads, not `= Options()`: a default argument of a nested type is
  // read while the class is still incomplete, and gcc and clang refuse
  // Options' member initializers there. MSVC accepts it, which is how
  // this went unnoticed.
  /** The bytes of a baseline JFIF file, with the default Options (quality
   *  90, 4:2:0, over white). Empty when the image has no pixels or a side
   *  over 65535, the most a JPEG header can hold. Allocates: not for the
   *  audio thread. */
  static std::vector<uint8_t> encode(const Bitmap& image) { return encode(image, Options()); }
  /** The same, with `options`. */
  static std::vector<uint8_t> encode(const Bitmap& image, const Options& options) {
    std::vector<uint8_t> out;
    const int W = image.width(), H = image.height();
    if (W <= 0 || H <= 0 || W > 65535 || H > 65535) return out;
    Writer w(out);

    // Tables, scaled as IJG's jpeg_quality_scaling does.
    static const uint8_t kLuma[64] = {16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
                                      14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
                                      18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
                                      49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
    static const uint8_t kChroma[64] = {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
                                        24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
                                        99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
                                        99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};
    int quality = options.quality < 1 ? 1 : (options.quality > 100 ? 100 : options.quality);
    const int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
    uint8_t q[2][64];
    for (int i = 0; i < 64; ++i) {
      const int l = (kLuma[i] * scale + 50) / 100, c = (kChroma[i] * scale + 50) / 100;
      q[0][i] = (uint8_t) (l < 1 ? 1 : (l > 255 ? 255 : l));
      q[1][i] = (uint8_t) (c < 1 ? 1 : (c > 255 ? 255 : c));
    }

    w.marker(0xD8);
    // JFIF APP0: version 1.1, no density, no thumbnail.
    static const uint8_t kJfif[] = {'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0};
    w.segment(0xE0, kJfif, sizeof(kJfif));
    for (int t = 0; t < 2; ++t) {
      uint8_t body[65];
      body[0] = (uint8_t) t;
      for (int i = 0; i < 64; ++i) body[1 + i] = q[t][detail::jpeg::kNatural[i]];
      w.segment(0xDB, body, sizeof(body));
    }
    const int hs = options.subsampleChroma ? 2 : 1;
    {
      uint8_t sof[15] = {8,
                         (uint8_t) (H >> 8), (uint8_t) H, (uint8_t) (W >> 8), (uint8_t) W, 3,
                         1, (uint8_t) ((hs << 4) | hs), 0,
                         2, 0x11, 1,
                         3, 0x11, 1};
      w.segment(0xC0, sof, sizeof(sof));
    }
    for (int t = 0; t < 4; ++t) {
      const StandardTable& s = standardTable(t);
      std::vector<uint8_t> body;
      body.push_back((uint8_t) (((t & 1) << 4) | (t >> 1))); // Tc: odd tables are AC
      body.insert(body.end(), s.counts, s.counts + 16);
      body.insert(body.end(), s.values, s.values + s.count);
      w.segment(0xC4, body.data(), body.size());
    }
    static const uint8_t kSos[] = {3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0};
    w.segment(0xDA, kSos, sizeof(kSos));

    // The pixels as straight-alpha colour over the background, in YCbCr.
    const size_t n = (size_t) W * (size_t) H;
    std::vector<float> Y(n), Cb(n), Cr(n);
    const Colour bg = options.background;
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) {
        const PremulColour p = image.pixelAt(x, y);
        const float a = (float) p.a / 255.0f;
        // Premultiplied over an opaque background is one add per channel.
        const float r = (float) p.r + (float) bg.r * (1.0f - a);
        const float g = (float) p.g + (float) bg.g * (1.0f - a);
        const float b = (float) p.b + (float) bg.b * (1.0f - a);
        const size_t i = (size_t) y * (size_t) W + (size_t) x;
        Y[i] = 0.299f * r + 0.587f * g + 0.114f * b;
        Cb[i] = -0.168736f * r - 0.331264f * g + 0.5f * b + 128.0f;
        Cr[i] = 0.5f * r - 0.418688f * g - 0.081312f * b + 128.0f;
      }

    Encoder e(w, q);
    const int mcu = 8 * hs;
    for (int my = 0; my < (H + mcu - 1) / mcu; ++my)
      for (int mx = 0; mx < (W + mcu - 1) / mcu; ++mx) {
        for (int by = 0; by < hs; ++by)
          for (int bx = 0; bx < hs; ++bx) {
            float block[64];
            gather(Y, W, H, mx * mcu + bx * 8, my * mcu + by * 8, 1, block);
            e.block(block, 0);
          }
        float block[64];
        gather(Cb, W, H, mx * mcu, my * mcu, hs, block);
        e.block(block, 1);
        gather(Cr, W, H, mx * mcu, my * mcu, hs, block);
        e.block(block, 2);
      }
    e.flush();
    w.marker(0xD9);
    return out;
  }

private:
  /** Appends markers and length-prefixed segments to the output bytes. */
  struct Writer {
    std::vector<uint8_t>& out;
    explicit Writer(std::vector<uint8_t>& o) : out(o) {}
    void marker(uint8_t m) {
      out.push_back(0xFF);
      out.push_back(m);
    }
    void segment(uint8_t m, const uint8_t* body, size_t n) {
      marker(m);
      out.push_back((uint8_t) ((n + 2) >> 8));
      out.push_back((uint8_t) (n + 2));
      out.insert(out.end(), body, body + n);
    }
  };

  /** A Huffman table as a DHT segment carries it: how many codes of each
   *  length 1 to 16, then the symbols in code order. */
  struct StandardTable {
    uint8_t counts[16];
    uint8_t values[162];
    int count;
  };

  /** Annex K.3: DC luma, AC luma, DC chroma, AC chroma. */
  static const StandardTable& standardTable(int which) {
    static const StandardTable kTables[4] = {
        {{0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0}, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}, 12},
        {{0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d},
         {0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71,
          0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72,
          0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37,
          0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
          0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83,
          0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3,
          0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
          0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
          0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa},
         162},
        {{0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0}, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}, 12},
        {{0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77},
         {0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22,
          0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1,
          0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36,
          0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
          0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a,
          0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a,
          0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba,
          0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
          0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa},
         162}};
    return kTables[which];
  }

  /** An 8x8 block of a plane, each sample the average of a `step` by `step`
   *  square of it -- 1 for luma, 2 for 4:2:0 chroma -- and the edge pixels
   *  repeated past the image, so a partial MCU does not ring against black. */
  static void gather(const std::vector<float>& plane, int W, int H, int x0, int y0, int step, float* out) {
    for (int by = 0; by < 8; ++by)
      for (int bx = 0; bx < 8; ++bx) {
        float sum = 0.0f;
        for (int sy = 0; sy < step; ++sy)
          for (int sx = 0; sx < step; ++sx) {
            int x = x0 + bx * step + sx, y = y0 + by * step + sy;
            x = x >= W ? W - 1 : x;
            y = y >= H ? H - 1 : y;
            sum += plane[(size_t) y * (size_t) W + (size_t) x];
          }
        out[by * 8 + bx] = sum / (float) (step * step) - 128.0f;
      }
  }

  /** The entropy coder for one scan: each 8x8 block transformed, quantised
   *  by `q` and Huffman coded against the standard tables, with each
   *  component's DC predictor and the bits not yet written out. */
  struct Encoder {
    Writer& w;
    const uint8_t (*q)[64];
    uint32_t bitBuffer = 0;
    int bitCount = 0;
    int dc[3] = {0, 0, 0};
    uint16_t code[4][256];
    uint8_t length[4][256];

    Encoder(Writer& writer, const uint8_t (*quant)[64]) : w(writer), q(quant) {
      for (int t = 0; t < 4; ++t) {
        const StandardTable& s = standardTable(t);
        std::memset(length[t], 0, sizeof(length[t]));
        int c = 0, k = 0;
        for (int len = 1; len <= 16; ++len) {
          for (int i = 0; i < s.counts[len - 1]; ++i, ++k) {
            code[t][s.values[k]] = (uint16_t) c++;
            length[t][s.values[k]] = (uint8_t) len;
          }
          c <<= 1;
        }
      }
    }

    void put(uint32_t value, int bits) {
      bitBuffer = (bitBuffer << bits) | (value & ((1u << bits) - 1u));
      bitCount += bits;
      while (bitCount >= 8) {
        const uint8_t byte = (uint8_t) (bitBuffer >> (bitCount - 8));
        w.out.push_back(byte);
        if (byte == 0xFF) w.out.push_back(0x00); // stuffed, or it reads as a marker
        bitCount -= 8;
      }
      bitBuffer &= (1u << bitCount) - 1u;
    }

    void flush() {
      if (bitCount > 0) put(0x7F, 7 - ((bitCount + 7) % 8)); // padded with ones, per F.1.2.3
    }

    static int category(int v) {
      int a = v < 0 ? -v : v, s = 0;
      while (a) {
        ++s;
        a >>= 1;
      }
      return s;
    }

    /** The forward DCT, quantized and Huffman coded. Separable and in
     *  floating point: an encoder's DCT need only be accurate, and the
     *  decoder's side is where bit-exactness with libjpeg is wanted. */
    void block(const float* in, int component) {
      static const struct Basis {
        float c[8][8];
        Basis() {
          for (int u = 0; u < 8; ++u)
            for (int x = 0; x < 8; ++x)
              c[u][x] = (u == 0 ? 0.35355339f : 0.5f) * std::cos((2.0f * (float) x + 1.0f) * (float) u * 3.14159265f / 16.0f);
        }
      } basis;
      float tmp[64], dct[64];
      for (int y = 0; y < 8; ++y)
        for (int u = 0; u < 8; ++u) {
          float s = 0.0f;
          for (int x = 0; x < 8; ++x) s += basis.c[u][x] * in[y * 8 + x];
          tmp[y * 8 + u] = s;
        }
      for (int u = 0; u < 8; ++u)
        for (int v = 0; v < 8; ++v) {
          float s = 0.0f;
          for (int y = 0; y < 8; ++y) s += basis.c[v][y] * tmp[y * 8 + u];
          dct[v * 8 + u] = s;
        }
      const int table = component == 0 ? 0 : 1;
      int zz[64];
      for (int i = 0; i < 64; ++i) {
        const int natural = detail::jpeg::kNatural[i];
        const float v = dct[natural] / (float) q[table][natural];
        zz[i] = (int) (v < 0.0f ? v - 0.5f : v + 0.5f);
      }
      const int dcTable = table * 2, acTable = table * 2 + 1;
      const int diff = zz[0] - dc[component];
      dc[component] = zz[0];
      int s = category(diff);
      put(code[dcTable][s], length[dcTable][s]);
      if (s) put((uint32_t) (diff < 0 ? diff - 1 : diff), s);
      int run = 0;
      for (int i = 1; i < 64; ++i) {
        if (zz[i] == 0) {
          ++run;
          continue;
        }
        while (run > 15) {
          put(code[acTable][0xF0], length[acTable][0xF0]);
          run -= 16;
        }
        s = category(zz[i]);
        const int symbol = (run << 4) | s;
        put(code[acTable][symbol], length[acTable][symbol]);
        put((uint32_t) (zz[i] < 0 ? zz[i] - 1 : zz[i]), s);
        run = 0;
      }
      if (run > 0) put(code[acTable][0x00], length[acTable][0x00]);
    }
  };
};

} // namespace gfx
} // namespace sonore
