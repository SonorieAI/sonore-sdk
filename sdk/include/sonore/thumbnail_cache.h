// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: AudioThumbnailCache: waveforms already computed, kept by source.
//
// A sampler holds the thumbnail of the sample it has loaded for as long as it
// holds the sample, and needs nothing more. A BROWSER does: it draws a
// waveform for every file the user scrolls past, each of which means decoding
// the whole file, and scrolling back up means decoding them all again. This
// keeps what was computed, keyed by the source, so the second look is a
// lookup.
//
// ── Keyed by identity, not by name ──────────────────────────────────────────
//
// The key is a 64-bit hash the caller chooses. hashFile() makes one from a
// file's path, length and modification time, so a file the user edits and
// saves gets a NEW key and the old waveform is simply never asked for again
// -- there is no invalidation to get wrong, and the stale entry ages out of
// the budget like any other. (Windows reports modification times here to the
// second; a file saved twice within one second at the same length keeps its
// key. The length changes with nearly every real edit.) hashData() keys audio
// that never had a file.
//
// ── Bounded by memory, least recently used out first ────────────────────────
//
// The reference bounds its cache by a COUNT of thumbnails. A count says
// nothing about memory: a thumbnail of a one-shot at 200 buckets and one of an
// hour-long stem at 20 000 differ a hundredfold. The budget here is bytes --
// what the buckets occupy plus a fixed overhead per entry -- and storing past
// it evicts the least recently used. A lookup counts as a use, because the
// file the user keeps coming back to is the one worth keeping.
//
// ── Kept across runs ────────────────────────────────────────────────────────
//
// saveTo() writes the whole cache, most recently used first, and loadFrom()
// reads it back -- so a browser opens with the waveforms it showed last time.
// The bytes come from disk, so they are parsed as untrusted: anything
// malformed is refused whole and the cache is left as it was.
//
// Thread-safe: a loader thread stores while the UI thread looks up. Never on
// the audio thread -- every call can allocate.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>

#include "path_io.h"
#include "thumbnail.h"

namespace sonore {

/**
 * Computed AudioThumbnails kept under a 64-bit key, within a budget in
 * bytes (16 MB unless told otherwise), least recently used out first, and
 * savable across runs. Mirrors the reference's class of the same name,
 * bounded by memory rather than by count -- see the file comment.
 *
 * It stores and hands back COPIES: storeThumb copies in and loadThumb copies
 * out, so nothing returned is tied to the cache's lifetime. Any thread but
 * the audio one, since every call locks and may allocate. The key is the
 * caller's to choose: hashFile() for a file, hashData() for anything else.
 */
class AudioThumbnailCache {
public:
  /** What an entry costs beyond its buckets: the list node, the map slot and
   *  the thumbnail object. An estimate, deliberately on the high side, so a
   *  budget of many tiny thumbnails is not exceeded by the bookkeeping. */
  static constexpr size_t kEntryOverhead = 128;

  /** Empty, with a budget of `maxBytes` bytes. */
  explicit AudioThumbnailCache(size_t maxBytes = 16u * 1024u * 1024u) : maxBytes_(maxBytes) {}

  AudioThumbnailCache(const AudioThumbnailCache&) = delete;            ///< Not copyable: it holds a lock.
  AudioThumbnailCache& operator=(const AudioThumbnailCache&) = delete; ///< Not copyable: it holds a lock.

