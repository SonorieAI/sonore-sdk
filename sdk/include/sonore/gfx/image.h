// SPDX-License-Identifier: Apache-2.0
//
// The things the reference builds around an image -- its storage type, its
// scale, and a cache of decoded ones -- in this SDK's terms: around a Bitmap.
//
// ── ImageType: one backing, named the way the reference names it ───────────
//
// The reference has an image type because an image may live on the GPU or in
// main memory and the code drawing it must not care. Here everything is
// rasterised in software into one premultiplied RGBA Bitmap, on purpose --
// identical pixels on every machine, no context to lose -- and what a window
// peer blits IS that bitmap. So NativeImageType and SoftwareImageType make the
// same thing and say so: they exist so code written against the reference's
// Image(format, w, h, clear, type) has a place to land, and so a cache can
// tell by type ID which kind somebody asked for. A Bitmap is always cleared --
// `clearImage` false is accepted and ignored, because handing out memory that
// held another plugin's pixels is not an optimisation worth having.
//
// ── ScaledImage: art and the density it was drawn for ───────────────────────
//
// A 2x knob strip drawn at 1x is twice the size it was meant to be; drawn at
// 2x on a 1x screen it is half the size and soft. The image alone cannot say
// which it is. ScaledImage carries the scale beside the pixels, draws at the
// LOGICAL size the art means, and picks, from a set of renditions, the one a
// display of a given density wants -- Backing::scale is that density.
//
// ── ImageCache: decoded once, shared, and let go ────────────────────────────
//
// An editor is opened and closed many times in a session, and several
// instances of a plugin show the same artwork. Decoding the same file per
// editor per instance is wasted startup and wasted memory. The cache hands
// out shared pointers keyed by a path, a hash of the bytes, or an id; an
// image is let go once nothing but the cache has held it or asked for it for
// the length of the timeout -- the reference's rule, so an editor closed and
// reopened within a few seconds finds its art still decoded. A byte budget
// bounds what the cache alone keeps: past it, unused images go oldest first.
// Images somebody still holds are never "evicted" -- dropping the cache's
// pointer would free nothing and only lose the sharing.
//
// There is no timer thread. Every call purges what has expired, and an
// editor's own timer may call purge() so a cache nobody asks still empties.
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>

#include "../path_io.h"
#include "bitmap.h"
#include "graphics.h"
#include "image_format.h"

namespace sonore {
namespace gfx {

// ── Image types ──────────────────────────────────────────────────────────────

/**
 * Where an image's pixels live, as the reference names it: the base of
 * NativeImageType and SoftwareImageType.
 *
 * Here every type makes the same premultiplied RGBA Bitmap -- see the file
 * comment for why the distinction is kept at all. A type is stateless and
 * may be used from any thread; getTypeID is what tells two apart.
 */
class ImageType {
public:
  virtual ~ImageType() = default;
  /** A new image of this type, width by height, fully transparent. */
  virtual Bitmap create(int width, int height, bool clearImage = true) const = 0;
  /** Which type: images of one ID share a backing. */
  virtual int getTypeID() const = 0;
  /** `source` in this type's backing. Every type here shares one, so this
   *  is a copy -- which is still what the reference's convert promises: an
   *  image the caller may change without changing the source. */
  virtual Bitmap convert(const Bitmap& source) const { return source; }
};

/** The backing a window peer blits from -- here the same premultiplied RGBA
 *  bitmap as everything else, because that is what the platforms are given. */
class NativeImageType : public ImageType {
public:
  Bitmap create(int width, int height, bool clearImage = true) const override {
    (void) clearImage;
    return Bitmap(width, height);
  }
  int getTypeID() const override { return 1; }
};

/** An image in main memory, drawn by the software rasteriser. */
class SoftwareImageType : public ImageType {
public:
  Bitmap create(int width, int height, bool clearImage = true) const override {
    (void) clearImage;
    return Bitmap(width, height);
  }
  int getTypeID() const override { return 2; }
};

// ── ScaledImage ──────────────────────────────────────────────────────────────

/**
 * An image and the display density it was drawn for: 1 for ordinary art, 2
 * for `@2x` art. Mirrors the reference's ScaledImage; the file comment says why the
 * scale has to travel with the pixels.
 *
 * Holds the bitmap by shared pointer to const, so copies are cheap and an
 * image from ImageCache can be wrapped without a copy. A scale of 0 or less
 * is taken as 1. Drawing goes through a Graphics, on whatever thread paints.
 */
class ScaledImage {
public:
  /** No pixels: not valid. */
  ScaledImage() = default;
  /** Share `image`, drawn for `scale` device pixels per logical pixel; 0 or less
   *  is 1. */
  ScaledImage(std::shared_ptr<const Bitmap> image, float scale = 1.0f)
      : image_(std::move(image)), scale_(scale > 0.0f ? scale : 1.0f) {}
  /** Take `image` over, moved into a shared pointer. */
  explicit ScaledImage(Bitmap image, float scale = 1.0f)
      : ScaledImage(std::make_shared<const Bitmap>(std::move(image)), scale) {}

