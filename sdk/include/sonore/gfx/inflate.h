// SPDX-License-Identifier: Apache-2.0
//
// DEFLATE, because PNG is a container around it.
//
// ── Why this is written rather than depended on ─────────────────────────────
//
// The alternative is vendoring miniz or stb_image, which are both fine and both
// permissively licensed. This SDK already parses TrueType itself and rasterises
// itself rather than taking a library for either, on the argument that the small
// thing you understand beats the big thing you do not. DEFLATE is the same size
// of problem as those: one Huffman decoder, three block types, and a sliding
// window.
//
// It is also the part of a PNG decoder where a bug is a SECURITY bug. Every
// length and every distance in the stream is attacker-controlled, and a decoder
// that trusts them reads or writes outside its buffers. Writing it means every
// one of those bounds is a line somebody chose, and the tests below aim at
// exactly those lines rather than at "does this image look right".
//
// ── What it implements ──────────────────────────────────────────────────────
//
// RFC 1951 in full: stored, fixed-Huffman and dynamic-Huffman blocks. Plus the
// two-byte RFC 1950 zlib wrapper PNG puts around it. Not implemented, because
// nothing produces them: preset dictionaries, and the reserved block type 3
// which is an error by definition.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace sonore {
namespace gfx {

/**
 * A canonical Huffman decoder built from code lengths.
 *
 * DEFLATE never transmits the codes themselves, only how LONG each symbol's
 * code is; the codes follow from the lengths by a rule both ends know. That is
 * the whole trick of the format and it is why this table is rebuilt for every
 * dynamic block.
 */
class HuffmanTable {
public:
  static constexpr int kMaxBits = 15;

  /** False for a malformed set of lengths -- over-subscribed, meaning the code
   *  lengths describe more codes than a binary tree of that depth can hold. A
   *  stream saying that is corrupt or hostile, and building a table from it
   *  would decode garbage forever rather than stopping. */
  bool build(const uint8_t* lengths, int count) {
    counts_.assign(kMaxBits + 1, 0);
    symbols_.assign((size_t) count, 0);
    for (int i = 0; i < count; ++i) counts_[lengths[i]]++;
    // Length zero means "this symbol is not used", not "a code of no bits".
    counts_[0] = 0;

    int left = 1;
    for (int length = 1; length <= kMaxBits; ++length) {
      left <<= 1;
      left -= counts_[(size_t) length];
      if (left < 0) return false; // over-subscribed
    }

    std::vector<int> offsets((size_t) kMaxBits + 1, 0);
    for (int length = 1; length < kMaxBits; ++length)
      offsets[(size_t) length + 1] = offsets[(size_t) length] + counts_[(size_t) length];
    for (int i = 0; i < count; ++i)
      if (lengths[i] != 0) symbols_[(size_t) offsets[lengths[i]]++] = i;
    return true;
  }

  const std::vector<int>& counts() const { return counts_; }
  const std::vector<int>& symbols() const { return symbols_; }

private:
  std::vector<int> counts_;
  std::vector<int> symbols_;
};

/**
 * Raw DEFLATE and zlib-wrapped DEFLATE.
 *
 * `limit` is not optional. A compressed stream is a handful of bytes that can
 * legitimately expand to gigabytes, and a decoder with no ceiling turns a
 * malformed preset or a hostile PNG into an out-of-memory kill of the HOST --
 * which is a plugin taking a DAW down with it.
 */
class Inflater {
public:
  /** Raw DEFLATE, no wrapper. */
  static bool inflate(const uint8_t* data, size_t size, std::vector<uint8_t>& out, size_t limit) {
    Inflater in(data, size, out, limit);
    return in.run();
  }

