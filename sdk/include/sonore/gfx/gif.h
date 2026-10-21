// SPDX-License-Identifier: Apache-2.0
//
// GIF, every frame of it, into premultiplied RGBA Bitmaps.
//
// ── Why a plugin needs it ───────────────────────────────────────────────────
//
// Not for its 256 colours. For the one thing the other formats here do not
// do: move. A spinner while a sample library loads, a character that blinks,
// the animated logo a brand's designer already made -- every one of them
// arrives as a GIF, because GIF is what every tool exports when asked for
// "an animation" and every browser plays.
//
// ── What a frame is ─────────────────────────────────────────────────────────
//
// A GIF frame is not a picture. It is a rectangle of palette indices painted
// onto a canvas that keeps whatever the previous frames left, some indices
// transparent so the canvas shows through, and an instruction for what to do
// to the rectangle once the frame's delay is over: leave it, clear it, or put
// back what was there before. A decoder that hands those rectangles to a
// plugin hands it the hard part.
//
// So this composites. Each GifFrame holds the WHOLE canvas as it looks while
// that frame is shown -- what a player blits, once per delay -- and also the
// rectangle, the disposal and the delay as the file gave them, for a player
// that wants them. Disposal follows the browsers rather than the letter of
// the 1989 specification: "restore to background" clears to transparent,
// because that is what every browser does and so what every GIF is authored
// against, and the background colour index is ignored for the same reason.
//
// ── What is read ────────────────────────────────────────────────────────────
//
//   - GIF87a and GIF89a; global and local colour tables of 2 to 256 entries,
//     and none at all (indices are then greys, as Pillow reads them);
//   - LZW at every minimum code size, codes growing to twelve bits, a clear
//     code anywhere, and a full table that keeps going without one (the
//     "deferred clear" encoders use to compress better);
//   - interlaced frames, in the four passes;
//   - the graphic control extension: delay, transparency, disposal;
//   - NETSCAPE2.0's loop count.
//
// The delay is reported as the file gives it. Browsers draw any delay under
// 20 ms as 100 ms -- a GIF saying 0 means "as fast as you like", and old
// ones relied on that being slow -- and GifFrame::displayMs() does the same
// for a player that wants to look the way a browser does.
//
// ── Every count in the file is attacker-controlled ──────────────────────────
//
// LZW can describe four thousand pixels in a dozen bits, so a small file can
// honestly be a very large canvas, and an animation multiplies that by its
// frame count. Both are capped -- the canvas as PNG caps its image, and the
// sum over every frame -- and an animation that reaches the second cap stops
// there, keeps the frames it has, and says so in `warning`.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "bitmap.h"

namespace sonore {
namespace gfx {

/** What happens to a frame's rectangle once its delay is over. */
enum class GifDisposal : uint8_t {
  Unspecified = 0, ///< None given, or a value past 3: treated as Keep, as every player does.
  Keep = 1,        ///< Leave it; the next frame paints over it.
  Background = 2,  ///< Clear it to transparent (not to the background colour).
  Previous = 3     ///< Put back what was under it before this frame.
};

/** One frame of a GIF, and the canvas as it looks while it shows. */
struct GifFrame {
  /** The whole canvas, composited: this frame over what the earlier ones
   *  and their disposals left. Premultiplied, like every Bitmap. */
  Bitmap image;
  /** The delay the file gives, in milliseconds -- hundredths of a second in
   *  the file, so always a multiple of ten. 0 when it gives none. */
  int delayMs = 0;
  /** The frame's own rectangle on the canvas, as the file gave it, in
   *  canvas pixels: this is its left edge. It may reach past the canvas;
   *  what lies outside is not drawn into `image`. */
  int left = 0,
      top = 0,    ///< The rectangle's top edge, in canvas pixels.
      width = 0,  ///< The rectangle's width in pixels; 0 for a frame that only carries a delay.
      height = 0; ///< The rectangle's height in pixels.
  /** What the file says to do with the rectangle after this frame; already
   *  applied to the next frame's `image`. */
  GifDisposal disposal = GifDisposal::Unspecified;
  /** The rows were stored in four passes. Informational: `image` is already
   *  in row order. */
  bool interlaced = false;
  /** The palette index that leaves the canvas showing through, 0 to 255. */
  int transparentIndex = -1; // -1: no transparent colour

