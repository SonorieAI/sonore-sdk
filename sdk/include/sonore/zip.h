// SPDX-License-Identifier: Apache-2.0
//
// Reading a .zip.
//
// ── What it is for ──────────────────────────────────────────────────────────
//
// A preset pack. A sample pack. An expansion somebody downloaded. Every plugin
// that ships more than its own binary ships it as a zip, and without this the
// only options are to unpack it with a shell command -- which is a process
// launch from a plugin, on a thread the host owns -- or to ask the user to do
// it themselves and then explain where.
//
// ── Almost all of it already existed ───────────────────────────────────────
//
// gfx/inflate.h is a complete DEFLATE decoder, written by hand for PNG because
// every length and distance in the stream is attacker-controlled and the bounds
// checks were the point. A zip entry is a DEFLATE stream with a header on it,
// so what is here is the container format and nothing else.
//
// ── Reading only, and reading is the dangerous direction ───────────────────
//
// Writing a zip needs a DEFLATE COMPRESSOR, which is a different piece of work
// from a decompressor and one nothing here needs: a plugin reads packs, it does
// not make them. Stored (uncompressed) entries could be written trivially and
// deliberately are not -- a half-implemented writer producing zips that only
// this reader accepts is worse than no writer.
//
// Every size, offset and name length in a zip comes from the file, which came
// from the internet. So: nothing is trusted, every read is bounds-checked
// against the buffer, the uncompressed size is capped, and the CRC is VERIFIED
// rather than reported. A decoder that skipped the CRC would hand a plugin a
// subtly corrupt sample and no way to know.
//
// ── Zip slip ────────────────────────────────────────────────────────────────
//
// An entry may be named "../../../autostart/evil.sh". Extraction here is to
// MEMORY, so nothing is written anywhere by this file -- but any caller that
// then writes the result to disk has to check the name first, and
// isSafeEntryName is that check, named so it is hard to not notice.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "gfx/inflate.h"