  /** Whether there are pixels to draw. */
  bool isValid() const { return image_ && !image_->isEmpty(); }

  /** The pixels; an empty bitmap when there are none. */
  const Bitmap& getImage() const {
    static const Bitmap kNone;
    return image_ ? *image_ : kNone;
  }
  /** The shared pixels, or null. */
  const std::shared_ptr<const Bitmap>& imagePointer() const { return image_; }

  /** Device pixels per logical pixel the art was drawn for: 2 for @2x. */
  float getScale() const { return scale_; }

  /** The size the art means, in logical pixels: its pixels over its scale. */
  Rect getScaledBounds() const {
    if (!image_) return {};
    return {0.0f, 0.0f, (float) image_->width() / scale_, (float) image_->height() / scale_};
  }

  /** At its logical size, top-left at `topLeft` in the Graphics' user space.
   *  Under a scale transform equal to the art's scale this is a pixel for a
   *  pixel. */
  void drawAt(Graphics& g, Point topLeft) const {
    if (!isValid()) return;
    const Rect b = getScaledBounds();
    g.drawImage(*image_, Rect(topLeft.x, topLeft.y, b.w, b.h));
  }

  /** Scaled to fit `area`, aspect kept and centred. */
  void drawWithin(Graphics& g, const Rect& area) const {
    if (!isValid() || area.isEmpty()) return;
    const Rect b = getScaledBounds();
    g.drawImage(*image_, area.withAspect(b.w / b.h));
  }

  /**
   * Which of several renditions a display of `deviceScale` wants: the
   * smallest whose scale is at least the display's -- drawn down a little
   * rather than up -- or, when none is that dense, the densest there is.
   * -1 for an empty set.
   */
  static int pickFor(const std::vector<ScaledImage>& renditions, float deviceScale) {
    int best = -1, densest = -1;
    for (int i = 0; i < (int) renditions.size(); ++i) {
      const ScaledImage& r = renditions[(size_t) i];
      if (!r.isValid()) continue;
      if (densest < 0 || r.scale_ > renditions[(size_t) densest].scale_) densest = i;
      if (r.scale_ >= deviceScale && (best < 0 || r.scale_ < renditions[(size_t) best].scale_)) best = i;
    }
    return best >= 0 ? best : densest;
  }

private:
  std::shared_ptr<const Bitmap> image_;
  float scale_ = 1.0f;
};

// ── ImageCache ───────────────────────────────────────────────────────────────

/**
 * Decoded images, shared between every editor and instance that asks for the
 * same file, bytes or id. Mirrors the reference's ImageCache; when an image is
 * let go, and why the cache has no thread of its own, is in the file comment.
 *
 * What it hands out is a shared pointer to a const Bitmap: hold it for as
 * long as it is drawn, and the cache never frees it from under you. Every
 * call takes the cache's lock, so any thread may use it -- but the getters
 * read and decode files, so not the audio thread. shared() is the
 * process-wide one; a separate instance is for a test.
 */
class ImageCache {
public:
  /** What the cache hands out: hold it while it is drawn. */
  using Image = std::shared_ptr<const Bitmap>;