  /** The RFC 1950 wrapper PNG uses: two header bytes, the stream, an Adler-32.
   *  The checksum is verified -- a decoder that ignored it would hand a plugin
   *  a subtly corrupt image and no way to know. */
  static bool inflateZlib(const uint8_t* data, size_t size, std::vector<uint8_t>& out,
                          size_t limit) {
    if (size < 6) return false; // 2 header + at least something + 4 adler
    const uint8_t cmf = data[0], flg = data[1];
    if ((cmf & 0x0f) != 8) return false;          // only DEFLATE is defined
    if (((cmf << 8) | flg) % 31 != 0) return false; // the header's own check
    if (flg & 0x20) return false;                 // a preset dictionary; nothing emits these

    if (!inflate(data + 2, size - 2, out, limit)) return false;

    // The Adler-32 is the last four bytes, big-endian. Finding it needs to know
    // where the stream ended, which inflate does not report -- so it is taken
    // from the end of the buffer, which is where PNG puts it.
    const uint8_t* tail = data + size - 4;
    const uint32_t expected = ((uint32_t) tail[0] << 24) | ((uint32_t) tail[1] << 16) |
                              ((uint32_t) tail[2] << 8) | (uint32_t) tail[3];
    return adler32(out.data(), out.size()) == expected;
  }

  static uint32_t adler32(const uint8_t* data, size_t size) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < size; ++i) {
      a = (a + data[i]) % 65521u;
      b = (b + a) % 65521u;
    }
    return (b << 16) | a;
  }