namespace sonore {

/** One thing inside the archive. */
struct ZipEntry {
  /** As stored, with forward slashes -- the format mandates them, whatever the
   *  machine that wrote it uses. */
  std::string name;
  uint64_t compressedSize = 0;
  uint64_t uncompressedSize = 0;
  uint32_t crc = 0;
  /** 0 = stored, 8 = deflated. Anything else is refused rather than guessed. */
  uint16_t method = 0;
  /** Where the local header is, from the start of the file. */
  uint64_t localHeaderOffset = 0;
  /** A directory is an entry whose name ends in a slash and whose content is
   *  empty. Kept rather than dropped: a pack's folder structure is information,
   *  and a caller recreating it needs the empty ones too. */
  bool isDirectory = false;
};

class ZipFile {
public:
  /**
   * Read the directory of an archive already in memory.
   *
   * The whole file, not a stream. A preset pack is a few megabytes and a plugin
   * has it in a buffer already; a streaming reader would be a seek interface
   * threaded through every call for no case that exists here.
   */
  bool openFromMemory(const uint8_t* data, size_t size) {
    entries_.clear();
    data_ = nullptr;
    size_ = 0;
    if (!data || size < kEndRecordSize) return false;

    // ── Find the end-of-central-directory record ──
    //
    // Searched BACKWARDS, because it is at the end -- and it is not at a fixed
    // offset, since it carries a variable-length comment. The comment is
    // 16 bits, so it can be 65535 bytes and no further back than that.
    const size_t searchLimit = size < (kEndRecordSize + 65535u) ? size
                                                               : (kEndRecordSize + 65535u);
    size_t endAt = size;
    for (size_t back = kEndRecordSize; back <= searchLimit; ++back) {
      const size_t at = size - back;
      if (read32(data, size, at) == 0x06054b50u) {
        endAt = at;
        break;
      }
    }
    if (endAt == size) return false;

    const uint16_t entriesOnDisk = read16(data, size, endAt + 8);
    const uint32_t directorySize = read32(data, size, endAt + 12);
    const uint32_t directoryOffset = read32(data, size, endAt + 16);

    // Zip64 says "look elsewhere" with these sentinels. Refused rather than
    // half-read: a 4GB preset pack does not exist, and pretending to support
    // one would mean reading a directory that is not where this says it is.
    if (directoryOffset == 0xFFFFFFFFu || entriesOnDisk == 0xFFFFu) return false;
    if ((uint64_t) directoryOffset + directorySize > size) return false;

    // ── Walk the central directory ──
    size_t at = directoryOffset;
    const size_t directoryEnd = (size_t) directoryOffset + directorySize;
    for (uint16_t i = 0; i < entriesOnDisk; ++i) {
      if (at + kCentralHeaderSize > directoryEnd) return false;
      if (read32(data, size, at) != 0x02014b50u) return false;

      ZipEntry entry;
      const uint16_t flags = read16(data, size, at + 8);
      entry.method = read16(data, size, at + 10);
      entry.crc = read32(data, size, at + 16);
      entry.compressedSize = read32(data, size, at + 20);
      entry.uncompressedSize = read32(data, size, at + 24);
      const uint16_t nameLength = read16(data, size, at + 28);
      const uint16_t extraLength = read16(data, size, at + 30);
      const uint16_t commentLength = read16(data, size, at + 32);
      entry.localHeaderOffset = read32(data, size, at + 42);

      const size_t nameAt = at + kCentralHeaderSize;
      if (nameAt + nameLength > directoryEnd) return false;
      entry.name.assign((const char*) data + nameAt, nameLength);

      // Bit 11 says the name is UTF-8. Without it the name is CP437, and
      // converting is a table nobody needs: every archiver written this century
      // sets the flag, and a name that arrives as CP437 is passed through as
      // the bytes it is rather than being mangled by a guess.
      (void) flags;

      entry.isDirectory = !entry.name.empty() && entry.name.back() == '/';
      entries_.push_back(std::move(entry));

      at = nameAt + nameLength + extraLength + commentLength;
    }

    data_ = data;
    size_ = size;
    return true;
  }

  int numEntries() const { return (int) entries_.size(); }

  const ZipEntry& entry(int index) const {
    static const ZipEntry empty;
    if (index < 0 || index >= (int) entries_.size()) return empty;
    return entries_[(size_t) index];
  }

  /** By name, exactly. -1 for anything not there. */
  int indexOf(const std::string& name) const {
    for (size_t i = 0; i < entries_.size(); ++i)
      if (entries_[i].name == name) return (int) i;
    return -1;
  }

  /**
   * Decompress one entry into memory.
   *
   * `limit` caps the output. A zip records its own uncompressed size and a
   * malicious one can lie -- the classic decompression bomb is a few kilobytes
   * claiming to be a few gigabytes -- so the size in the header is a HINT and
   * this is the rule.
   *
   * The CRC is checked. False for a mismatch, and the buffer is cleared: a
   * caller that ignored the return value would otherwise have plausible
   * garbage rather than nothing.
   */
  bool extract(int index, std::vector<uint8_t>* out, size_t limit = 256u * 1024u * 1024u) const {
    if (!out) return false;
    out->clear();
    if (!data_ || index < 0 || index >= (int) entries_.size()) return false;
    const ZipEntry& e = entries_[(size_t) index];
    if (e.isDirectory) return true; // nothing to give, and not an error

    // The LOCAL header, whose name and extra lengths differ from the central
    // one's -- extra fields are routinely present in one and not the other, and
    // assuming they match is how a reader lands in the middle of the data.
    const size_t local = (size_t) e.localHeaderOffset;
    if (local + kLocalHeaderSize > size_) return false;
    if (read32(data_, size_, local) != 0x04034b50u) return false;
    const uint16_t nameLength = read16(data_, size_, local + 26);
    const uint16_t extraLength = read16(data_, size_, local + 28);

    const size_t dataAt = local + kLocalHeaderSize + nameLength + extraLength;
    if (dataAt > size_) return false;
    if (e.compressedSize > size_ - dataAt) return false;
    if (e.uncompressedSize > limit) return false;

    if (e.method == 0) {
      // Stored. The two sizes must agree, or the entry is lying about one of
      // them and there is no way to tell which.
      if (e.compressedSize != e.uncompressedSize) return false;
      out->assign(data_ + dataAt, data_ + dataAt + (size_t) e.compressedSize);
    } else if (e.method == 8) {
      if (!gfx::Inflater::inflate(data_ + dataAt, (size_t) e.compressedSize, *out, limit)) {
        out->clear();
        return false;
      }
    } else {
      return false; // bzip2, LZMA, XZ -- refused rather than guessed at
    }

    if (crc32(out->data(), out->size()) != e.crc) {
      out->clear();
      return false;
    }
    return true;
  }