  /** An empty cache of its own -- for a test; plugins use shared(). */
  ImageCache() = default;
  /** Not copyable: it holds a lock and the images' bookkeeping. */
  ImageCache(const ImageCache&) = delete;
  ImageCache& operator=(const ImageCache&) = delete;

  /** The process-wide cache, which every editor of every instance shares. */
  static ImageCache& shared() {
    static ImageCache cache;
    return cache;
  }

  /**
   * A file's image, decoded once. The file's size and modification time are
   * kept with it: an image edited on disk while a plugin runs -- a designer
   * iterating on a skin -- is decoded again rather than served stale.
   * Null when the file cannot be read or decoded.
   */
  Image getFromFile(const std::string& path) {
    const uint64_t key = hashOfPath(path);
    uint64_t size = 0, modified = 0;
    statFile(path, size, modified);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      purgeLocked();
      auto it = entries_.find(key);
      if (it != entries_.end() && it->second.fileSize == size && it->second.fileModified == modified) {
        it->second.lastUsed = now();
        return it->second.image;
      }
    }
    // Decoded outside the lock: a large JPEG takes milliseconds, and another
    // editor asking for something already cached should not wait for it.
    DecodedImage decoded = ImageFileFormat::loadFile(path);
    if (!decoded.ok()) return nullptr;
    Image image = std::make_shared<const Bitmap>(std::move(decoded.bitmap));
    std::lock_guard<std::mutex> lock(mutex_);
    Entry& e = insertLocked(key, image);
    e.fileSize = size;
    e.fileModified = modified;
    return e.image;
  }

  /** An image from encoded bytes, keyed by a hash of the bytes -- the same
   *  artwork compiled into two plugins is decoded once. */
  Image getFromMemory(const void* data, size_t size) {
    if (data == nullptr || size == 0) return nullptr;
    const uint64_t key = hashOf(data, size);
    if (Image found = getFromHashCode(key)) return found;
    DecodedImage decoded = ImageFileFormat::decode((const uint8_t*) data, size);
    if (!decoded.ok()) return nullptr;
    Image image = std::make_shared<const Bitmap>(std::move(decoded.bitmap));
    std::lock_guard<std::mutex> lock(mutex_);
    return insertLocked(key, image).image;
  }

  /** What was cached under `hash`, or null. */
  Image getFromHashCode(uint64_t hash) {
    std::lock_guard<std::mutex> lock(mutex_);
    purgeLocked();
    auto it = entries_.find(hash);
    if (it == entries_.end()) return nullptr;
    it->second.lastUsed = now();
    return it->second.image;
  }

  /** An image made some other way -- drawn, filtered, generated -- kept
   *  under an id of the caller's choosing. Returns what the cache holds. */
  Image addImageToCache(Bitmap image, uint64_t hash) {
    return addImageToCache(std::make_shared<const Bitmap>(std::move(image)), hash);
  }
  /** The same, for an image already shared. Null is ignored and returns null;
   *  an image already under `hash` is replaced, its holders keeping theirs. */
  Image addImageToCache(Image image, uint64_t hash) {
    if (!image) return nullptr;
    std::lock_guard<std::mutex> lock(mutex_);
    return insertLocked(hash, std::move(image)).image;
  }

  /** How long an image nobody else holds is kept after it was last asked
   *  for. Five seconds by default, as the reference has it. */
  void setCacheTimeout(int milliseconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    timeoutMs_ = milliseconds < 0 ? 0 : milliseconds;
  }

  /** What the cache alone may keep, in bytes of pixels: 64 MB by default. */
  void setMaxBytes(size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    maxBytes_ = bytes;
    enforceBudgetLocked();
  }

