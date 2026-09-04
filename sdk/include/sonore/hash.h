// SPDX-License-Identifier: Apache-2.0
//
// SHA-256.
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
// SHA-256 and nothing else. A framework ships MD5, RSA and Blowfish too; a
// cipher nobody in this SDK calls is a cipher nobody has tested, and an
// untested cipher is worse than an absent one because it looks like security.
//
// NOT for passwords. A bare hash is the wrong tool for those and this one has
// no salt and no work factor -- if that job ever appears here it wants a
// purpose-built KDF, not this file with a loop around it.
//
// Written from FIPS 180-4, which is a specification of arithmetic. The
// constants below are the first thirty-two bits of the fractional parts of
// cube roots of primes; they are derived, not copied.
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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
  Sha256() { reset(); }

  void reset() {
    // The initial state: fractional parts of the square roots of the first
    // eight primes.
    h_[0] = 0x6a09e667u; h_[1] = 0xbb67ae85u; h_[2] = 0x3c6ef372u; h_[3] = 0xa54ff53au;
    h_[4] = 0x510e527fu; h_[5] = 0x9b05688cu; h_[6] = 0x1f83d9abu; h_[7] = 0x5be0cd19u;
    length_ = 0;
    pending_ = 0;
  }

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

/** The whole of a buffer, in one call. */
inline std::string sha256Hex(const void* data, size_t bytes) {
  Sha256 s;
  s.update(data, bytes);
  return s.hex();
}

inline std::string sha256Hex(const std::string& text) {
  return sha256Hex(text.data(), text.size());
}

/**
 * A file, streamed.
 *
 * Returns an empty string for a file that cannot be read, and does NOT throw:
 * a caller hashing a sample that has since been unplugged wants to say so in
 * its interface, not to unwind.
 */
inline std::string sha256File(const char* path) {
  FILE* f = std::fopen(path, "rb");
  if (!f) return std::string();
  Sha256 s;
  std::vector<unsigned char> chunk(64 * 1024);
  for (;;) {
    const size_t got = std::fread(chunk.data(), 1, chunk.size(), f);
    if (got == 0) break;
    s.update(chunk.data(), got);
  }
  std::fclose(f);
  return s.hex();
}

} // namespace hash
} // namespace sonore