  /** The delay a browser would show: anything under 20 ms is 100 ms. */
  int displayMs() const { return delayMs < 20 ? 100 : delayMs; }
};

/** A decoded GIF, or a reason it is not one. */
struct GifImage {
  /** The first frame -- what a still image of a GIF means. */
  Bitmap bitmap;
  /** Every frame decoded, in file order; just the first with
   *  Options::allFrames off. Empty only when `error` is set. */
  std::vector<GifFrame> frames;
  int width = 0,  ///< The logical screen's width in pixels: every frame's canvas.
      height = 0; ///< The logical screen's height in pixels.
  /** NETSCAPE2.0's loop count: 0 is forever. A GIF without the extension
   *  plays once, and says 1. */
  int loopCount = 1;
  /** Why there is no image, in English; empty on success. */
  std::string error;
  /** Set when the file went wrong after at least one frame: the frames
   *  before the damage are kept, and a frame whose data ran short leaves
   *  its missing pixels unpainted. Empty for a clean file. */
  std::string warning;
  /** True when there is at least one frame: `error` is empty. A GIF with a
   *  `warning` is ok. */
  bool ok() const { return error.empty(); }

  /** One pass through the animation, in browser timing. */
  int durationMs() const {
    int total = 0;
    for (const GifFrame& f : frames) total += f.displayMs();
    return total;
  }
};

namespace detail {
namespace gif {

/** The image data's sub-blocks -- a length byte, that many bytes, until a
 *  length of zero -- read as one stream of bits, least significant first. */
struct BlockBits {
  const uint8_t* data = nullptr;
  size_t size = 0;
  size_t pos = 0;     // the next byte in the file
  size_t blockLeft = 0;
  bool ended = false; // the terminating empty block, or the end of the file
  uint32_t acc = 0;
  int count = 0;

  bool nextByte(uint8_t& out) {
    while (blockLeft == 0) {
      if (ended || pos >= size) {
        ended = true;
        return false;
      }
      blockLeft = data[pos++];
      if (blockLeft == 0) {
        ended = true;
        return false;
      }
    }
    if (pos >= size) {
      ended = true;
      return false;
    }
    out = data[pos++];
    --blockLeft;
    return true;
  }

  /** A code of `bits` bits, or -1 when the data has run out. */
  int code(int bits) {
    while (count < bits) {
      uint8_t b = 0;
      if (!nextByte(b)) return -1;
      acc |= (uint32_t) b << count;
      count += 8;
    }
    const int v = (int) (acc & ((1u << bits) - 1u));
    acc >>= bits;
    count -= bits;
    return v;
  }

  /** Past the rest of the sub-blocks, to whatever follows the image. */
  void skipRest() {
    while (!ended) {
      if (blockLeft > 0) {
        const size_t step = blockLeft < size - pos ? blockLeft : size - pos;
        pos += step;
        blockLeft -= step;
        if (pos >= size) ended = true;
        continue;
      }
      uint8_t ignored = 0;
      if (!nextByte(ignored)) break;
    }
  }
};

/**
 * LZW, as GIF uses it: codes start one bit wider than the minimum code size
 * and grow as the table fills, to twelve bits and no further. Returns how
 * many indices it wrote; `damaged` says a code was impossible.
 */
inline size_t decodeLzw(BlockBits& in, int minimumCodeSize, uint8_t* out, size_t wanted, bool& damaged) {
  damaged = false;
  const int clear = 1 << minimumCodeSize;
  const int end = clear + 1;
  uint16_t prefix[4096];
  uint8_t suffix[4096];
  uint8_t first[4096]; // the first index of each code's string
  uint16_t length[4096];
  for (int i = 0; i < clear; ++i) {
    prefix[i] = 0xFFFF;
    suffix[i] = (uint8_t) i;
    first[i] = (uint8_t) i;
    length[i] = 1;
  }
  int next = clear + 2;
  int size = minimumCodeSize + 1;
  int previous = -1;
  size_t written = 0;
  uint8_t stack[4096];

  while (written < wanted) {
    const int c = in.code(size);
    if (c < 0) break; // the data ran out: what was written stands
    if (c == clear) {
      next = clear + 2;
      size = minimumCodeSize + 1;
      previous = -1;
      continue;
    }
    if (c == end) break;
    int code = c;
    uint8_t head;
    if (previous < 0) {
      if (code >= clear) {
        damaged = true; // the first code after a clear must be a plain index
        break;
      }
      out[written++] = (uint8_t) code;
      previous = code;
      continue;
    }
    if (code < next) {
      head = first[code];
    } else if (code == next && next < 4096) {
      // The one code the decoder has not yet built: the previous string and
      // its own first index, which is the case that makes LZW worth a test.
      head = first[previous];
    } else {
      damaged = true;
      break;
    }
    // A table full at 4096 adds nothing: the encoder has stopped adding too,
    // and keeps emitting twelve-bit codes until it chooses to clear.
    if (next < 4096) {
      prefix[next] = (uint16_t) previous;
      suffix[next] = head;
      first[next] = first[previous];
      length[next] = (uint16_t) (length[previous] + 1);
      ++next;
      if (next == (1 << size) && size < 12) ++size;
    }
    // The string for `code`, written backwards through its prefixes.
    int depth = 0;
    for (int k = code; k != 0xFFFF && depth < 4096; k = prefix[k]) stack[depth++] = suffix[k];
    while (depth > 0 && written < wanted) out[written++] = stack[--depth];
    previous = code;
  }
  return written;
}

} // namespace gif
} // namespace detail

/**
 * Decodes a GIF -- every frame, composited onto the canvas -- from bytes in
 * memory. See the file comment for what a frame holds and what is read.
 *
 * Stateless: decode() is a static function of its input, safe from any thread
 * but not on the audio thread (it allocates every canvas). A file that is not
 * a GIF comes back with `error` set, and one that goes wrong partway keeps
 * the frames before the damage and says why in `warning`. The caps in
 * Options are there because the file is untrusted.
 */
class GifDecoder {
public:
  /** The canvas, capped as PngDecoder caps an image. */
  static constexpr uint64_t kMaxPixels = 64ull * 1024ull * 1024ull;
  /** Every frame's canvas, summed: 256 MB of RGBA. A spinner of sixty
   *  128x128 frames is a hundredth of it. */
  static constexpr uint64_t kMaxTotalPixels = 64ull * 1024ull * 1024ull;