  /** Every image nobody else holds, released now whatever its age. */
  void releaseUnusedImages() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (it->second.image.use_count() == 1) it = eraseLocked(it);
      else ++it;
    }
  }

  /** The timed pass: what has gone unused for longer than the timeout. */
  void purge() {
    std::lock_guard<std::mutex> lock(mutex_);
    purgeLocked();
  }

  /** How many images the cache holds now. */
  size_t numImages() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
  }

  /** Bytes of pixels the cache holds now, counting images others hold too. */
  size_t bytesHeld() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bytes_;
  }

  /** The clock, in milliseconds -- for a test that cannot wait five seconds. */
  void setClock(std::function<int64_t()> nowMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    clock_ = std::move(nowMs);
  }

  /** FNV-1a, 64 bits: what getFromMemory keys by. */
  static uint64_t hashOf(const void* data, size_t size) {
    uint64_t h = 0xcbf29ce484222325ull;
    const uint8_t* p = (const uint8_t*) data;
    for (size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
  }

  /** A path's key: its hash, kept apart from content hashes by a prefix. */
  static uint64_t hashOfPath(const std::string& path) {
    const std::string keyed = "file:" + path;
    return hashOf(keyed.data(), keyed.size());
  }

private:
  /** One cached image: when it was last asked for, the bytes it counts
   *  against the budget, and -- for one from a file -- the file's size and
   *  time, so an edited file is decoded again. */
  struct Entry {
    Image image;
    int64_t lastUsed = 0;
    size_t bytes = 0;
    uint64_t fileSize = 0, fileModified = 0;
  };
  using Map = std::unordered_map<uint64_t, Entry>;

  int64_t now() const {
    if (clock_) return clock_();
    return (int64_t) std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  Entry& insertLocked(uint64_t key, Image image) {
    purgeLocked();
    auto it = entries_.find(key);
    if (it != entries_.end()) {
      // Two threads decoded the same thing at once, or an id was reused:
      // the newer image replaces the older, whose holders keep theirs.
      bytes_ -= it->second.bytes;
      entries_.erase(it);
    }
    Entry e;
    e.bytes = image->sizeInBytes();
    e.image = std::move(image);
    e.lastUsed = now();
    bytes_ += e.bytes;
    Entry& stored = entries_[key] = std::move(e);
    // The new image is held by its caller as well as here, so the budget
    // can never evict what was just asked for.
    const Image keep = stored.image;
    enforceBudgetLocked();
    return entries_[key];
  }

  Map::iterator eraseLocked(Map::iterator it) {
    bytes_ -= it->second.bytes;
    return entries_.erase(it);
  }

  /** An image somebody holds is in use, and its clock restarts: the timeout
   *  runs from when it was last held or asked for, as the reference's does,
   *  so an editor closed after an hour finds its art still there when it
   *  reopens a moment later. */
  void purgeLocked() {
    const int64_t t = now();
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (it->second.image.use_count() > 1) {
        it->second.lastUsed = t;
        ++it;
      } else if (t - it->second.lastUsed > (int64_t) timeoutMs_) {
        it = eraseLocked(it);
      } else {
        ++it;
      }
    }
  }

  /** Past the budget: unused images, least recently asked for first. */
  void enforceBudgetLocked() {
    while (bytes_ > maxBytes_) {
      auto oldest = entries_.end();
      for (auto it = entries_.begin(); it != entries_.end(); ++it)
        if (it->second.image.use_count() == 1 && (oldest == entries_.end() || it->second.lastUsed < oldest->second.lastUsed))
          oldest = it;
      if (oldest == entries_.end()) return; // everything left is in use
      eraseLocked(oldest);
    }
  }

  static void statFile(const std::string& path, uint64_t& size, uint64_t& modified) {
    size = modified = 0;
#if defined(_WIN32)
    struct _stat64 info;
    if (_wstat64(::sonore::pathio::widenPath(path.c_str()).c_str(), &info) == 0) {
      size = (uint64_t) info.st_size;
      modified = (uint64_t) info.st_mtime;
    }
#else
    struct stat info;
    if (::stat(path.c_str(), &info) == 0) {
      size = (uint64_t) info.st_size;
      modified = (uint64_t) info.st_mtime;
    }
#endif
  }

  mutable std::mutex mutex_;
  Map entries_;
  size_t bytes_ = 0;
  size_t maxBytes_ = 64u * 1024u * 1024u;
  int timeoutMs_ = 5000;
  std::function<int64_t()> clock_;
};

} // namespace gfx
} // namespace sonore
