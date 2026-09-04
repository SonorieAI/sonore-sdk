// SPDX-License-Identifier: Apache-2.0
//
// PNG, into the same premultiplied RGBA a Bitmap holds.
//
// ── Why a plugin needs it ───────────────────────────────────────────────────
//
// A logo. A texture behind a panel. A knob drawn by a designer rather than by
// arithmetic. All of it was free in a webview -- an <img> tag -- and impossible
// in the native UI, which could draw shapes and text and nothing that came out
// of an image editor.
//
// ── PNG only, and 8 bits only ───────────────────────────────────────────────
//
// Not JPEG: a plugin's artwork is flat colour and hard edges, which is what PNG
// is for and what JPEG is worst at, and a JPEG decoder is a DCT and a Huffman
// decoder and a colour transform for a format nobody should be using here.
//
// 16-bit-per-channel PNGs are read and reduced to 8, because a Bitmap is 8 and
// keeping the extra bits would mean carrying them nowhere. Interlaced (Adam7)
// PNGs are REFUSED by name rather than decoded wrong -- they are rare, they are
// a second pass over the whole image, and a decoder that silently produced a
// quarter-resolution picture would be worse than one that says no.
//
// ── Every length in the file is attacker-controlled ─────────────────────────
//
// A PNG arrives from wherever a plugin's assets came from, which for a
// generated plugin is a pipeline nobody audits. Width times height times four
// overflows a 32-bit size at a width and height that both fit in the header, so
// the size is computed in 64 bits and refused before anything is allocated.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "bitmap.h"
#include "inflate.h"

namespace sonore {
namespace gfx {

/**
 * A decoded image, or a reason it is not one.
 *
 * The reason is a string rather than an enum because it goes in front of a
 * person: "interlaced PNGs are not supported" tells somebody what to do, where
 * kErrorUnsupported tells them to go and read a header.
 */
struct PngImage {
  Bitmap bitmap;
  std::string error;
  bool ok() const { return error.empty(); }
};

class PngDecoder {
public:
  /** No image a plugin embeds is anywhere near this, and it stops a header
   *  claiming 60000x60000 from asking for fourteen gigabytes before anything
   *  has looked at whether the file even contains that much. */
  static constexpr uint64_t kMaxPixels = 64ull * 1024ull * 1024ull;

  static PngImage decode(const uint8_t* data, size_t size) {
    PngImage result;

    static const uint8_t kSignature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (size < 8 || std::memcmp(data, kSignature, 8) != 0) {
      result.error = "not a PNG file";
      return result;
    }

    uint32_t width = 0, height = 0;
    int depth = 0, colourType = 0, interlace = 0;
    bool haveHeader = false;
    std::vector<uint8_t> compressed;
    std::vector<uint8_t> palette;      // RGB triples
    std::vector<uint8_t> paletteAlpha; // one per entry, if a tRNS chunk arrived

    size_t at = 8;
    while (at + 8 <= size) {
      const uint32_t length = readBE32(data + at);
      const char* type = (const char*) (data + at + 4);
      // In 64 bits, because at + 12 + length in 32 would wrap and let a chunk
      // claim a length that reaches past the file while appearing not to.
      if ((uint64_t) at + 12ull + (uint64_t) length > (uint64_t) size) {
        result.error = "truncated PNG: a chunk claims more data than the file holds";
        return result;
      }
      const uint8_t* body = data + at + 8;

      if (std::memcmp(type, "IHDR", 4) == 0) {
        if (length < 13) {
          result.error = "malformed PNG header";
          return result;
        }
        width = readBE32(body);
        height = readBE32(body + 4);
        depth = body[8];
        colourType = body[9];
        interlace = body[12];
        haveHeader = true;

        if (width == 0 || height == 0) {
          result.error = "PNG has no pixels";
          return result;
        }
        if ((uint64_t) width * (uint64_t) height > kMaxPixels) {
          result.error = "PNG is larger than this decoder will allocate";
          return result;
        }
        if (interlace != 0) {
          // Named rather than decoded wrong. A silently quarter-resolution
          // picture is worse than a refusal somebody can act on.
          result.error = "interlaced (Adam7) PNGs are not supported";
          return result;
        }
        if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16) {
          result.error = "unsupported PNG bit depth";
          return result;
        }
        if (colourType != 0 && colourType != 2 && colourType != 3 && colourType != 4 &&
            colourType != 6) {
          result.error = "unsupported PNG colour type";
          return result;
        }
      } else if (std::memcmp(type, "PLTE", 4) == 0) {
        palette.assign(body, body + length);
      } else if (std::memcmp(type, "tRNS", 4) == 0) {
        paletteAlpha.assign(body, body + length);
      } else if (std::memcmp(type, "IDAT", 4) == 0) {
        // Appended, because a PNG may split its data across any number of IDAT
        // chunks and several encoders do -- a decoder that took only the first
        // works on its own test images and on nobody else's.
        compressed.insert(compressed.end(), body, body + length);
      } else if (std::memcmp(type, "IEND", 4) == 0) {
        break;
      }

      at += 12ull + (uint64_t) length; // 4 length + 4 type + body + 4 CRC
    }

