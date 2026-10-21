// SPDX-License-Identifier: Apache-2.0
//
// SHA-256, MD5 and Whirlpool.
//
// ── Why a hash is in an audio SDK at all ────────────────────────────────────
//
// Two jobs, both about NAMING things by their content rather than by where
// they came from:
//
//   - A sample a plugin loaded. Two projects referencing the same kick drum
//     should agree that it is the same kick drum, and a path does not tell you
//     that -- the same file lives at a different path on every machine.
//   - A state blob. "Has this preset changed since it was saved" is a question
//     about bytes, and comparing bytes to bytes gets slower as the state gets
//     bigger while comparing 32 of them does not.
//
// ── Which one ───────────────────────────────────────────────────────────────
//
// SHA-256, unless something outside this SDK names another. It is the one
// every other system in this project speaks, and the only one of the three
// whose collision resistance anybody still relies on.
//
// MD5 is here for the files and tools that name it: a sample pack's manifest
// of MD5 sums, a catalogue keyed on one, a digest the reference computed. It
// is BROKEN as a security hash -- two different files with the same MD5 are an
// afternoon's work on a laptop -- so it identifies data nobody is attacking
// and never vouches for data somebody might be.
//
// Whirlpool is the reference's 512-bit hash, here so a digest made there can
// be checked here. Nothing in this SDK picks it on its own merits.
//
// Every one is tested against its published vectors (RFC 1321's suite, the
// FIPS examples, the ISO/NESSIE set), because a hash nobody checks against
// somebody else's answers is a hash that agrees only with itself.
//
// NOT for passwords. A bare hash is the wrong tool for those and none of these
// has a salt or a work factor -- if that job ever appears here it wants a
// purpose-built KDF, not this file with a loop around it.
//
// Written from FIPS 180-4, RFC 1321 and the Whirlpool specification, which
// are descriptions of arithmetic. SHA-256's constants are the first thirty-two
// bits of the fractional parts of cube roots of primes; Whirlpool's S-box and
// its round tables are BUILT at first use from the specification's three
// 4-bit mini-boxes and its circulant matrix, rather than pasted in as 2,048
// opaque words. Derived, not copied: a typo in a derivation breaks every
// vector, where a typo in a pasted table can break only the inputs that reach
// that entry.
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "path_io.h"

namespace sonore {
namespace hash {

/**
 * A SHA-256 in progress.
 *
 * Streaming, because the thing most worth hashing here is a file too big to
 * want a second copy of in memory.
 */
class Sha256 {
public:
  /** Ready for the first update(). */
  Sha256() { reset(); }

  /** Forgets everything given so far, as if just constructed. */
  void reset() {
    // The initial state: fractional parts of the square roots of the first
    // eight primes.
    h_[0] = 0x6a09e667u; h_[1] = 0xbb67ae85u; h_[2] = 0x3c6ef372u; h_[3] = 0xa54ff53au;
    h_[4] = 0x510e527fu; h_[5] = 0x9b05688cu; h_[6] = 0x1f83d9abu; h_[7] = 0x5be0cd19u;
    length_ = 0;
    pending_ = 0;
  }

  /** Appends `bytes` bytes at `data` to the message; `data` may be null only
   *  when `bytes` is 0. Allocates nothing. */
  void update(const void* data, size_t bytes) {
    const uint8_t* p = (const uint8_t*) data;
    length_ += (uint64_t) bytes;
    while (bytes > 0) {
      const size_t room = 64 - pending_;
      const size_t take = bytes < room ? bytes : room;
      std::memcpy(buffer_ + pending_, p, take);
      pending_ += take;
      p += take;
      bytes -= take;
      if (pending_ == 64) {
        block(buffer_);
        pending_ = 0;
      }
    }
  }

  /** Appends the string's bytes as they are, with no terminator. */
  void update(const std::string& text) { update(text.data(), text.size()); }