  bool extract(const std::string& name, std::vector<uint8_t>* out,
               size_t limit = 256u * 1024u * 1024u) const {
    return extract(indexOf(name), out, limit);
  }

  /**
   * Whether an entry's name is safe to append to a directory.
   *
   * An archive may name an entry "../../../autostart/evil.sh", and a caller
   * that joins that to a destination folder writes outside it -- "zip slip",
   * and it has been found in the unpacking code of an enormous number of
   * shipped applications.
   *
   * Nothing in this file writes to disk, so nothing here is vulnerable. This
   * exists for the caller that does, and is named so that not calling it looks
   * like an omission.
   */
  static bool isSafeEntryName(const std::string& name) {
    if (name.empty()) return false;
    // Absolute, in either convention, and a Windows drive.
    if (name[0] == '/' || name[0] == '\\') return false;
    if (name.size() >= 2 && name[1] == ':') return false;
    // Any component that is "..", and any backslash at all -- the format says
    // forward slashes, so a backslash is either an archiver being wrong or
    // somebody hoping the reader splits on only one of them.
    size_t start = 0;
    for (size_t i = 0; i <= name.size(); ++i) {
      if (i == name.size() || name[i] == '/') {
        const std::string part = name.substr(start, i - start);
        if (part == "..") return false;
        start = i + 1;
      } else if (name[i] == '\\') {
        return false;
      }
    }
    return true;
  }

  /**
   * CRC-32, the one zip and PNG both use.
   *
   * The table is built once, on first use, rather than written out as 256
   * constants -- generating it is four lines and a table in the source is 256
   * opportunities for one digit to be wrong in a way that only shows up on the
   * files that happen to hit that byte.
   */
  static uint32_t crc32(const uint8_t* data, size_t size) {
    static const uint32_t* table = [] {
      static uint32_t built[256];
      for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int bit = 0; bit < 8; ++bit) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        built[i] = c;
      }
      return built;
    }();

    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
  }

private:
  static constexpr size_t kEndRecordSize = 22;
  static constexpr size_t kCentralHeaderSize = 46;
  static constexpr size_t kLocalHeaderSize = 30;

  /** Little-endian, bounds-checked. Zero past the end rather than a read: every
   *  offset in this file came from the file. */
  static uint16_t read16(const uint8_t* data, size_t size, size_t at) {
    if (at + 2 > size) return 0;
    return (uint16_t) (data[at] | (data[at + 1] << 8));
  }

  static uint32_t read32(const uint8_t* data, size_t size, size_t at) {
    if (at + 4 > size) return 0;
    return (uint32_t) data[at] | ((uint32_t) data[at + 1] << 8) | ((uint32_t) data[at + 2] << 16) |
           ((uint32_t) data[at + 3] << 24);
  }

  std::vector<ZipEntry> entries_;
  const uint8_t* data_ = nullptr;
  size_t size_ = 0;
};

} // namespace sonore
