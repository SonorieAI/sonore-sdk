// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: dependency-free offline licence verification.
//
// The Sonorie server signs a licence KEY ("SNRE1-") and a device TOKEN
// ("SNRA1-") with a 2048-bit RSA key; the plugin embeds the public key and the
// product code and verifies BOTH offline (embedded key, local .lic, no network
// on the audio or the UI path). This header provides the four pieces that
// takes -- SHA-256, base64, big-integer modular exponentiation and the blob
// layout -- from scratch, because the SDK has no framework to borrow them from.
//
// The scheme is RAW modular exponentiation, which is what the original build
// used and therefore what the server already signs. Kept deliberately: changing it would invalidate every key and token
// minted so far, and a signature made server-side has to verify here byte for
// byte (src/lib/license/rsa-core.ts, format.ts):
//
//   verify(msg) : SHA256(msg) == sig ^ e mod n
//
// where SHA256(msg) is the 32-byte digest read as a big-endian integer, e/n are
// the public exponent/modulus (hex, from the embedded "e_hex,n_hex" key), and
// sig is the hex signature carried inside the base64 licence blob.
//
// Layout MUST match src/lib/license/format.ts:
//   KEY   "SNRE1-" + base64({lid,iat,sig}),  signs  "<product>:<lid>:<emailLower>:<iat>"
//   TOKEN "SNRA1-" + base64({lid,mid,exp,sig}), signs "SNRA1:<product>:<lid>:<midLower>:<exp>"
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace sonore {
namespace license {

// ── SHA-256 ─────────────────────────────────────────────────────────────────
// A plain, well-known implementation; the digest is the 32-byte big-endian hash.
class Sha256 {
public:
  Sha256() { reset(); }
  void reset() {
    len_ = 0;
    bufLen_ = 0;
    static const uint32_t init[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                     0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    std::memcpy(h_, init, sizeof(h_));
  }
  void update(const uint8_t* data, size_t n) {
    len_ += n;
    while (n > 0) {
      size_t take = 64 - bufLen_;
      if (take > n) take = n;
      std::memcpy(buf_ + bufLen_, data, take);
      bufLen_ += take;
      data += take;
      n -= take;
      if (bufLen_ == 64) { block(buf_); bufLen_ = 0; }
    }
  }
  void update(const std::string& s) { update((const uint8_t*) s.data(), s.size()); }
  /** Finalise into 32 bytes. */
  void finish(uint8_t out[32]) {
    const uint64_t bits = len_ * 8ull;
    uint8_t pad = 0x80;
    update(&pad, 1);
    uint8_t zero = 0;
    while (bufLen_ != 56) update(&zero, 1);
    uint8_t lenBytes[8];
    for (int i = 0; i < 8; ++i) lenBytes[i] = (uint8_t) (bits >> (56 - 8 * i));
    update(lenBytes, 8);
    for (int i = 0; i < 8; ++i) {
      out[i * 4 + 0] = (uint8_t) (h_[i] >> 24);
      out[i * 4 + 1] = (uint8_t) (h_[i] >> 16);
      out[i * 4 + 2] = (uint8_t) (h_[i] >> 8);
      out[i * 4 + 3] = (uint8_t) (h_[i]);
    }
  }

private:
  static uint32_t ror(uint32_t x, int r) { return (x >> r) | (x << (32 - r)); }
  void block(const uint8_t* p) {
    static const uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
        0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
        0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
        0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
        0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
        0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
        0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
        0xc67178f2u};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
      w[i] = ((uint32_t) p[i * 4] << 24) | ((uint32_t) p[i * 4 + 1] << 16) |
             ((uint32_t) p[i * 4 + 2] << 8) | ((uint32_t) p[i * 4 + 3]);
    for (int i = 16; i < 64; ++i) {
      uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], hh = h_[7];
    for (int i = 0; i < 64; ++i) {
      uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
      uint32_t ch = (e & f) ^ (~e & g);
      uint32_t t1 = hh + S1 + ch + k[i] + w[i];
      uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
      uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      uint32_t t2 = S0 + maj;
      hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
    h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += hh;
  }
  uint32_t h_[8];
  uint8_t buf_[64];
  size_t bufLen_ = 0;
  uint64_t len_ = 0;
};

inline void sha256(const std::string& msg, uint8_t out[32]) {
  Sha256 s;
  s.update(msg);
  s.finish(out);
}

// ── Base64 decode (standard alphabet, whitespace ignored) ───────────────────
inline bool base64Decode(const std::string& in, std::string& out) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  out.clear();
  int buf = 0, bits = 0;
  for (char c : in) {
    if (c == '=' || c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
    int v = val(c);
    if (v < 0) return false;
    buf = (buf << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back((char) ((buf >> bits) & 0xff));
    }
  }
  return true;
}

// ── Big integer (base 2^32, little-endian limbs): just enough for RSA verify ──
class BigInt {
public:
  BigInt() {}
  static BigInt fromHex(const std::string& hex) {
    BigInt r;
    // Group hex digits into 32-bit limbs from the least-significant end.
    int end = (int) hex.size();
    for (int i = end; i > 0; i -= 8) {
      int start = i - 8 < 0 ? 0 : i - 8;
      uint32_t limb = 0;
      for (int j = start; j < i; ++j) {
        int d = hexDigit(hex[(size_t) j]);
        if (d < 0) { r.limbs_.clear(); return r; } // invalid → zero
        limb = (limb << 4) | (uint32_t) d;
      }
      r.limbs_.push_back(limb);
    }
    r.trim();
    return r;
  }
  static BigInt fromBytesBE(const uint8_t* b, size_t n) {
    // Big-endian bytes → integer. Build limbs from the least-significant byte.
    BigInt r;
    uint32_t limb = 0;
    int shift = 0;
    for (int i = (int) n - 1; i >= 0; --i) {
      limb |= (uint32_t) b[i] << shift;
      shift += 8;
      if (shift == 32) { r.limbs_.push_back(limb); limb = 0; shift = 0; }
    }
    if (shift > 0) r.limbs_.push_back(limb);
    r.trim();
    return r;
  }
  bool isZero() const { return limbs_.empty(); }
  bool operator==(const BigInt& o) const { return limbs_ == o.limbs_; }

  /** this = (this * o) mod m, via a full product then reduce. */
  static BigInt mulMod(const BigInt& a, const BigInt& b, const BigInt& m) {
    return mod(mul(a, b), m);
  }
  /** base ^ exp mod m (square-and-multiply; exp is small for RSA e, general for d). */
  static BigInt powMod(const BigInt& base, const BigInt& exp, const BigInt& m) {
    BigInt result = fromU32(1);
    BigInt b = mod(base, m);
    const int bits = exp.bitLength();
    for (int i = 0; i < bits; ++i) {
      if (exp.testBit(i)) result = mulMod(result, b, m);
      b = mulMod(b, b, m);
    }
    return result;
  }

private:
  static int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  }
  static BigInt fromU32(uint32_t v) {
    BigInt r;
    if (v) r.limbs_.push_back(v);
    return r;
  }
  void trim() {
    while (!limbs_.empty() && limbs_.back() == 0) limbs_.pop_back();
  }
  int bitLength() const {
    if (limbs_.empty()) return 0;
    uint32_t top = limbs_.back();
    int bits = (int) (limbs_.size() - 1) * 32;
    while (top) { bits++; top >>= 1; }
    return bits;
  }
  bool testBit(int i) const {
    size_t limb = (size_t) (i / 32);
    if (limb >= limbs_.size()) return false;
    return (limbs_[limb] >> (i % 32)) & 1u;
  }
  static BigInt mul(const BigInt& a, const BigInt& b) {
    BigInt r;
    if (a.limbs_.empty() || b.limbs_.empty()) return r;
    r.limbs_.assign(a.limbs_.size() + b.limbs_.size(), 0);
    for (size_t i = 0; i < a.limbs_.size(); ++i) {
      uint64_t carry = 0;
      const uint64_t ai = a.limbs_[i];
      for (size_t j = 0; j < b.limbs_.size(); ++j) {
        uint64_t cur = (uint64_t) r.limbs_[i + j] + ai * b.limbs_[j] + carry;
        r.limbs_[i + j] = (uint32_t) cur;
        carry = cur >> 32;
      }
      r.limbs_[i + b.limbs_.size()] += (uint32_t) carry;
    }
    r.trim();
    return r;
  }
  bool geq(const BigInt& o) const {
    if (limbs_.size() != o.limbs_.size()) return limbs_.size() > o.limbs_.size();
    for (size_t i = limbs_.size(); i-- > 0;)
      if (limbs_[i] != o.limbs_[i]) return limbs_[i] > o.limbs_[i];
    return true; // equal
  }
  void subInPlace(const BigInt& o) {
    int64_t borrow = 0;
    for (size_t i = 0; i < limbs_.size(); ++i) {
      int64_t cur = (int64_t) limbs_[i] - (i < o.limbs_.size() ? (int64_t) o.limbs_[i] : 0) - borrow;
      if (cur < 0) { cur += (int64_t) 1 << 32; borrow = 1; } else borrow = 0;
      limbs_[i] = (uint32_t) cur;
    }
    trim();
  }
  void shiftLeft1SetBit(bool bit) {
    uint32_t carry = bit ? 1u : 0u;
    for (size_t i = 0; i < limbs_.size(); ++i) {
      uint32_t next = limbs_[i] >> 31;
      limbs_[i] = (limbs_[i] << 1) | carry;
      carry = next;
    }
    if (carry) limbs_.push_back(carry);
  }
  /** a mod m by binary long division: O(bits) subtractions, fine for a
   *  load-time verify (never on the audio thread). */
  static BigInt mod(const BigInt& a, const BigInt& m) {
    BigInt rem;
    if (m.limbs_.empty()) return a; // mod 0 undefined; return a rather than divide
    for (int i = a.bitLength() - 1; i >= 0; --i) {
      rem.shiftLeft1SetBit(a.testBit(i));
      if (rem.geq(m)) rem.subInPlace(m);
    }
    return rem;
  }

  std::vector<uint32_t> limbs_; // little-endian, no trailing zero limbs
};

// ── The signature check: SHA256(msg) == sig ^ e mod n ───────────────────────
/** publicKey is "e_hex,n_hex" (the embedded public-key form). */
inline bool verifySignature(const std::string& message, const std::string& sigHex,
                            const std::string& publicKey) {
  const size_t comma = publicKey.find(',');
  if (comma == std::string::npos) return false;
  const std::string eHex = publicKey.substr(0, comma);
  const std::string nHex = publicKey.substr(comma + 1);
  if (sigHex.empty()) return false;
  for (char c : sigHex)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;

  const BigInt e = BigInt::fromHex(eHex);
  const BigInt n = BigInt::fromHex(nHex);
  const BigInt sig = BigInt::fromHex(sigHex);
  if (n.isZero()) return false;

  const BigInt recovered = BigInt::powMod(sig, e, n);

  uint8_t digest[32];
  sha256(message, digest);
  const BigInt want = BigInt::fromBytesBE(digest, 32);
  return recovered == want;
}

// ── Blob decode + message reconstruction (mirrors format.ts) ────────────────
inline std::string asciiLower(const std::string& s) {
  std::string r = s;
  for (char& c : r)
    if (c >= 'A' && c <= 'Z') c = (char) (c + 32);
  return r;
}

// Pull one flat JSON field. The blobs are machine-generated and contain no
// nested objects or escaped quotes, so a scan for "key": is sufficient and safe.
inline bool jsonString(const std::string& json, const std::string& key, std::string& out) {
  const std::string needle = "\"" + key + "\"";
  size_t k = json.find(needle);
  if (k == std::string::npos) return false;
  size_t colon = json.find(':', k + needle.size());
  if (colon == std::string::npos) return false;
  size_t q1 = json.find('"', colon + 1);
  if (q1 == std::string::npos) return false;
  size_t q2 = json.find('"', q1 + 1);
  if (q2 == std::string::npos) return false;
  out = json.substr(q1 + 1, q2 - q1 - 1);
  return true;
}
inline bool jsonNumber(const std::string& json, const std::string& key, long long& out) {
  const std::string needle = "\"" + key + "\"";
  size_t k = json.find(needle);
  if (k == std::string::npos) return false;
  size_t colon = json.find(':', k + needle.size());
  if (colon == std::string::npos) return false;
  size_t i = colon + 1;
  while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
  bool neg = false;
  if (i < json.size() && json[i] == '-') { neg = true; ++i; }
  if (i >= json.size() || json[i] < '0' || json[i] > '9') return false;
  long long v = 0;
  while (i < json.size() && json[i] >= '0' && json[i] <= '9') { v = v * 10 + (json[i] - '0'); ++i; }
  out = neg ? -v : v;
  return true;
}

inline std::string stripWhitespace(const std::string& s) {
  std::string r;
  r.reserve(s.size());
  for (char c : s)
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') r.push_back(c);
  return r;
}

/** Verify a purchase KEY offline: signature over "<product>:<lid>:<emailLower>:<iat>". */
inline bool verifyKey(const std::string& key, const std::string& productCode,
                      const std::string& email, const std::string& publicKey) {
  const std::string k = stripWhitespace(key);
  if (k.rfind("SNRE1-", 0) != 0) return false;
  std::string json;
  if (!base64Decode(k.substr(6), json)) return false;
  std::string lid, sig;
  long long iat = 0;
  if (!jsonString(json, "lid", lid) || !jsonString(json, "sig", sig) ||
      !jsonNumber(json, "iat", iat))
    return false;
  const std::string message =
      productCode + ":" + lid + ":" + asciiLower(email) + ":" + std::to_string(iat);
  return verifySignature(message, sig, publicKey);
}

/** Verify a device TOKEN offline: this machine, unexpired, and a valid signature
 *  over "SNRA1:<product>:<lid>:<midLower>:<exp>". `machineId` is this device's id
 *  and `nowSec` the current unix time. */
inline bool verifyToken(const std::string& token, const std::string& productCode,
                        const std::string& machineId, long long nowSec,
                        const std::string& publicKey) {
  const std::string t = stripWhitespace(token);
  if (t.rfind("SNRA1-", 0) != 0) return false;
  std::string json;
  if (!base64Decode(t.substr(6), json)) return false;
  std::string lid, mid, sig;
  long long exp = 0;
  if (!jsonString(json, "lid", lid) || !jsonString(json, "mid", mid) ||
      !jsonString(json, "sig", sig) || !jsonNumber(json, "exp", exp))
    return false;
  if (asciiLower(mid) != asciiLower(machineId)) return false;
  if (exp <= nowSec) return false;
  const std::string message =
      "SNRA1:" + productCode + ":" + lid + ":" + asciiLower(mid) + ":" + std::to_string(exp);
  return verifySignature(message, sig, publicKey);
}

} // namespace license
} // namespace sonore