  /** The digest, as 32 bytes. Finishing does not disturb the state, so a
   *  caller may keep appending afterwards -- which is what makes this usable
   *  for "the hash so far" without an extra copy of the object. */
  void finish(uint8_t out[32]) const {
    // On a COPY. The padding is part of the digest, not part of the message,
    // and folding it into h_ would make a second finish() answer differently.
    Sha256 copy = *this;

    // A single 1 bit, zeroes, then the length in BITS as a big-endian 64.
    const uint64_t bits = copy.length_ * 8u;
    const uint8_t one = 0x80;
    copy.update(&one, 1);
    const uint8_t zero = 0;
    while (copy.pending_ != 56) copy.update(&zero, 1);
    uint8_t tail[8];
    for (int i = 0; i < 8; ++i) tail[i] = (uint8_t) ((bits >> (56 - i * 8)) & 0xff);
    copy.update(tail, 8);

    for (int i = 0; i < 8; ++i) {
      out[i * 4 + 0] = (uint8_t) ((copy.h_[i] >> 24) & 0xff);
      out[i * 4 + 1] = (uint8_t) ((copy.h_[i] >> 16) & 0xff);
      out[i * 4 + 2] = (uint8_t) ((copy.h_[i] >> 8) & 0xff);
      out[i * 4 + 3] = (uint8_t) (copy.h_[i] & 0xff);
    }
  }

  /** The digest as 64 lowercase hex characters, which is how it is written
   *  everywhere a human or a filename will see it. */
  std::string hex() const {
    uint8_t digest[32];
    finish(digest);
    static const char* kDigits = "0123456789abcdef";
    std::string text;
    text.reserve(64);
    for (int i = 0; i < 32; ++i) {
      text += kDigits[(digest[i] >> 4) & 0xf];
      text += kDigits[digest[i] & 0xf];
    }
    return text;
  }

private:
  static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

  void block(const uint8_t* chunk) {
    // The round constants: fractional parts of the cube roots of the first
    // sixty-four primes.
    static const uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
        0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
        0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
        0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
        0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
        0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
      w[i] = ((uint32_t) chunk[i * 4] << 24) | ((uint32_t) chunk[i * 4 + 1] << 16) |
             ((uint32_t) chunk[i * 4 + 2] << 8) | (uint32_t) chunk[i * 4 + 3];
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
    uint32_t e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; ++i) {
      const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const uint32_t ch = (e & f) ^ (~e & g);
      const uint32_t t1 = h + S1 + ch + k[i] + w[i];
      const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t t2 = S0 + maj;
      h = g; g = f; f = e; e = d + t1;
      d = c; c = b; b = a; a = t1 + t2;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
    h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
  }

  uint32_t h_[8]{};
  uint64_t length_ = 0;
  uint8_t buffer_[64]{};
  size_t pending_ = 0;
};

namespace hashdetail {

inline std::string toHex(const uint8_t* bytes, size_t count) {
  static const char* kDigits = "0123456789abcdef";
  std::string text;
  text.reserve(count * 2);
  for (size_t i = 0; i < count; ++i) {
    text += kDigits[(bytes[i] >> 4) & 0xf];
    text += kDigits[bytes[i] & 0xf];
  }
  return text;
}

/**
 * A file through any of the hashes here, 64 KB at a time: the memory used is
 * the same for a 4 KB preset and a 4 GB sample library.
 *
 * Empty for a file that cannot be opened OR cannot be read to its end. The
 * digest of the part before a read error is the digest of a file that does
 * not exist, and handing it back would be worse than handing back nothing.
 * Never throws: a caller hashing a sample that has since been unplugged wants
 * to say so in its interface, not to unwind.
 */
template <class Hash>
std::string streamFile(const char* path) {
  if (!path) return std::string();
  FILE* f = ::sonore::pathio::open(path, "rb");
  if (!f) return std::string();
  Hash h;
  std::vector<unsigned char> chunk(64 * 1024);
  for (;;) {
    const size_t got = std::fread(chunk.data(), 1, chunk.size(), f);
    if (got > 0) h.update(chunk.data(), got);
    if (got < chunk.size()) break; // end of file, or an error: ferror says which
  }
  const bool failed = std::ferror(f) != 0;
  std::fclose(f);
  return failed ? std::string() : h.hex();
}

} // namespace hashdetail

/** The whole of a buffer, in one call. */
inline std::string sha256Hex(const void* data, size_t bytes) {
  Sha256 s;
  s.update(data, bytes);
  return s.hex();
}

/** The same for a string's bytes. */
inline std::string sha256Hex(const std::string& text) {
  return sha256Hex(text.data(), text.size());
}

/** A file, streamed. Empty when it cannot be read -- see streamFile. */
inline std::string sha256File(const char* path) {
  return hashdetail::streamFile<Sha256>(path);
}

/**
 * An MD5 in progress (RFC 1321), streaming like Sha256 and with the same
 * shape: update, then finish or hex, and finishing leaves the state alone.
 *
 * For interoperating with something that already names MD5 -- read the
 * header before reaching for it for anything else.
 */
class MD5 {
public:
  /** Ready for the first update(). */
  MD5() { reset(); }