private:
  Inflater(const uint8_t* data, size_t size, std::vector<uint8_t>& out, size_t limit)
      : data_(data), size_(size), out_(out), limit_(limit) {}

  bool run() {
    out_.clear();
    for (;;) {
      const int last = bits(1);
      const int type = bits(2);
      if (failed_) return false;
      bool ok = false;
      switch (type) {
        case 0: ok = stored(); break;
        case 1: ok = fixed(); break;
        case 2: ok = dynamic(); break;
        default: return false; // type 3 is reserved and means corrupt
      }
      if (!ok) return false;
      if (last) break;
    }
    return true;
  }

  /** LSB-first, which is DEFLATE's bit order and the opposite of the byte order
   *  its length fields use. Getting the two the same way round is the classic
   *  way to write a decoder that works on stored blocks and nothing else. */
  int bits(int count) {
    while (bitCount_ < count) {
      if (position_ >= size_) {
        failed_ = true;
        return 0;
      }
      bitBuffer_ |= (uint32_t) data_[position_++] << bitCount_;
      bitCount_ += 8;
    }
    const int value = (int) (bitBuffer_ & ((1u << count) - 1u));
    bitBuffer_ >>= count;
    bitCount_ -= count;
    return value;
  }

  bool stored() {
    // Aligned to a byte, then a length and its one's complement -- which is the
    // format's own consistency check and worth honouring.
    bitBuffer_ = 0;
    bitCount_ = 0;
    if (position_ + 4 > size_) return false;
    const uint32_t length = (uint32_t) data_[position_] | ((uint32_t) data_[position_ + 1] << 8);
    const uint32_t inverse =
        (uint32_t) data_[position_ + 2] | ((uint32_t) data_[position_ + 3] << 8);
    position_ += 4;
    if ((length ^ 0xffffu) != inverse) return false;
    if (position_ + length > size_) return false;
    if (out_.size() + length > limit_) return false;
    out_.insert(out_.end(), data_ + position_, data_ + position_ + length);
    position_ += length;
    return true;
  }

  bool fixed() {
    // The lengths are fixed by the spec, so both tables are built from
    // constants rather than read from the stream.
    static HuffmanTable* literals = nullptr;
    static HuffmanTable* distances = nullptr;
    if (!literals) {
      static HuffmanTable lit, dist;
      uint8_t lengths[288];
      for (int i = 0; i < 144; ++i) lengths[i] = 8;
      for (int i = 144; i < 256; ++i) lengths[i] = 9;
      for (int i = 256; i < 280; ++i) lengths[i] = 7;
      for (int i = 280; i < 288; ++i) lengths[i] = 8;
      lit.build(lengths, 288);
      uint8_t distLengths[30];
      for (int i = 0; i < 30; ++i) distLengths[i] = 5;
      dist.build(distLengths, 30);
      literals = &lit;
      distances = &dist;
    }
    return block(*literals, *distances);
  }

  bool dynamic() {
    const int numLiterals = bits(5) + 257;
    const int numDistances = bits(1 + 4) + 1;
    const int numLengths = bits(4) + 4;
    if (failed_ || numLiterals > 286 || numDistances > 30) return false;

    // The code lengths are themselves Huffman-coded, in an order chosen so the
    // common ones come first and the tail can be omitted.
    static const int kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1,
                                   15};
    uint8_t lengthLengths[19] = {0};
    for (int i = 0; i < numLengths; ++i) lengthLengths[kOrder[i]] = (uint8_t) bits(3);
    if (failed_) return false;

    HuffmanTable lengthTable;
    if (!lengthTable.build(lengthLengths, 19)) return false;

    std::vector<uint8_t> lengths((size_t) (numLiterals + numDistances), 0);
    int at = 0;
    while (at < numLiterals + numDistances) {
      const int symbol = decode(lengthTable);
      if (symbol < 0) return false;
      if (symbol < 16) {
        lengths[(size_t) at++] = (uint8_t) symbol;
        continue;
      }
      int repeat = 0;
      uint8_t value = 0;
      if (symbol == 16) {
        // Repeat the PREVIOUS length. With nothing before it there is nothing
        // to repeat, and a stream saying so is corrupt.
        if (at == 0) return false;
        value = lengths[(size_t) at - 1];
        repeat = 3 + bits(2);
      } else if (symbol == 17) {
        repeat = 3 + bits(3);
      } else {
        repeat = 11 + bits(7);
      }
      if (failed_ || at + repeat > numLiterals + numDistances) return false;
      for (int i = 0; i < repeat; ++i) lengths[(size_t) at++] = value;
    }

    HuffmanTable literals, distances;
    if (!literals.build(lengths.data(), numLiterals)) return false;
    if (!distances.build(lengths.data() + numLiterals, numDistances)) return false;
    return block(literals, distances);
  }

  /** One symbol, walking the canonical code one bit at a time. */
  int decode(const HuffmanTable& table) {
    int code = 0, first = 0, index = 0;
    for (int length = 1; length <= HuffmanTable::kMaxBits; ++length) {
      code |= bits(1);
      if (failed_) return -1;
      const int count = table.counts()[(size_t) length];
      if (code - first < count) {
        const int at = index + (code - first);
        if (at < 0 || at >= (int) table.symbols().size()) return -1;
        return table.symbols()[(size_t) at];
      }
      index += count;
      first = (first + count) << 1;
      code <<= 1;
    }
    return -1; // no code that long: corrupt
  }

  bool block(const HuffmanTable& literals, const HuffmanTable& distances) {
    static const int kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,
                                        15, 17, 19, 23, 27, 31, 35, 43, 51,  59,
                                        67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const int kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                         2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const int kDistanceBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,
                                          17,   25,   33,   49,   65,   97,    129,   193,
                                          257,  385,  513,  769,  1025, 1537,  2049,  3073,
                                          4097, 6145, 8193, 12289, 16385, 24577};
    static const int kDistanceExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                            6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

    for (;;) {
      const int symbol = decode(literals);
      if (symbol < 0) return false;
      if (symbol < 256) {
        if (out_.size() >= limit_) return false;
        out_.push_back((uint8_t) symbol);
        continue;
      }
      if (symbol == 256) return true; // end of block

      const int lengthIndex = symbol - 257;
      if (lengthIndex >= 29) return false;
      const int length = kLengthBase[lengthIndex] + bits(kLengthExtra[lengthIndex]);

      const int distanceSymbol = decode(distances);
      if (distanceSymbol < 0 || distanceSymbol >= 30) return false;
      const int distance =
          kDistanceBase[distanceSymbol] + bits(kDistanceExtra[distanceSymbol]);
      if (failed_) return false;

      // THE bounds check. `distance` is attacker-controlled, and a reference
      // reaching further back than the output holds would read whatever is
      // before the buffer -- which is the classic decompressor vulnerability
      // and the reason this is written rather than trusted.
      if (distance <= 0 || (size_t) distance > out_.size()) return false;
      if (out_.size() + (size_t) length > limit_) return false;

      // Copied a byte at a time, deliberately: a run may overlap itself, which
      // is how DEFLATE encodes a repeated pattern, and memcpy would be
      // undefined there and would produce the wrong bytes with any vectorised
      // implementation.
      size_t from = out_.size() - (size_t) distance;
      for (int i = 0; i < length; ++i) out_.push_back(out_[from + (size_t) i]);
    }
  }

  const uint8_t* data_;
  size_t size_;
  std::vector<uint8_t>& out_;
  size_t limit_;
  size_t position_ = 0;
  uint32_t bitBuffer_ = 0;
  int bitCount_ = 0;
  bool failed_ = false;
};

} // namespace gfx
} // namespace sonore