    if (!haveHeader) {
      result.error = "PNG has no header chunk";
      return result;
    }
    if (compressed.empty()) {
      result.error = "PNG has no image data";
      return result;
    }

    const int channels = channelsFor(colourType);
    const uint64_t bitsPerPixel = (uint64_t) channels * (uint64_t) depth;
    const uint64_t bytesPerRow = ((uint64_t) width * bitsPerPixel + 7ull) / 8ull;
    // One filter byte per row, which is the +1.
    const uint64_t expected = (bytesPerRow + 1ull) * (uint64_t) height;

    std::vector<uint8_t> raw;
    if (!Inflater::inflateZlib(compressed.data(), compressed.size(), raw, (size_t) expected)) {
      result.error = "PNG image data could not be decompressed";
      return result;
    }
    if (raw.size() < expected) {
      result.error = "PNG image data is shorter than its header describes";
      return result;
    }

    if (!unfilter(raw, width, height, (size_t) bytesPerRow, (int) ((bitsPerPixel + 7) / 8))) {
      result.error = "PNG uses an unknown row filter";
      return result;
    }

    result.bitmap.resize((int) width, (int) height);
    if (!expand(raw, width, height, (size_t) bytesPerRow, depth, colourType, palette, paletteAlpha,
                result.bitmap)) {
      result.error = "PNG palette index is out of range";
      return result;
    }
    return result;
  }

  static PngImage decode(const std::vector<uint8_t>& bytes) {
    return decode(bytes.data(), bytes.size());
  }

private:
  static uint32_t readBE32(const uint8_t* p) {
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) |
           (uint32_t) p[3];
  }

  static int channelsFor(int colourType) {
    switch (colourType) {
      case 0: return 1; // grey
      case 2: return 3; // RGB
      case 3: return 1; // palette index
      case 4: return 2; // grey + alpha
      default: return 4; // RGBA
    }
  }

  /**
   * Undo the per-row filters, in place.
   *
   * Each row is predicted from the one above and the pixel to the left, and the
   * file stores the difference. It has to be undone top to bottom because every
   * row depends on the reconstructed row above -- not on the stored one.
   */
  static bool unfilter(std::vector<uint8_t>& raw, uint32_t width, uint32_t height,
                       size_t bytesPerRow, int bytesPerPixel) {
    (void) width;
    if (bytesPerPixel < 1) bytesPerPixel = 1;
    for (uint32_t y = 0; y < height; ++y) {
      const size_t rowStart = (size_t) y * (bytesPerRow + 1);
      const uint8_t filter = raw[rowStart];
      uint8_t* row = raw.data() + rowStart + 1;
      const uint8_t* above = y > 0 ? raw.data() + (size_t) (y - 1) * (bytesPerRow + 1) + 1
                                   : nullptr;

      for (size_t i = 0; i < bytesPerRow; ++i) {
        const int a = i >= (size_t) bytesPerPixel ? row[i - (size_t) bytesPerPixel] : 0;
        const int b = above ? above[i] : 0;
        const int c = (above && i >= (size_t) bytesPerPixel)
                          ? above[i - (size_t) bytesPerPixel]
                          : 0;
        switch (filter) {
          case 0: break;
          case 1: row[i] = (uint8_t) (row[i] + a); break;
          case 2: row[i] = (uint8_t) (row[i] + b); break;
          case 3: row[i] = (uint8_t) (row[i] + (a + b) / 2); break;
          case 4: row[i] = (uint8_t) (row[i] + paeth(a, b, c)); break;
          default: return false;
        }
      }
    }
    return true;
  }

