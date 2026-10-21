// SPDX-License-Identifier: Apache-2.0
//
// ImageFileFormat: whatever image a plugin is handed, decoded by what it is
// rather than by what its name says.
//
// ── Why sniff ───────────────────────────────────────────────────────────────
//
// A file's extension is a claim somebody typed. Cover art saved as "art.png"
// by a tool that wrote a JPEG, a ".jpg" that is a PNG because a web server
// converted it, a preset's picture with no name at all because it came out of
// a state blob -- each decodes fine by its first bytes and fails by its name.
// Every format here starts with a signature that says what it is: PNG's eight
// bytes, JPEG's FF D8 FF, GIF's "GIF87a" or "GIF89a". So the bytes decide.
//
// The formats it recognises and does NOT decode -- WebP, BMP, TIFF, HEIC,
// AVIF, ICO -- are named in the error rather than called "not an image". A
// user told "WebP is not supported" converts the file; one told "unknown
// format" assumes it is broken.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../path_io.h"
#include "bitmap.h"
#include "gif.h"
#include "jpeg.h"
#include "png.h"

namespace sonore {
namespace gfx {

/** What a file's first bytes say it is. */
enum class ImageFormat {
  Unknown, ///< None of the three: not decoded, and the error says what it looks like.
  Png,     ///< The eight-byte PNG signature.
  Jpeg,    ///< FF D8 FF: a start of image and the first segment's marker.
  Gif      ///< GIF87a or GIF89a.
};

/** An image of any of the formats ImageFileFormat reads. */
struct DecodedImage {
  /** The picture, premultiplied RGBA, or a GIF's first frame; a JPEG already
   *  turned upright when Options::applyOrientation is on. */
  Bitmap bitmap; // the picture, or a GIF's first frame
  /** What the signature said, set even when the decode then failed. */
  ImageFormat format = ImageFormat::Unknown;
  /** Why there is no picture, as a sentence in English; empty on success.
   *  For a format not decoded here (WebP, BMP, TIFF, HEIC, AVIF, ICO, an
   *  SVG) it names the format and what to convert it to. */
  std::string error;
  /** A JPEG or GIF that is usable but damaged, as JpegImage::warning and
   *  GifImage::warning say; empty otherwise. */
  std::string warning;
  /** A GIF's frames, when ImageFileFormat::Options::allFrames asked for
   *  them; empty otherwise and for every other format. */
  std::vector<GifFrame> frames;
  /** A GIF's NETSCAPE2.0 loop count, 0 for forever (GifImage::loopCount);
   *  1 for everything else. */
  int loopCount = 1;
  /** A JPEG's EXIF orientation, 1 to 8; 1 for everything else. */
  int orientation = 1;
  /** True when there is a picture: `error` is empty, `warning` or not. */
  bool ok() const { return error.empty(); }
};

/**
 * Decodes a PNG, JPEG or GIF from bytes or a file, choosing the decoder by the
 * bytes' signature rather than the name -- see the file comment.
 *
 * Plays the part of the reference's ImageFileFormat, as static functions over
 * PngDecoder, JpegDecoder and GifDecoder: nothing to construct, no state, safe
 * from any thread but not the audio thread (it allocates, and loadFile reads
 * the disk). A failure comes back as a DecodedImage with `error` set.
 */
class ImageFileFormat {
public:
  /** The largest file readFile reads: 256 MB. */
  static constexpr long kMaxImageFileBytes = 256L * 1024 * 1024;

  /** What decode and loadFile do beyond the first picture: a GIF's other
   *  frames, and whether a JPEG is turned upright. */
  struct Options {
    /** A GIF's every frame into DecodedImage::frames. Off by default: most
     *  callers want a picture, and a long animation is a lot of memory. */
    bool allFrames = false;
    /** A JPEG turned upright by its EXIF orientation. */
    bool applyOrientation = true;
  };