  /** Change the budget; shrinking it evicts at once. */
  void setMaxBytes(size_t maxBytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    maxBytes_ = maxBytes;
    evictLocked();
  }
  /** The budget, in bytes. */
  size_t maxBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return maxBytes_;
  }
  /** What the entries cost now, in bytes, kEntryOverhead each included;
   *  never more than maxBytes(). */
  size_t bytesUsed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bytesUsed_;
  }
  /** How many thumbnails it holds. */
  size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return index_.size();
  }

  /** Keep a copy of `thumb` under `hash`, replacing what was there. False
   *  when it is empty or alone larger than the whole budget -- storing it
   *  would only evict everything else and then itself. */
  bool storeThumb(const AudioThumbnail& thumb, uint64_t hash) {
    const size_t cost = thumb.memoryBytes() + kEntryOverhead;
    std::lock_guard<std::mutex> lock(mutex_);
    if (thumb.empty() || cost > maxBytes_) return false;
    removeLocked(hash);
    entries_.push_front(Entry{hash, thumb, cost});
    index_[hash] = entries_.begin();
    bytesUsed_ += cost;
    evictLocked();
    return true;
  }

  /** Copy the thumbnail stored under `hash` into `thumb` and mark it used.
   *  False, leaving `thumb` alone, when there is none. */
  bool loadThumb(AudioThumbnail& thumb, uint64_t hash) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = index_.find(hash);
    if (found == index_.end()) return false;
    entries_.splice(entries_.begin(), entries_, found->second); // now most recent
    thumb = found->second->thumb;
    return true;
  }

  /** Whether a thumbnail is stored under `hash`. Does not count as a use. */
  bool containsThumb(uint64_t hash) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return index_.count(hash) != 0;
  }

  /** Forget the thumbnail under `hash`. False when there was none. */
  bool removeThumb(uint64_t hash) {
    std::lock_guard<std::mutex> lock(mutex_);
    return removeLocked(hash);
  }

  /** Forget every thumbnail; the budget stays. */
  void clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    index_.clear();
    bytesUsed_ = 0;
  }

  /** The whole cache as bytes: "SNTC", a version, the entry count, then each
   *  entry's hash, length and AudioThumbnail::saveTo bytes, most recently
   *  used first. */
  void saveTo(std::vector<uint8_t>& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    out.clear();
    const uint8_t magic[4] = {'S', 'N', 'T', 'C'};
    out.insert(out.end(), magic, magic + 4);
    putU32(out, 1);
    putU32(out, (uint32_t) entries_.size());
    std::vector<uint8_t> one;
    for (const Entry& e : entries_) {
      e.thumb.saveTo(one);
      putU64(out, e.hash);
      putU32(out, (uint32_t) one.size());
      out.insert(out.end(), one.begin(), one.end());
    }
  }

  /**
   * Replace the contents with what saveTo wrote. Every entry is parsed and
   * checked before anything is changed; one bad entry refuses the lot and
   * the cache keeps what it had. Entries past the budget are dropped from
   * the least recently used end, as storing them one by one would.
   */
  bool loadFrom(const uint8_t* data, size_t size) {
    if (!data || size < 12 || std::memcmp(data, "SNTC", 4) != 0 || getU32(data + 4) != 1)
      return false;
    const uint32_t count = getU32(data + 8);
    // Each entry is at least 12 bytes of header plus a 28-byte thumbnail, so
    // a count the remaining bytes cannot hold is refused before any parsing.
    if ((uint64_t) count * 40u > (uint64_t) (size - 12)) return false;
    std::vector<Entry> parsed;
    parsed.reserve(count);
    size_t at = 12;
    for (uint32_t i = 0; i < count; ++i) {
      if (size - at < 12) return false;
      const uint64_t hash = getU64(data + at);
      const uint32_t length = getU32(data + at + 8);
      at += 12;
      if (length > size - at) return false;
      AudioThumbnail thumb;
      if (!thumb.loadFrom(data + at, length)) return false;
      at += length;
      const size_t cost = thumb.memoryBytes() + kEntryOverhead;
      parsed.push_back(Entry{hash, std::move(thumb), cost});
    }
    if (at != size) return false; // trailing bytes: not something saveTo wrote

    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    index_.clear();
    bytesUsed_ = 0;
    // Oldest first, each pushed to the front, so the file's order -- most
    // recent first -- is the list's order again. A hash repeated in a file
    // keeps its most recent entry.
    for (size_t i = parsed.size(); i-- > 0;) {
      Entry& e = parsed[i];
      if (e.cost > maxBytes_) continue;
      removeLocked(e.hash);
      entries_.push_front(std::move(e));
      index_[entries_.front().hash] = entries_.begin();
      bytesUsed_ += entries_.front().cost;
    }
    evictLocked();
    return true;
  }

  /** A key for a file: its path, length and modification time, so the key
   *  changes when the file does. 0 when the file cannot be found -- which no
   *  real file hashes to, see hashData. */
  static uint64_t hashFile(const char* path) {
    if (!path || !path[0]) return 0;
    uint64_t size = 0;
    int64_t modified = 0;
#if defined(_WIN32)
    struct _stat64 st {};
    if (_wstat64(pathio::widenPath(path).c_str(), &st) != 0) return 0;
#else
    struct stat st {};
    if (::stat(path, &st) != 0) return 0;
#endif
    size = (uint64_t) st.st_size;
    modified = (int64_t) st.st_mtime;
    uint64_t h = hashData(path, std::strlen(path));
    h = mix(h, &size, sizeof(size));
    h = mix(h, &modified, sizeof(modified));
    return h == 0 ? 1 : h;
  }

  /** FNV-1a, 64-bit, over any bytes: decoded audio that never had a file, or
   *  a name the caller already trusts to be unique. */
  static uint64_t hashData(const void* data, size_t size) {
    return mix(14695981039346656037ull, data, size);
  }

private:
  /** One cached thumbnail: its key, its own copy, and what it counts against
   *  the budget -- its buckets plus kEntryOverhead. */
  struct Entry {
    uint64_t hash;        ///< Its key.
    AudioThumbnail thumb; ///< Its own copy.
    size_t cost;          ///< Bytes it counts against the budget.
  };

  static uint64_t mix(uint64_t h, const void* data, size_t size) {
    const uint8_t* p = (const uint8_t*) data;
    for (size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
  }

  bool removeLocked(uint64_t hash) {
    const auto found = index_.find(hash);
    if (found == index_.end()) return false;
    bytesUsed_ -= found->second->cost;
    entries_.erase(found->second);
    index_.erase(found);
    return true;
  }

  void evictLocked() {
    while (bytesUsed_ > maxBytes_ && !entries_.empty()) {
      const Entry& oldest = entries_.back();
      bytesUsed_ -= oldest.cost;
      index_.erase(oldest.hash);
      entries_.pop_back();
    }
  }

  static void putU32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back((uint8_t) ((v >> (8 * i)) & 0xffu));
  }
  static void putU64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back((uint8_t) ((v >> (8 * i)) & 0xffu));
  }
  static uint32_t getU32(const uint8_t* p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) |
           ((uint32_t) p[3] << 24);
  }
  static uint64_t getU64(const uint8_t* p) {
    return (uint64_t) getU32(p) | ((uint64_t) getU32(p + 4) << 32);
  }

  mutable std::mutex mutex_;
  std::list<Entry> entries_; // most recently used at the front
  std::unordered_map<uint64_t, std::list<Entry>::iterator> index_;
  size_t bytesUsed_ = 0;
  size_t maxBytes_;
};

} // namespace sonore