  /** The Paeth predictor: whichever of left, above and above-left is closest to
   *  their linear estimate. Ties go to `a`, then `b` -- the order matters and
   *  getting it wrong produces an image that is right except along edges. */
  static int paeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = p > a ? p - a : a - p;
    const int pb = p > b ? p - b : b - p;
    const int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
  }

  /** Rows of whatever the file holds, into premultiplied RGBA. */
  static bool expand(const std::vector<uint8_t>& raw, uint32_t width, uint32_t height,
                     size_t bytesPerRow, int depth, int colourType,
                     const std::vector<uint8_t>& palette,
                     const std::vector<uint8_t>& paletteAlpha, Bitmap& out) {
    const int channels = channelsFor(colourType);

    for (uint32_t y = 0; y < height; ++y) {
      const uint8_t* row = raw.data() + (size_t) y * (bytesPerRow + 1) + 1;
      for (uint32_t x = 0; x < width; ++x) {
        uint8_t r = 0, g = 0, b = 0, a = 255;

        if (depth == 16) {
          // The low byte is dropped rather than rounded. A Bitmap is 8 bits, so
          // the extra precision has nowhere to go; rounding would cost a
          // multiply per channel for a difference of at most one level.
          const uint8_t* p = row + ((size_t) x * (size_t) channels) * 2;
          if (colourType == 0) {
            r = g = b = p[0];
          } else if (colourType == 2) {
            r = p[0];
            g = p[2];
            b = p[4];
          } else if (colourType == 4) {
            r = g = b = p[0];
            a = p[2];
          } else {
            r = p[0];
            g = p[2];
            b = p[4];
            a = p[6];
          }
        } else if (depth == 8) {
          const uint8_t* p = row + (size_t) x * (size_t) channels;
          if (colourType == 0) {
            r = g = b = p[0];
          } else if (colourType == 2) {
            r = p[0];
            g = p[1];
            b = p[2];
          } else if (colourType == 3) {
            const size_t index = (size_t) p[0];
            if (index * 3 + 2 >= palette.size()) return false;
            r = palette[index * 3];
            g = palette[index * 3 + 1];
            b = palette[index * 3 + 2];
            if (index < paletteAlpha.size()) a = paletteAlpha[index];
          } else if (colourType == 4) {
            r = g = b = p[0];
            a = p[1];
          } else {
            r = p[0];
            g = p[1];
            b = p[2];
            a = p[3];
          }
        } else {
          // 1, 2 and 4 bits: several pixels to a byte, most significant first.
          // Small logos and icons are palette-indexed at these depths all the
          // time, so this is not an exotic path.
          const uint32_t perByte = (uint32_t) (8 / depth);
          const uint8_t byte = row[x / perByte];
          const uint32_t shift = (uint32_t) (8 - depth) - (x % perByte) * (uint32_t) depth;
          const uint32_t value = (byte >> shift) & (uint32_t) ((1 << depth) - 1);
          if (colourType == 3) {
            const size_t index = (size_t) value;
            if (index * 3 + 2 >= palette.size()) return false;
            r = palette[index * 3];
            g = palette[index * 3 + 1];
            b = palette[index * 3 + 2];
            if (index < paletteAlpha.size()) a = paletteAlpha[index];
          } else {
            // Grey at 1, 2 or 4 bits is scaled to the full range, so a 1-bit
            // image is black and white rather than black and almost-black.
            const uint32_t maximum = (uint32_t) ((1 << depth) - 1);
            r = g = b = (uint8_t) (value * 255u / maximum);
          }
        }

        // Premultiplied on the way in, because that is what a Bitmap holds and
        // what the rasteriser composites. A decoder that stored straight alpha
        // would draw a semi-transparent logo too bright.
        out.setPixel((int) x, (int) y, PremulColour::from(Colour(r, g, b, a)));
      }
    }
    return true;
  }
};

} // namespace gfx
} // namespace sonore