  /** What to decode, and the most it may allocate: a canvas past `maxPixels`
   *  is refused, and an animation stops once its frames' canvases add up to
   *  `maxTotalPixels`. */
  struct Options {
    /** Off: the first frame only, which is all a still image needs. */
    bool allFrames = true;
    /** The most pixels the canvas, or one frame's own rectangle, may have.
     *  Past it the file is refused (or, after the first frame, the frames so
     *  far are kept with a `warning`). */
    uint64_t maxPixels = kMaxPixels;
    /** The most pixels all frames together may take, each counted as the
     *  larger of the canvas and its own rectangle. The first frame is always
     *  kept; decoding stops at the frame that would pass it, with a
     *  `warning`. */
    uint64_t maxTotalPixels = kMaxTotalPixels;
  };

  // Overloads, not `= Options()`: a default argument of a nested type is
  // read while the class is still incomplete, and gcc and clang refuse
  // Options' member initializers there. MSVC accepts it, which is how
  // this went unnoticed.
  /** Decodes `size` bytes at `data` with the default Options: every frame.
   *  The bytes are only read during the call; null data is an empty file,
   *  which is an error. */
  static GifImage decode(const uint8_t* data, size_t size) { return decode(data, size, Options()); }
  /** The same, with `options`. */
  static GifImage decode(const uint8_t* data, size_t size, const Options& options) {
    GifImage result;
    if (data == nullptr) size = 0;
    if (size < 13 || std::memcmp(data, "GIF", 3) != 0 ||
        !(std::memcmp(data + 3, "87a", 3) == 0 || std::memcmp(data + 3, "89a", 3) == 0)) {
      result.error = "not a GIF file";
      return result;
    }
    const int W = data[6] | (data[7] << 8);
    const int H = data[8] | (data[9] << 8);
    const uint8_t flags = data[10];
    if (W == 0 || H == 0) {
      result.error = "GIF has no pixels";
      return result;
    }
    if ((uint64_t) W * (uint64_t) H > options.maxPixels) {
      result.error = "GIF is larger than this decoder will allocate";
      return result;
    }
    result.width = W;
    result.height = H;
    size_t at = 13;
    std::vector<uint8_t> globalPalette;
    if (flags & 0x80) {
      const size_t entries = (size_t) 2 << (flags & 7);
      if (at + entries * 3 > size) {
        result.error = "truncated GIF: the colour table runs past the end of the file";
        return result;
      }
      globalPalette.assign(data + at, data + at + entries * 3);
      at += entries * 3;
    }

    // The canvas the frames are painted on, straight RGBA in a Bitmap's
    // layout -- with only fully opaque and fully clear pixels in a GIF,
    // premultiplied and straight are the same bytes but for the clear ones,
    // which are all zero either way.
    Bitmap canvas(W, H);
    std::vector<uint8_t> indices;
    std::vector<uint8_t> saved; // the rectangle under a frame that restores it
    uint64_t totalPixels = 0;

    // The graphic control extension, which applies to the next image only.
    int delay = 0, transparent = -1;
    GifDisposal disposal = GifDisposal::Unspecified;

    while (at < size) {
      const uint8_t introducer = data[at++];
      if (introducer == 0x3B) break; // the trailer
      if (introducer == 0x21) {
        if (at >= size) break;
        const uint8_t label = data[at++];
        if (label == 0xF9 && at + 6 <= size && data[at] >= 4) {
          const uint8_t packed = data[at + 1];
          const int method = (packed >> 2) & 7;
          disposal = method <= 3 ? (GifDisposal) method : GifDisposal::Unspecified;
          delay = (data[at + 2] | (data[at + 3] << 8)) * 10;
          transparent = (packed & 1) ? data[at + 4] : -1;
        } else if (label == 0xFF && at + 12 <= size && data[at] == 11 &&
                   (std::memcmp(data + at + 1, "NETSCAPE2.0", 11) == 0 ||
                    std::memcmp(data + at + 1, "ANIMEXTS1.0", 11) == 0)) {
          const size_t sub = at + 12;
          if (sub + 4 <= size && data[sub] >= 3 && data[sub + 1] == 1) result.loopCount = data[sub + 2] | (data[sub + 3] << 8);
        }
        // Every extension is sub-blocks, whatever its label: skipped whole.
        while (at < size) {
          const size_t n = data[at++];
          if (n == 0) break;
          at += n;
        }
        continue;
      }
      if (introducer != 0x2C) {
        // Not a block GIF has. What came before it stands, as in a browser.
        if (result.frames.empty()) result.error = "GIF has an unknown block before any image";
        else result.warning = "GIF has an unknown block; the frames before it are kept";
        break;
      }

      if (at + 9 > size) {
        result.warning = "GIF ends inside an image descriptor";
        break;
      }
      GifFrame frame;
      frame.left = data[at] | (data[at + 1] << 8);
      frame.top = data[at + 2] | (data[at + 3] << 8);
      frame.width = data[at + 4] | (data[at + 5] << 8);
      frame.height = data[at + 6] | (data[at + 7] << 8);
      const uint8_t imageFlags = data[at + 8];
      at += 9;
      frame.interlaced = (imageFlags & 0x40) != 0;
      frame.delayMs = delay;
      frame.disposal = disposal;
      frame.transparentIndex = transparent;
      delay = 0;
      transparent = -1;
      disposal = GifDisposal::Unspecified;

      std::vector<uint8_t> localPalette;
      if (imageFlags & 0x80) {
        const size_t entries = (size_t) 2 << (imageFlags & 7);
        if (at + entries * 3 > size) {
          result.warning = "GIF ends inside a colour table";
          break;
        }
        localPalette.assign(data + at, data + at + entries * 3);
        at += entries * 3;
      }
      const std::vector<uint8_t>& palette = (imageFlags & 0x80) ? localPalette : globalPalette;
      if (at >= size) {
        result.warning = "GIF ends before an image's data";
        break;
      }
      const int minimumCodeSize = data[at++];
      if (minimumCodeSize < 1 || minimumCodeSize > 11) {
        if (result.frames.empty()) result.error = "GIF image has an impossible LZW code size";
        else result.warning = "GIF image has an impossible LZW code size; the frames before it are kept";
        break;
      }

      // The frame's own rectangle is the file's to state, and nothing ties it
      // to the canvas: a 1x1 screen may carry a 65535x65535 frame, whose
      // indices are four gigabytes allocated and zeroed for a 30-byte file.
      // It is held to the canvas's cap, and counted against the animation's
      // budget by its rectangle where that is larger than the canvas.
      const uint64_t framePixels = (uint64_t) frame.width * (uint64_t) frame.height;
      if (framePixels > options.maxPixels) {
        if (result.frames.empty()) result.error = "GIF frame is larger than this decoder will allocate";
        else result.warning = "GIF frame is larger than this decoder will allocate; the frames before it are kept";
        break;
      }
      totalPixels += (std::max)((uint64_t) W * (uint64_t) H, framePixels);
      if (!result.frames.empty() && totalPixels > options.maxTotalPixels) {
        result.warning = "GIF animation is longer than this decoder will hold; the first frames are kept";
        break;
      }

      // The indices, in the order the rows were stored. A frame of zero
      // size has none and still takes its turn -- some encoders write one
      // to carry a delay.
      const size_t pixels = (size_t) frame.width * (size_t) frame.height;
      indices.assign(pixels, 0);
      detail::gif::BlockBits bits;
      bits.data = data;
      bits.size = size;
      bits.pos = at;
      bool damaged = false;
      const size_t got = pixels ? detail::gif::decodeLzw(bits, minimumCodeSize, indices.data(), pixels, damaged) : 0;
      bits.skipRest();
      at = bits.pos;
      if ((damaged || got < pixels) && result.warning.empty())
        result.warning = "GIF image data is short or damaged; the missing pixels are left clear";

      // Interlaced rows arrive in four passes: every eighth from 0, every
      // eighth from 4, every fourth from 2, every second from 1.
      std::vector<int> rowOf((size_t) frame.height);
      if (frame.interlaced) {
        static const int kStart[4] = {0, 4, 2, 1}, kStep[4] = {8, 8, 4, 2};
        int r = 0;
        for (int pass = 0; pass < 4; ++pass)
          for (int y = kStart[pass]; y < frame.height; y += kStep[pass]) rowOf[(size_t) r++] = y;
      } else {
        for (int y = 0; y < frame.height; ++y) rowOf[(size_t) y] = y;
      }

      // The part of the frame on the canvas. A frame may claim to reach past
      // it; what lies outside is not drawn, as a browser does not draw it.
      const int x0 = (std::max)(frame.left, 0), y0 = (std::max)(frame.top, 0);
      const int x1 = (std::min)(frame.left + frame.width, W), y1 = (std::min)(frame.top + frame.height, H);

      if (frame.disposal == GifDisposal::Previous && x1 > x0 && y1 > y0) {
        saved.assign((size_t) (x1 - x0) * (size_t) (y1 - y0) * 4u, 0);
        for (int y = y0; y < y1; ++y)
          std::memcpy(&saved[(size_t) (y - y0) * (size_t) (x1 - x0) * 4u], canvas.data() + canvas.offsetOf(x0, y),
                      (size_t) (x1 - x0) * 4u);
      }

      const size_t colours = palette.size() / 3;
      for (size_t i = 0; i < got; ++i) {
        const int fy = rowOf[i / (size_t) frame.width];
        const int fx = (int) (i % (size_t) frame.width);
        const int x = frame.left + fx, y = frame.top + fy;
        if (x < x0 || x >= x1 || y < y0 || y >= y1) continue;
        const int index = indices[i];
        if (index == frame.transparentIndex) continue;
        uint8_t* p = canvas.data() + canvas.offsetOf(x, y);
        if (palette.empty()) {
          p[0] = p[1] = p[2] = (uint8_t) index; // no table at all: greys
        } else if ((size_t) index < colours) {
          p[0] = palette[(size_t) index * 3];
          p[1] = palette[(size_t) index * 3 + 1];
          p[2] = palette[(size_t) index * 3 + 2];
        } else {
          p[0] = p[1] = p[2] = 0; // past the table: black, as Pillow draws it
        }
        p[3] = 255;
      }

      frame.image = canvas;
      if (result.frames.empty()) result.bitmap = canvas;

      // The disposal, before the next frame is painted.
      if (x1 > x0 && y1 > y0) {
        if (frame.disposal == GifDisposal::Background) {
          for (int y = y0; y < y1; ++y) std::memset(canvas.data() + canvas.offsetOf(x0, y), 0, (size_t) (x1 - x0) * 4u);
        } else if (frame.disposal == GifDisposal::Previous) {
          for (int y = y0; y < y1; ++y)
            std::memcpy(canvas.data() + canvas.offsetOf(x0, y), &saved[(size_t) (y - y0) * (size_t) (x1 - x0) * 4u],
                        (size_t) (x1 - x0) * 4u);
        }
      }
      result.frames.push_back(std::move(frame));
      if (!options.allFrames) break;
    }

    if (result.frames.empty() && result.error.empty()) result.error = "GIF has no image";
    return result;
  }

  /** Decodes the whole of `bytes` -- a file read into memory. */
  static GifImage decode(const std::vector<uint8_t>& bytes) { return decode(bytes, Options()); }
  /** The same, with `options`. */
  static GifImage decode(const std::vector<uint8_t>& bytes, const Options& options) {
    return decode(bytes.data(), bytes.size(), options);
  }
};

} // namespace gfx
} // namespace sonore