  /** Forgets everything given so far, as if just constructed. */
  void reset() {
    h_[0] = 0x67452301u;
    h_[1] = 0xefcdab89u;
    h_[2] = 0x98badcfeu;
    h_[3] = 0x10325476u;
    length_ = 0;
    pending_ = 0;
  }

  /** Appends `bytes` bytes at `data` to the message; `data` may be null only
   *  when `bytes` is 0. Allocates nothing. */
  void update(const void* data, size_t bytes) {
    const uint8_t* p = (const uint8_t*) data;
    length_ += (uint64_t) bytes;
    while (bytes > 0) {
      const size_t room = 64 - pending_;
      const size_t take = bytes < room ? bytes : room;
      std::memcpy(buffer_ + pending_, p, take);
      pending_ += take;
      p += take;
      bytes -= take;
      if (pending_ == 64) {
        block(buffer_);
        pending_ = 0;
      }
    }
  }

  /** Appends the string's bytes as they are, with no terminator. */
  void update(const std::string& text) { update(text.data(), text.size()); }

  /** The digest, as 16 bytes. */
  void finish(uint8_t out[16]) const {
    // SHA-256's padding with the byte order turned round: MD5 is
    // little-endian throughout, the length included.
    MD5 copy = *this;
    const uint64_t bits = copy.length_ * 8u;
    const uint8_t one = 0x80;
    copy.update(&one, 1);
    const uint8_t zero = 0;
    while (copy.pending_ != 56) copy.update(&zero, 1);
    uint8_t tail[8];
    for (int i = 0; i < 8; ++i) tail[i] = (uint8_t) ((bits >> (i * 8)) & 0xff);
    copy.update(tail, 8);
    for (int i = 0; i < 4; ++i)
      for (int b = 0; b < 4; ++b) out[i * 4 + b] = (uint8_t) ((copy.h_[i] >> (b * 8)) & 0xff);
  }

  /** The digest as 32 lowercase hex characters, the form md5sum prints. */
  std::string hex() const {
    uint8_t digest[16];
    finish(digest);
    return hashdetail::toHex(digest, 16);
  }

private:
  static uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