  /** What the bytes are, by their signature alone. */
  static ImageFormat sniff(const uint8_t* data, size_t size) {
    if (data == nullptr) return ImageFormat::Unknown;
    static const uint8_t kPng[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (size >= 8 && std::memcmp(data, kPng, 8) == 0) return ImageFormat::Png;
    // FF D8 is the start of image; the FF after it is the first segment's
    // marker, which every encoder writes. Two bytes alone match too much.
    if (size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) return ImageFormat::Jpeg;
    if (size >= 6 && std::memcmp(data, "GIF8", 4) == 0 && (data[4] == '7' || data[4] == '9') && data[5] == 'a')
      return ImageFormat::Gif;
    return ImageFormat::Unknown;
  }

  /** The same, for bytes in a vector. */
  static ImageFormat sniff(const std::vector<uint8_t>& bytes) { return sniff(bytes.data(), bytes.size()); }

  /** "PNG", "JPEG", "GIF" or "unknown": a static string, for a message. */
  static const char* nameOf(ImageFormat format) {
    switch (format) {
      case ImageFormat::Png: return "PNG";
      case ImageFormat::Jpeg: return "JPEG";
      case ImageFormat::Gif: return "GIF";
      default: return "unknown";
    }
  }

  // Overloads, not `= Options()`: a default argument of a nested type is
  // read while the class is still incomplete, and gcc and clang refuse
  // Options' member initializers there. MSVC accepts it, which is how
  // this went unnoticed.
  /** Decodes `size` bytes at `data` by their signature, with the default
   *  Options: a GIF's first frame only, a JPEG turned upright. The bytes are
   *  only read during the call; null data is an empty file, which is an
   *  error. */
  static DecodedImage decode(const uint8_t* data, size_t size) { return decode(data, size, Options()); }
  /** The same, with `options`. */
  static DecodedImage decode(const uint8_t* data, size_t size, const Options& options) {
    DecodedImage out;
    if (data == nullptr) size = 0;
    out.format = sniff(data, size);
    switch (out.format) {
      case ImageFormat::Png: {
        PngImage png = PngDecoder::decode(data, size);
        out.bitmap = std::move(png.bitmap);
        out.error = std::move(png.error);
        break;
      }
      case ImageFormat::Jpeg: {
        JpegDecoder::Options o;
        o.applyOrientation = options.applyOrientation;
        JpegImage jpeg = JpegDecoder::decode(data, size, o);
        out.bitmap = std::move(jpeg.bitmap);
        out.error = std::move(jpeg.error);
        out.warning = std::move(jpeg.warning);
        out.orientation = jpeg.orientation;
        break;
      }
      case ImageFormat::Gif: {
        GifDecoder::Options o;
        o.allFrames = options.allFrames;
        GifImage gif = GifDecoder::decode(data, size, o);
        out.bitmap = std::move(gif.bitmap);
        out.error = std::move(gif.error);
        out.warning = std::move(gif.warning);
        out.loopCount = gif.loopCount;
        if (options.allFrames) out.frames = std::move(gif.frames);
        break;
      }
      default: out.error = unknownFormat(data, size); break;
    }
    return out;
  }

  /** Decodes the whole of `bytes` -- a file read into memory. */
  static DecodedImage decode(const std::vector<uint8_t>& bytes) { return decode(bytes, Options()); }
  /** The same, with `options`. */
  static DecodedImage decode(const std::vector<uint8_t>& bytes, const Options& options) {
    return decode(bytes.data(), bytes.size(), options);
  }

  /** A file, by its UTF-8 path, whatever its extension says. */
  static DecodedImage loadFile(const std::string& path) { return loadFile(path, Options()); }
  /** The same, with `options`. A file that cannot be opened or read, or is
   *  over kMaxImageFileBytes, comes back with `error` set and `format`
   *  Unknown. */
  static DecodedImage loadFile(const std::string& path, const Options& options) {
    std::vector<uint8_t> bytes;
    if (!readFile(path, bytes)) {
      DecodedImage out;
      out.error = "the image file could not be read";
      return out;
    }
    return decode(bytes, options);
  }

  /** A whole file into `out`, by UTF-8 path. False if it cannot be read. */
  static bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    out.clear();
    std::FILE* f = ::sonore::pathio::open(path.c_str(), "rb");
    if (!f) return false;
    bool ok = std::fseek(f, 0, SEEK_END) == 0;
    const long size = ok ? std::ftell(f) : -1;
    // No image this SDK draws is a quarter of a gigabyte, and a path that is a
    // directory can open and report a size of anything on some filesystems:
    // refused rather than handed to a resize that throws.
    ok = ok && size >= 0 && size <= kMaxImageFileBytes && std::fseek(f, 0, SEEK_SET) == 0;
    if (ok && size > 0) {
      out.resize((size_t) size);
      ok = std::fread(out.data(), 1, out.size(), f) == out.size();
    }
    std::fclose(f);
    if (!ok) out.clear();
    return ok;
  }

private:
  /** The formats that are not decoded here, by name, so the error says what
   *  to convert from. */
  static std::string unknownFormat(const uint8_t* d, size_t n) {
    auto starts = [&](const char* s, size_t at = 0) {
      const size_t len = std::strlen(s);
      return n >= at + len && std::memcmp(d + at, s, len) == 0;
    };
    if (starts("RIFF") && starts("WEBP", 8)) return "WebP images are not supported; convert it to PNG or JPEG";
    if (starts("BM")) return "BMP images are not supported; convert it to PNG";
    if (n >= 4 && ((d[0] == 'I' && d[1] == 'I' && d[2] == 42 && d[3] == 0) ||
                   (d[0] == 'M' && d[1] == 'M' && d[2] == 0 && d[3] == 42)))
      return "TIFF images are not supported; convert it to PNG or JPEG";
    if (starts("ftypheic", 4) || starts("ftypheix", 4) || starts("ftypmif1", 4))
      return "HEIC images are not supported; convert it to JPEG";
    if (starts("ftypavif", 4)) return "AVIF images are not supported; convert it to PNG or JPEG";
    if (n >= 4 && d[0] == 0 && d[1] == 0 && d[2] == 1 && d[3] == 0) return "ICO images are not supported; convert it to PNG";
    if (starts("<svg") || starts("<?xml")) return "an SVG is drawn with gfx::Drawable, not decoded as an image";
    return "not an image this can read (PNG, JPEG or GIF)";
  }
};

} // namespace gfx
} // namespace sonore