  void block(const uint8_t* chunk) {
    // floor(|sin(i + 1)| * 2^32), as RFC 1321 defines them. Written out
    // rather than computed: the C library only promises a sin() that is
    // close, and a constant one off in its last bit is a different hash.
    static const uint32_t k[64] = {
        0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au,
        0xa8304613u, 0xfd469501u, 0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
        0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u, 0xf61e2562u, 0xc040b340u,
        0x265e5a51u, 0xe9b6c7aau, 0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
        0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u,
        0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
        0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u, 0x289b7ec6u, 0xeaa127fau,
        0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
        0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u,
        0xffeff47du, 0x85845dd1u, 0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
        0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u};
    static const int shift[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                  5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                  6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

    uint32_t m[16];
    for (int i = 0; i < 16; ++i)
      m[i] = (uint32_t) chunk[i * 4] | ((uint32_t) chunk[i * 4 + 1] << 8) |
             ((uint32_t) chunk[i * 4 + 2] << 16) | ((uint32_t) chunk[i * 4 + 3] << 24);

    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
    for (int i = 0; i < 64; ++i) {
      uint32_t f;
      int g;
      if (i < 16) {
        f = (b & c) | (~b & d);
        g = i;
      } else if (i < 32) {
        f = (d & b) | (~d & c);
        g = (5 * i + 1) % 16;
      } else if (i < 48) {
        f = b ^ c ^ d;
        g = (3 * i + 5) % 16;
      } else {
        f = c ^ (b | ~d);
        g = (7 * i) % 16;
      }
      const uint32_t rotated = rotl(a + f + k[i] + m[g], shift[i]);
      a = d;
      d = c;
      c = b;
      b = b + rotated;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
  }

  uint32_t h_[4]{};
  uint64_t length_ = 0;
  uint8_t buffer_[64]{};
  size_t pending_ = 0;
};

/** The MD5 of a whole buffer, as 32 lowercase hex characters. */
inline std::string md5Hex(const void* data, size_t bytes) {
  MD5 m;
  m.update(data, bytes);
  return m.hex();
}

/** The same for a string's bytes. */
inline std::string md5Hex(const std::string& text) { return md5Hex(text.data(), text.size()); }

/** A file, streamed. Empty when it cannot be read -- see streamFile. */
inline std::string md5File(const char* path) { return hashdetail::streamFile<MD5>(path); }

/**
 * A Whirlpool in progress: the final (2003) version of the algorithm, as
 * ISO/IEC 10118-3 standardises it and the NESSIE vectors pin down -- not
 * Whirlpool-0 or Whirlpool-T, which share the name and not the output.
 *
 * 512-bit digest, 512-bit blocks, a 256-bit length counter. Same shape as
 * the others: update, then finish or hex.
 */
class Whirlpool {
public:
  /** Ready for the first update(). */
  Whirlpool() { reset(); }

  /** Forgets everything given so far, as if just constructed. */
  void reset() {
    for (uint64_t& word : h_) word = 0;
    length_ = 0;
    pending_ = 0;
  }

  /** Appends `bytes` bytes at `data` to the message; `data` may be null only
   *  when `bytes` is 0. Allocates nothing; the first block hashed in the
   *  process builds the round tables. */
  void update(const void* data, size_t bytes) {
    const uint8_t* p = (const uint8_t*) data;
    length_ += (uint64_t) bytes;
    while (bytes > 0) {
      const size_t room = 64 - pending_;
      const size_t take = bytes < room ? bytes : room;
      std::memcpy(buffer_ + pending_, p, take);
      pending_ += take;
      p += take;
      bytes -= take;
      if (pending_ == 64) {
        block(buffer_);
        pending_ = 0;
      }
    }
  }

  /** Appends the string's bytes as they are, with no terminator. */
  void update(const std::string& text) { update(text.data(), text.size()); }

  /** The digest, as 64 bytes. */
  void finish(uint8_t out[64]) const {
    // A one bit, zeroes to 32 bytes short of a block, then the length in
    // bits as a 256-bit big-endian number. A byte count in 64 bits is a bit
    // count of 67, so the top 189 bits are always zero.
    Whirlpool copy = *this;
    const uint64_t bytes = copy.length_;
    const uint8_t one = 0x80;
    copy.update(&one, 1);
    const uint8_t zero = 0;
    while (copy.pending_ != 32) copy.update(&zero, 1);
    uint8_t tail[32] = {0};
    tail[23] = (uint8_t) (bytes >> 61);
    const uint64_t bitsLow = bytes << 3;
    for (int i = 0; i < 8; ++i) tail[24 + i] = (uint8_t) ((bitsLow >> (56 - i * 8)) & 0xff);
    copy.update(tail, 32);
    for (int i = 0; i < 8; ++i)
      for (int b = 0; b < 8; ++b) out[i * 8 + b] = (uint8_t) ((copy.h_[i] >> (56 - b * 8)) & 0xff);
  }

  /** The digest as 128 lowercase hex characters. */
  std::string hex() const {
    uint8_t digest[64];
    finish(digest);
    return hashdetail::toHex(digest, 64);
  }

private:
  /** The round tables, built from the specification's mini-boxes once per
   *  process (tables() holds them in a function-local static, about 16 KB).
   *
   *  Row j of the state is a 64-bit word, its byte 0 in the top bits. Table
   *  c[j][x] is the contribution of byte value x in column j to a row after
   *  the S-box (gamma) and the circulant matrix (theta); c[j] is c[0] rotated
   *  by j bytes, because each row of a circulant matrix is the one above it
   *  rotated. rc[r] is round r's constant: row 0 the next eight S-box
   *  entries, the other rows zero. */
  struct Tables {
    uint64_t c[8][256]; ///< c[column][byte value], as above
    uint64_t rc[11];    ///< rc[round], rounds 1 to 10 (rc[0] unused)
  };

  /** Multiplication in GF(2^8) modulo x^8 + x^4 + x^3 + x^2 + 1 (0x11D),
   *  the field the specification chooses. */
  static uint8_t gmul(uint8_t a, uint8_t b) {
    unsigned product = 0, x = a;
    for (; b != 0; b >>= 1) {
      if (b & 1) product ^= x;
      x <<= 1;
      if (x & 0x100) x ^= 0x11D;
    }
    return (uint8_t) product;
  }

  static Tables build() {
    // The S-box from the specification's three 4-bit mini-boxes: E, its
    // inverse, and R, arranged as a small substitution-permutation network.
    static const uint8_t e[16] = {0x1, 0xB, 0x9, 0xC, 0xD, 0x6, 0xF, 0x3,
                                  0xE, 0x8, 0x7, 0x4, 0xA, 0x2, 0x5, 0x0};
    static const uint8_t r[16] = {0x7, 0xC, 0xB, 0xD, 0xE, 0x4, 0x9, 0xF,
                                  0x6, 0x3, 0x8, 0xA, 0x2, 0x5, 0x1, 0x0};
    uint8_t eInverse[16];
    for (uint8_t u = 0; u < 16; ++u) eInverse[e[u]] = u;
    uint8_t sbox[256];
    for (unsigned x = 0; x < 256; ++x) {
      const uint8_t hi = e[x >> 4];
      const uint8_t lo = eInverse[x & 0xF];
      const uint8_t mixed = r[hi ^ lo];
      sbox[x] = (uint8_t) ((e[hi ^ mixed] << 4) | eInverse[lo ^ mixed]);
    }

    // The first row of the circulant matrix C = cir(1, 1, 4, 1, 8, 5, 2, 9).
    static const uint8_t row0[8] = {1, 1, 4, 1, 8, 5, 2, 9};
    Tables t;
    for (unsigned x = 0; x < 256; ++x) {
      uint64_t row = 0;
      for (int k = 0; k < 8; ++k)
        row |= (uint64_t) gmul(sbox[x], row0[k]) << (56 - 8 * k);
      t.c[0][x] = row;
      for (int j = 1; j < 8; ++j) t.c[j][x] = (row >> (8 * j)) | (row << (64 - 8 * j));
    }
    t.rc[0] = 0;
    for (int round = 1; round <= 10; ++round) {
      uint64_t rc = 0;
      for (int j = 0; j < 8; ++j) rc |= (uint64_t) sbox[8 * (round - 1) + j] << (56 - 8 * j);
      t.rc[round] = rc;
    }
    return t;
  }

  /** Built once, on first use; a function-local static, so the first two
   *  threads to hash at once cannot both build it. */
  static const Tables& tables() {
    static const Tables t = build();
    return t;
  }

  /** Row i of theta(pi(gamma(in))): column j of the output's row i comes from
   *  row (i - j) mod 8 of the input, which is the cyclic shift pi. */
  static uint64_t mixRow(const Tables& t, const uint64_t in[8], int i) {
    uint64_t out = 0;
    for (int j = 0; j < 8; ++j)
      out ^= t.c[j][(in[(i + 8 - j) & 7] >> (56 - 8 * j)) & 0xff];
    return out;
  }

  void block(const uint8_t* chunk) {
    const Tables& t = tables();
    uint64_t m[8], key[8], state[8], next[8];
    for (int i = 0; i < 8; ++i) {
      uint64_t word = 0;
      for (int b = 0; b < 8; ++b) word = (word << 8) | chunk[i * 8 + b];
      m[i] = word;
      key[i] = h_[i];
      state[i] = m[i] ^ key[i];
    }
    // The block cipher W, keyed by the chaining value: ten rounds, each
    // first advancing the key schedule (the same round function, keyed by
    // the constant) and then the state (keyed by the round key).
    for (int round = 1; round <= 10; ++round) {
      for (int i = 0; i < 8; ++i) next[i] = mixRow(t, key, i);
      next[0] ^= t.rc[round];
      for (int i = 0; i < 8; ++i) key[i] = next[i];
      for (int i = 0; i < 8; ++i) next[i] = mixRow(t, state, i) ^ key[i];
      for (int i = 0; i < 8; ++i) state[i] = next[i];
    }
    // Miyaguchi-Preneel: the cipher's output, its input and the old chaining
    // value, all three.
    for (int i = 0; i < 8; ++i) h_[i] ^= state[i] ^ m[i];
  }

  uint64_t h_[8]{};
  uint64_t length_ = 0;
  uint8_t buffer_[64]{};
  size_t pending_ = 0;
};

/** The Whirlpool of a whole buffer, as 128 lowercase hex characters. */
inline std::string whirlpoolHex(const void* data, size_t bytes) {
  Whirlpool w;
  w.update(data, bytes);
  return w.hex();
}

/** The same for a string's bytes. */
inline std::string whirlpoolHex(const std::string& text) {
  return whirlpoolHex(text.data(), text.size());
}

/** A file, streamed. Empty when it cannot be read -- see streamFile. */
inline std::string whirlpoolFile(const char* path) {
  return hashdetail::streamFile<Whirlpool>(path);
}

} // namespace hash
} // namespace sonore
