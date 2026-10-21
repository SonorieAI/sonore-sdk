// SPDX-License-Identifier: Apache-2.0
//
// BigInteger: signed integers of any size.
//
// ── What it is for ──────────────────────────────────────────────────────────
//
// RSAKey and Primes in crypto.h are built on it, and those are the reason it
// exists: a key pair is two numbers hundreds of digits long, and the only
// operation that matters on them is "this to the power of that, modulo the
// other", which does not fit in any machine word. Beyond that it is the
// reference's arbitrary-precision integer -- construction from machine
// integers, text in bases 2, 8, 10 and 16, and little-endian bytes; the four
// arithmetic operations; gcd and modular inverse; bit access -- so code
// written against that one has the same names to reach for here.
//
// ── Not the licence check's integer ─────────────────────────────────────────
//
// license.h carries its own minimal integer for its one modular
// exponentiation. That stays separate on purpose: what a licence check
// accepts must not move because somebody made this class faster. The tests
// cross-check the two on random inputs, which is the only way they are
// allowed to be related.
//
// ── Representation ──────────────────────────────────────────────────────────
//
// Sign and magnitude. The magnitude is base-2^32 limbs, least significant
// first, with no zero limb at the top; zero is an empty vector and is never
// negative, so there is one representation of every value and == can compare
// the vectors directly. 32-bit limbs with 64-bit products, because every
// compiler this SDK targets multiplies 32x32->64 natively -- the 64-bit limb
// alternative needs __int128 on one compiler and _umul128 on another, and
// neither exists on all of them.
//
// ── Semantics that follow the reference rather than C++ ─────────────────────
//
//   - Division truncates toward zero and the remainder takes the dividend's
//     sign. That much IS C++'s rule for int.
//   - Division by zero gives a zero quotient and a zero remainder instead of
//     trapping. A caller that might divide by zero has to check first, as it
//     would for any other answer it cannot use.
//   - The bit operations -- & | ^, the shifts, setBit and friends -- act on the
//     MAGNITUDE and keep the left operand's sign. So -5 >> 1 is -2, not -3,
//     and -6 & 3 is -2. This is not two's complement: there is no infinite
//     run of ones to the left of a negative number here.
//   - toMemoryBlock is the magnitude, little-endian, in the fewest bytes that
//     hold it; the sign is not written.
//
// ── Cost ────────────────────────────────────────────────────────────────────
//
// Schoolbook multiplication and Knuth's Algorithm D for division: both
// quadratic. At RSA sizes -- 16 to 64 limbs -- that is close to the best
// there is; Karatsuba's crossover sits in the tens of limbs, so it would
// earn a modest fraction at the top of that range and cost a second
// multiplier to keep correct. A 1024-bit key pair is generated in well under
// a second. exponentModulo normalises the modulus once and reduces into
// reused buffers, so its loop does not allocate after the first square.
//
// NOT constant-time: how long exponentModulo takes depends on the bits of the
// exponent. Signing a licence file on a build machine is fine; a server that
// decrypts whatever it is sent, and lets the sender time it, is not.
//
// Allocates. Never on the audio thread.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace sonore {
namespace bigintdetail {

using Limbs = std::vector<uint32_t>;

inline void trim(Limbs& a) {
  while (!a.empty() && a.back() == 0) a.pop_back();
}

inline int compareMagnitude(const Limbs& a, const Limbs& b) {
  if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
  for (size_t i = a.size(); i-- > 0;)
    if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
  return 0;
}

/** a += b. a and b must be different vectors. */
inline void addMagnitude(Limbs& a, const Limbs& b) {
  if (a.size() < b.size()) a.resize(b.size(), 0u);
  uint64_t carry = 0;
  size_t i = 0;
  for (; i < b.size(); ++i) {
    const uint64_t t = (uint64_t) a[i] + b[i] + carry;
    a[i] = (uint32_t) t;
    carry = t >> 32;
  }
  for (; carry != 0 && i < a.size(); ++i) {
    const uint64_t t = (uint64_t) a[i] + carry;
    a[i] = (uint32_t) t;
    carry = t >> 32;
  }
  if (carry != 0) a.push_back((uint32_t) carry);
}

/** a -= b, where a >= b. a and b must be different vectors. */
inline void subtractMagnitude(Limbs& a, const Limbs& b) {
  uint32_t borrow = 0;
  size_t i = 0;
  for (; i < b.size(); ++i) {
    const uint64_t take = (uint64_t) b[i] + borrow;
    const uint64_t have = a[i];
    borrow = have < take ? 1u : 0u;
    a[i] = (uint32_t) (have - take);
  }
  for (; borrow != 0 && i < a.size(); ++i) {
    borrow = a[i] == 0 ? 1u : 0u;
    a[i] -= 1u;
  }
  trim(a);
}

/** out = a * b. out must not be a or b; its capacity is reused, which is
 *  what keeps the exponentiation loop from allocating. */
inline void multiplyMagnitude(const Limbs& a, const Limbs& b, Limbs& out) {
  if (a.empty() || b.empty()) {
    out.clear();
    return;
  }
  out.assign(a.size() + b.size(), 0u);
  for (size_t i = 0; i < a.size(); ++i) {
    const uint64_t ai = a[i];
    if (ai == 0) continue;
    uint64_t carry = 0;
    for (size_t j = 0; j < b.size(); ++j) {
      // (2^32-1)^2 + 2(2^32-1) is exactly 2^64-1: the sum cannot overflow.
      const uint64_t t = ai * b[j] + out[i + j] + carry;
      out[i + j] = (uint32_t) t;
      carry = t >> 32;
    }
    out[i + b.size()] = (uint32_t) carry;
  }
  trim(out);
}

/** a = a * factor + addend. */
inline void multiplySmallAdd(Limbs& a, uint32_t factor, uint32_t addend) {
  uint64_t carry = addend;
  for (uint32_t& limb : a) {
    const uint64_t t = (uint64_t) limb * factor + carry;
    limb = (uint32_t) t;
    carry = t >> 32;
  }
  if (carry != 0) a.push_back((uint32_t) carry);
  trim(a);
}

/** a /= divisor, returning the remainder. divisor must not be zero. */
inline uint32_t divideSmall(Limbs& a, uint32_t divisor) {
  uint64_t rem = 0;
  for (size_t i = a.size(); i-- > 0;) {
    const uint64_t cur = (rem << 32) | a[i];
    a[i] = (uint32_t) (cur / divisor);
    rem = cur % divisor;
  }
  trim(a);
  return (uint32_t) rem;
}

/** a mod divisor, leaving a alone. */
inline uint32_t remainderSmall(const Limbs& a, uint32_t divisor) {
  uint64_t rem = 0;
  for (size_t i = a.size(); i-- > 0;) rem = ((rem << 32) | a[i]) % divisor;
  return (uint32_t) rem;
}

inline void shiftLeft(Limbs& a, size_t bits) {
  if (a.empty() || bits == 0) return;
  const size_t whole = bits / 32;
  const unsigned part = (unsigned) (bits % 32);
  if (part != 0) {
    a.push_back(0u);
    for (size_t i = a.size() - 1; i > 0; --i) a[i] = (a[i] << part) | (a[i - 1] >> (32 - part));
    a[0] <<= part;
  }
  if (whole != 0) a.insert(a.begin(), whole, 0u);
  trim(a);
}

inline void shiftRight(Limbs& a, size_t bits) {
  const size_t whole = bits / 32;
  const unsigned part = (unsigned) (bits % 32);
  if (whole >= a.size()) {
    a.clear();
    return;
  }
  if (whole != 0) a.erase(a.begin(), a.begin() + (std::ptrdiff_t) whole);
  if (part != 0) {
    for (size_t i = 0; i + 1 < a.size(); ++i) a[i] = (a[i] >> part) | (a[i + 1] << (32 - part));
    a.back() >>= part;
  }
  trim(a);
}

inline unsigned leadingZeros(uint32_t x) {
  if (x == 0) return 32;
  unsigned n = 0;
  while ((x & 0x80000000u) == 0) {
    x <<= 1;
    ++n;
  }
  return n;
}

/**
 * Knuth's Algorithm D (TAOCP vol. 2, 4.3.1) on a dividend and divisor that
 * are already normalised -- the divisor's top bit set, the dividend shifted by
 * the same amount and given one extra limb at the top. On return `un` holds
 * the normalised remainder in its low n limbs; the quotient goes to
 * `quotient` (m+1 limbs, where the dividend had m+n) unless that is null.
 *
 * Every quantity is unsigned. The textbook version subtracts in signed 64-bit
 * and shifts negative numbers right, which is implementation-defined before
 * C++20; here a borrow is a separate bit and nothing is ever negative.
 */
inline void divideNormalised(uint32_t* un, size_t unSize, const uint32_t* vn, size_t n,
                             uint32_t* quotient) {
  const size_t m = unSize - 1 - n;
  const uint64_t top = vn[n - 1];
  const uint64_t next = vn[n - 2];
  for (size_t j = m + 1; j-- > 0;) {
    // Estimate the quotient digit from the top two limbs, then correct it
    // with the third: after this it is right or one too big.
    const uint64_t num = ((uint64_t) un[j + n] << 32) | un[j + n - 1];
    uint64_t qhat = num / top;
    uint64_t rhat = num % top;
    while (qhat > 0xffffffffull || qhat * next > ((rhat << 32) | un[j + n - 2])) {
      --qhat;
      rhat += top;
      if (rhat > 0xffffffffull) break;
    }

    // Multiply and subtract.
    uint64_t carry = 0;
    uint32_t borrow = 0;
    for (size_t i = 0; i < n; ++i) {
      const uint64_t p = qhat * vn[i] + carry;
      carry = p >> 32;
      const uint64_t take = (uint64_t) (uint32_t) p + borrow;
      const uint64_t have = un[i + j];
      borrow = have < take ? 1u : 0u;
      un[i + j] = (uint32_t) (have - take);
    }
    const uint64_t take = carry + borrow;
    const uint64_t have = un[j + n];
    un[j + n] = (uint32_t) (have - take);

    // One too big: the subtraction went negative. Add the divisor back once;
    // the carry out of the top limb cancels the borrow and is dropped.
    if (have < take) {
      --qhat;
      uint64_t c = 0;
      for (size_t i = 0; i < n; ++i) {
        const uint64_t t = (uint64_t) un[i + j] + vn[i] + c;
        un[i + j] = (uint32_t) t;
        c = t >> 32;
      }
      un[j + n] = (uint32_t) (un[j + n] + c);
    }
    if (quotient) quotient[j] = (uint32_t) qhat;
  }
}

/** Shift `v` left by `s` bits into `out`, which gets `extra` more limbs. */
inline void normalise(const Limbs& v, unsigned s, size_t extra, Limbs& out) {
  out.assign(v.size() + extra, 0u);
  for (size_t i = v.size(); i-- > 0;) {
    out[i] |= v[i] << s;
    // The divisor gets no extra limb, and needs none: s is its leading
    // zeros, so nothing shifts out of its top.
    if (s != 0 && i + 1 < out.size()) out[i + 1] |= v[i] >> (32 - s);
  }
}

/** q = u / v and r = u % v, for magnitudes. v must not be zero. Either
 *  output may be null; neither may be u or v. */
inline void divideMagnitude(const Limbs& u, const Limbs& v, Limbs* q, Limbs* r) {
  if (compareMagnitude(u, v) < 0) {
    if (q) q->clear();
    if (r) *r = u;
    return;
  }
  if (v.size() == 1) {
    Limbs quotient = u;
    const uint32_t rem = divideSmall(quotient, v[0]);
    if (q) q->swap(quotient);
    if (r) {
      r->clear();
      if (rem != 0) r->push_back(rem);
    }
    return;
  }
  const size_t n = v.size();
  const unsigned s = leadingZeros(v.back());
  Limbs vn, un;
  normalise(v, s, 0, vn);
  normalise(u, s, 1, un);
  Limbs quotient(un.size() - n, 0u);
  divideNormalised(un.data(), un.size(), vn.data(), n, quotient.data());
  if (q) {
    trim(quotient);
    q->swap(quotient);
  }
  if (r) {
    r->assign(n, 0u);
    for (size_t i = 0; i < n; ++i)
      (*r)[i] = (un[i] >> s) | (s != 0 ? un[i + 1] << (32 - s) : 0u);
    trim(*r);
  }
}

/**
 * Repeated reduction by one modulus: the normalised divisor is computed once
 * and the working buffer reused, so a modular exponentiation allocates for
 * its first step and never again.
 */
class Reducer {
public:
  explicit Reducer(const Limbs& modulus) : modulus_(modulus) {
    if (modulus_.size() >= 2) {
      shift_ = leadingZeros(modulus_.back());
      normalise(modulus_, shift_, 0, vn_);
    }
  }

  /** x = x mod the modulus. */
  void reduce(Limbs& x) {
    if (compareMagnitude(x, modulus_) < 0) return;
    if (modulus_.size() == 1) {
      const uint32_t rem = remainderSmall(x, modulus_[0]);
      x.clear();
      if (rem != 0) x.push_back(rem);
      return;
    }
    const size_t n = modulus_.size();
    normalise(x, shift_, 1, un_);
    divideNormalised(un_.data(), un_.size(), vn_.data(), n, nullptr);
    x.assign(n, 0u);
    for (size_t i = 0; i < n; ++i)
      x[i] = (un_[i] >> shift_) | (shift_ != 0 ? un_[i + 1] << (32 - shift_) : 0u);
    trim(x);
  }

private:
  Limbs modulus_, vn_, un_;
  unsigned shift_ = 0;
};

inline bool isSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/** A digit's value in any base up to 16, or 99 for anything that is not one. */
inline int digitValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 99;
}

} // namespace bigintdetail

/**
 * An integer with as many bits as it needs.
 *
 * A value type: copy it, compare it, use it as an int. Machine integers
 * convert implicitly, so `n % 7 == 0` and `x + 1` read the way they would for
 * an int.
 */
class BigInteger {
public:
  /** Zero. */
  BigInteger() = default;

  /** Any machine integer but bool, exactly, the most negative value of a
   *  signed type included. Implicit, so an int can stand where a
   *  BigInteger is expected. */
  template <typename Int, typename std::enable_if<std::is_integral<Int>::value &&
                                                      !std::is_same<Int, bool>::value,
                                                  int>::type = 0>
  BigInteger(Int value) { // NOLINT: implicit on purpose, like an int
    bool negative = false;
    uint64_t magnitude = magnitudeOf(value, negative, std::is_signed<Int>());
    while (magnitude != 0) {
      limbs_.push_back((uint32_t) magnitude);
      magnitude >>= 32;
    }
    negative_ = negative && !limbs_.empty();
  }

  // ── Queries ───────────────────────────────────────────────────────────

  /** True for 0. */
  bool isZero() const noexcept { return limbs_.empty(); }
  /** True for +1 only, not -1. */
  bool isOne() const noexcept { return !negative_ && limbs_.size() == 1 && limbs_[0] == 1; }
  /** True below zero; zero itself is never negative. */
  bool isNegative() const noexcept { return negative_; }

  /** Zero stays non-negative whatever is asked: there is one zero. */
  void setNegative(bool negative) noexcept { negative_ = negative && !limbs_.empty(); }
  /** Flips the sign; zero stays zero. */
  void negate() noexcept { setNegative(!negative_); }

  /** Sets it to zero, releasing nothing: the storage is kept for reuse. */
  void clear() noexcept {
    limbs_.clear();
    negative_ = false;
  }

  /** Exchanges the two values without copying their digits. */
  void swapWith(BigInteger& other) noexcept {
    limbs_.swap(other.limbs_);
    std::swap(negative_, other.negative_);
  }

  /** The low 64 bits, with the sign applied: exact when the value fits,
   *  wrapped like an unsigned conversion when it does not. */
  int64_t toInt64() const noexcept {
    uint64_t low = 0;
    if (!limbs_.empty()) low = limbs_[0];
    if (limbs_.size() > 1) low |= (uint64_t) limbs_[1] << 32;
    if (negative_) low = 0 - low;
    // Unsigned to signed without the implementation-defined conversion.
    return low <= (uint64_t) INT64_MAX ? (int64_t) low : -(int64_t) (~low) - 1;
  }

  /** The low 32 bits with the sign applied, as toInt64 does for 64. */
  int toInteger() const noexcept {
    uint32_t low = limbs_.empty() ? 0u : limbs_[0];
    if (negative_) low = 0u - low;
    return low <= (uint32_t) INT32_MAX ? (int) low : -(int) (~low) - 1;
  }

  /** -1, 0 or 1. */
  int compare(const BigInteger& other) const noexcept {
    if (negative_ != other.negative_) return negative_ ? -1 : 1;
    const int c = bigintdetail::compareMagnitude(limbs_, other.limbs_);
    return negative_ ? -c : c;
  }

  /** Compares the magnitudes, ignoring both signs. */
  int compareAbsolute(const BigInteger& other) const noexcept {
    return bigintdetail::compareMagnitude(limbs_, other.limbs_);
  }

  // ── Bits, of the magnitude ────────────────────────────────────────────

  /** Bit `bit` of the magnitude, 0 the lowest; false for a negative index
   *  or one past the top. The sign is not a bit here. */
  bool operator[](int bit) const noexcept {
    if (bit < 0) return false;
    const size_t limb = (size_t) bit / 32;
    return limb < limbs_.size() && ((limbs_[limb] >> (bit % 32)) & 1u) != 0;
  }

  /** Sets one bit, growing the number as far as it takes. A negative index
   *  is ignored. A bit index is a size: setBit(1 << 30) allocates 128 MB. */
  BigInteger& setBit(int bit) {
    if (bit < 0) return *this;
    const size_t limb = (size_t) bit / 32;
    if (limb >= limbs_.size()) limbs_.resize(limb + 1, 0u);
    limbs_[limb] |= 1u << (bit % 32);
    return *this;
  }

  /** setBit(bit) when `value` is true, clearBit(bit) when it is false. */
  BigInteger& setBit(int bit, bool value) { return value ? setBit(bit) : clearBit(bit); }

  /** Clears one bit of the magnitude; a bit past the top, or a negative
   *  index, changes nothing. A value cleared to zero loses its sign. */
  BigInteger& clearBit(int bit) noexcept {
    if (bit < 0) return *this;
    const size_t limb = (size_t) bit / 32;
    if (limb < limbs_.size()) {
      limbs_[limb] &= ~(1u << (bit % 32));
      bigintdetail::trim(limbs_);
      if (limbs_.empty()) negative_ = false;
    }
    return *this;
  }

  /** The index of the top set bit; -1 for zero. */
  int getHighestBit() const noexcept {
    if (limbs_.empty()) return -1;
    return (int) (limbs_.size() * 32 - 1) - (int) bigintdetail::leadingZeros(limbs_.back());
  }

  /** The first set bit at or above `start`; -1 if there is none. */
  int findNextSetBit(int start) const noexcept {
    if (start < 0) start = 0;
    for (size_t limb = (size_t) start / 32; limb < limbs_.size(); ++limb) {
      uint32_t word = limbs_[limb];
      if (limb == (size_t) start / 32) word &= ~0u << (start % 32);
      if (word == 0) continue;
      int bit = 0;
      while ((word & 1u) == 0) {
        word >>= 1;
        ++bit;
      }
      return (int) (limb * 32) + bit;
    }
    return -1;
  }

  /** How many bits of the magnitude are 1; 0 for zero. */
  int countNumberOfSetBits() const noexcept {
    int total = 0;
    for (uint32_t word : limbs_)
      for (; word != 0; word &= word - 1) ++total;
    return total;
  }

  /** Up to 32 bits starting at `start`, as an unsigned number. */
  uint32_t getBitRangeAsInt(int start, int numBits) const noexcept {
    if (start < 0 || numBits <= 0) return 0;
    if (numBits > 32) numBits = 32;
    const size_t limb = (size_t) start / 32;
    uint64_t window = 0;
    if (limb < limbs_.size()) window = limbs_[limb];
    if (limb + 1 < limbs_.size()) window |= (uint64_t) limbs_[limb + 1] << 32;
    window >>= start % 32;
    return (uint32_t) (numBits == 32 ? window : window & ((1ull << numBits) - 1));
  }

  /** Bits [start, start + numBits), as a non-negative number. */
  BigInteger getBitRange(int start, int numBits) const {
    BigInteger out;
    if (start < 0 || numBits <= 0) return out;
    out.limbs_ = limbs_;
    bigintdetail::shiftRight(out.limbs_, (size_t) start);
    const size_t keep = ((size_t) numBits + 31) / 32;
    if (out.limbs_.size() > keep) out.limbs_.resize(keep);
    if (numBits % 32 != 0 && out.limbs_.size() == keep)
      out.limbs_.back() &= (1u << (numBits % 32)) - 1;
    bigintdetail::trim(out.limbs_);
    return out;
  }

  // ── Arithmetic ────────────────────────────────────────────────────────

  /** Adds `other`, which may be this same object. */
  BigInteger& operator+=(const BigInteger& other) {
    if (this == &other) {
      const BigInteger copy(other);
      return addSigned(copy.limbs_, copy.negative_);
    }
    return addSigned(other.limbs_, other.negative_);
  }

  /** Subtracts `other`, which may be this same object. */
  BigInteger& operator-=(const BigInteger& other) {
    if (this == &other) {
      clear();
      return *this;
    }
    return addSigned(other.limbs_, !other.negative_);
  }

  /** Multiplies by `other`, which may be this same object. */
  BigInteger& operator*=(const BigInteger& other) {
    bigintdetail::Limbs product;
    bigintdetail::multiplyMagnitude(limbs_, other.limbs_, product);
    const bool negative = negative_ != other.negative_;
    limbs_.swap(product);
    setNegative(negative);
    return *this;
  }

  /** Divides, truncating toward zero; dividing by zero gives zero. See
   *  divideBy(). */
  BigInteger& operator/=(const BigInteger& divisor) {
    BigInteger remainder;
    divideBy(divisor, remainder);
    return *this;
  }

  /** The remainder of divideBy(): this value's sign, and zero for a zero
   *  divisor. -7 % 3 is -1. */
  BigInteger& operator%=(const BigInteger& divisor) {
    BigInteger remainder;
    divideBy(divisor, remainder);
    swapWith(remainder);
    return *this;
  }

  /** ORs the magnitudes; this value keeps its sign. Not two's complement:
   *  -4 | 1 is -5. */
  BigInteger& operator|=(const BigInteger& other) {
    if (limbs_.size() < other.limbs_.size()) limbs_.resize(other.limbs_.size(), 0u);
    for (size_t i = 0; i < other.limbs_.size(); ++i) limbs_[i] |= other.limbs_[i];
    return *this;
  }

  /** ANDs the magnitudes; this value keeps its sign unless the result is
   *  zero. Not two's complement. */
  BigInteger& operator&=(const BigInteger& other) {
    if (limbs_.size() > other.limbs_.size()) limbs_.resize(other.limbs_.size());
    for (size_t i = 0; i < limbs_.size(); ++i) limbs_[i] &= other.limbs_[i];
    bigintdetail::trim(limbs_);
    setNegative(negative_);
    return *this;
  }

  /** XORs the magnitudes; this value keeps its sign unless the result is
   *  zero. x ^= x is zero. Not two's complement. */
  BigInteger& operator^=(const BigInteger& other) {
    if (this == &other) {
      clear();
      return *this;
    }
    if (limbs_.size() < other.limbs_.size()) limbs_.resize(other.limbs_.size(), 0u);
    for (size_t i = 0; i < other.limbs_.size(); ++i) limbs_[i] ^= other.limbs_[i];
    bigintdetail::trim(limbs_);
    setNegative(negative_);
    return *this;
  }

  /** A negative count shifts the other way. A count is a size, as for setBit. */
  BigInteger& operator<<=(int bits) {
    if (bits < 0) return bits == INT32_MIN ? (clear(), *this) : (*this >>= -bits);
    bigintdetail::shiftLeft(limbs_, (size_t) bits);
    return *this;
  }

  /** Shifts the magnitude right, dropping the low bits, so a negative value
   *  rounds toward zero: -5 >> 1 is -2, not the -3 an int gives. A negative
   *  count shifts left. */
  BigInteger& operator>>=(int bits) {
    if (bits < 0) return bits == INT32_MIN ? (clear(), *this) : (*this <<= -bits);
    bigintdetail::shiftRight(limbs_, (size_t) bits);
    setNegative(negative_);
    return *this;
  }

  ///@{
  /** Adds or subtracts one, as for an int: the prefix forms return this
   *  object, the postfix forms a copy of the value before. */
  BigInteger& operator++() { return *this += BigInteger(1); }
  BigInteger& operator--() { return *this -= BigInteger(1); }
  BigInteger operator++(int) {
    BigInteger before(*this);
    ++*this;
    return before;
  }
  BigInteger operator--(int) {
    BigInteger before(*this);
    --*this;
    return before;
  }
  ///@}

  /** The value with its sign flipped; -0 is 0. */
  BigInteger operator-() const {
    BigInteger out(*this);
    out.negate();
    return out;
  }

  friend BigInteger operator+(BigInteger a, const BigInteger& b) {
    a += b;
    return a;
  }
  friend BigInteger operator-(BigInteger a, const BigInteger& b) {
    a -= b;
    return a;
  }
  friend BigInteger operator*(BigInteger a, const BigInteger& b) {
    a *= b;
    return a;
  }
  friend BigInteger operator/(BigInteger a, const BigInteger& b) {
    a /= b;
    return a;
  }
  friend BigInteger operator%(BigInteger a, const BigInteger& b) {
    a %= b;
    return a;
  }
  friend BigInteger operator|(BigInteger a, const BigInteger& b) {
    a |= b;
    return a;
  }
  friend BigInteger operator&(BigInteger a, const BigInteger& b) {
    a &= b;
    return a;
  }
  friend BigInteger operator^(BigInteger a, const BigInteger& b) {
    a ^= b;
    return a;
  }
  friend BigInteger operator<<(BigInteger a, int bits) {
    a <<= bits;
    return a;
  }
  friend BigInteger operator>>(BigInteger a, int bits) {
    a >>= bits;
    return a;
  }

  friend bool operator==(const BigInteger& a, const BigInteger& b) noexcept {
    return a.negative_ == b.negative_ && a.limbs_ == b.limbs_;
  }
  friend bool operator!=(const BigInteger& a, const BigInteger& b) noexcept { return !(a == b); }
  friend bool operator<(const BigInteger& a, const BigInteger& b) noexcept { return a.compare(b) < 0; }
  friend bool operator<=(const BigInteger& a, const BigInteger& b) noexcept { return a.compare(b) <= 0; }
  friend bool operator>(const BigInteger& a, const BigInteger& b) noexcept { return a.compare(b) > 0; }
  friend bool operator>=(const BigInteger& a, const BigInteger& b) noexcept { return a.compare(b) >= 0; }

  /**
   * This divided by `divisor`: the quotient replaces this value and the
   * remainder goes to `remainder`. Truncating, the remainder carrying this
   * value's sign, and zero and zero for a zero divisor -- see the header.
   * Any of the three may be the same object; if this one is passed as the
   * remainder, the remainder is what it ends up holding.
   */
  void divideBy(const BigInteger& divisor, BigInteger& remainder) {
    if (divisor.isZero() || isZero()) {
      clear();
      remainder.clear();
      return;
    }
    bigintdetail::Limbs q, r;
    bigintdetail::divideMagnitude(limbs_, divisor.limbs_, &q, &r);
    const bool dividendNegative = negative_;
    const bool quotientNegative = negative_ != divisor.negative_;
    limbs_.swap(q);
    setNegative(quotientNegative);
    remainder.limbs_.swap(r);
    remainder.setNegative(dividendNegative);
  }

  /** The greatest common divisor of the two magnitudes; gcd(0, 0) is 0. */
  BigInteger findGreatestCommonDivisor(const BigInteger& other) const {
    bigintdetail::Limbs a = limbs_, b = other.limbs_, r;
    while (!b.empty()) {
      bigintdetail::divideMagnitude(a, b, nullptr, &r);
      a.swap(b);
      b.swap(r);
    }
    BigInteger out;
    out.limbs_.swap(a);
    return out;
  }

  /**
   * this = this ^ exponent mod modulus, the answer in [0, modulus).
   *
   * A negative base is reduced into range first. A negative exponent means
   * the modular inverse raised to its magnitude, and gives zero when there is
   * no inverse. A modulus below 2 gives zero: everything is congruent mod 1,
   * and mod 0 or less is not a question with an answer here.
   */
  void exponentModulo(const BigInteger& exponent, const BigInteger& modulus) {
    if (modulus.negative_ || modulus.limbs_.size() == 0 || modulus.isOne()) {
      clear();
      return;
    }
    BigInteger base(*this);
    base.reduceInto(modulus);
    if (exponent.negative_) {
      base.inverseModulo(modulus);
      if (base.isZero()) {
        clear();
        return;
      }
    }

    bigintdetail::Reducer reducer(modulus.limbs_);
    bigintdetail::Limbs result(1, 1u), scratch;
    const int top = exponent.getHighestBit();
    for (int bit = top; bit >= 0; --bit) {
      bigintdetail::multiplyMagnitude(result, result, scratch);
      reducer.reduce(scratch);
      result.swap(scratch);
      if (exponent[bit]) {
        bigintdetail::multiplyMagnitude(result, base.limbs_, scratch);
        reducer.reduce(scratch);
        result.swap(scratch);
      }
    }
    limbs_.swap(result);
    negative_ = false;
  }

  /**
   * this = the x in [1, modulus) with this * x == 1 (mod modulus), or zero
   * when there is none -- when the two share a factor, or the modulus is
   * below 2. Zero is never an inverse, so it is unambiguous as "none".
   */
  void inverseModulo(const BigInteger& modulus) {
    if (modulus.negative_ || modulus.isZero() || modulus.isOne()) {
      clear();
      return;
    }
    BigInteger a(*this);
    a.reduceInto(modulus);
    if (a.isZero()) {
      clear();
      return;
    }
    // Extended Euclid, keeping only the coefficient of a: invariant
    // oldS * a == oldR (mod modulus), and the same for s and r.
    BigInteger oldR(a), r(modulus), oldS(1), s(0);
    while (!r.isZero()) {
      BigInteger q(oldR), rem;
      q.divideBy(r, rem);
      oldR.swapWith(r);
      r.swapWith(rem);
      BigInteger nextS = oldS - q * s;
      oldS.swapWith(s);
      s.swapWith(nextS);
    }
    if (!oldR.isOne()) {
      clear();
      return;
    }
    oldS.reduceInto(modulus);
    swapWith(oldS);
  }

  // ── Text and bytes ────────────────────────────────────────────────────

  /**
   * The value in base 2, 8, 10 or 16, lower-case, with a leading '-' when
   * negative and the digits zero-padded to at least minimumNumCharacters.
   * Any other base gives an empty string -- the reference supports the same
   * four, and a string nobody can parse back is worse than none.
   */
  std::string toString(int base, int minimumNumCharacters = 1) const {
    static const char* kDigits = "0123456789abcdef";
    std::string digits; // least significant first, reversed at the end
    if (base == 2 || base == 8 || base == 16) {
      const int bitsPer = base == 2 ? 1 : (base == 8 ? 3 : 4);
      const int highest = getHighestBit();
      for (int bit = 0; bit <= highest; bit += bitsPer)
        digits.push_back(kDigits[getBitRangeAsInt(bit, bitsPer)]);
    } else if (base == 10) {
      // Nine decimal digits at a time: one division by 10^9 per limb-sized
      // step rather than one by 10 per digit.
      bigintdetail::Limbs work = limbs_;
      while (!work.empty()) {
        uint32_t chunk = bigintdetail::divideSmall(work, 1000000000u);
        if (work.empty()) {
          for (; chunk != 0; chunk /= 10) digits.push_back((char) ('0' + chunk % 10));
        } else {
          for (int k = 0; k < 9; ++k, chunk /= 10) digits.push_back((char) ('0' + chunk % 10));
        }
      }
    } else {
      return std::string();
    }
    if (digits.empty()) digits.push_back('0');
    while ((int) digits.size() < minimumNumCharacters) digits.push_back('0');
    if (negative_) digits.push_back('-');
    return std::string(digits.rbegin(), digits.rend());
  }

  /**
   * Reads text written in base 2, 8, 10 or 16: optional surrounding white
   * space, an optional '+' or '-', then one or more digits of that base
   * (either case for hex). Anything else -- an empty string, a stray
   * character, a "0x" -- is refused: the value becomes zero and this returns
   * false.
   *
   * Stricter than the reference, which skips characters it does not
   * recognise. A key read from a file with a typo in it should fail to load,
   * not load as a different key.
   *
   * Linear in the text for the power-of-two bases; quadratic for decimal,
   * as decimal conversion is everywhere that does not use FFTs.
   */
  bool parseString(const std::string& text, int base) {
    clear();
    if (base != 2 && base != 8 && base != 10 && base != 16) return false;
    size_t begin = 0, end = text.size();
    while (begin < end && bigintdetail::isSpace(text[begin])) ++begin;
    while (end > begin && bigintdetail::isSpace(text[end - 1])) --end;
    bool negative = false;
    if (begin < end && (text[begin] == '-' || text[begin] == '+')) {
      negative = text[begin] == '-';
      ++begin;
    }
    if (begin == end) return false;
    for (size_t i = begin; i < end; ++i)
      if (bigintdetail::digitValue(text[i]) >= base) return false;

    const size_t count = end - begin;
    if (base == 10) {
      size_t i = begin;
      size_t run = count % 9 == 0 ? 9 : count % 9;
      while (i < end) {
        uint32_t chunk = 0, scale = 1;
        for (size_t k = 0; k < run; ++k, ++i) {
          chunk = chunk * 10 + (uint32_t) bigintdetail::digitValue(text[i]);
          scale *= 10;
        }
        bigintdetail::multiplySmallAdd(limbs_, scale, chunk);
        run = 9;
      }
    } else {
      // Every digit is a fixed number of bits, so each one is written
      // straight to its place: no multiplication at all.
      const unsigned bitsPer = base == 2 ? 1u : (base == 8 ? 3u : 4u);
      limbs_.assign((count * bitsPer + 31) / 32, 0u);
      for (size_t k = 0; k < count; ++k) {
        const uint32_t value = (uint32_t) bigintdetail::digitValue(text[end - 1 - k]);
        const size_t pos = k * bitsPer;
        const unsigned offset = (unsigned) (pos % 32);
        limbs_[pos / 32] |= value << offset;
        if (offset + bitsPer > 32) limbs_[pos / 32 + 1] |= value >> (32 - offset);
      }
      bigintdetail::trim(limbs_);
    }
    setNegative(negative);
    return true;
  }

  /** The magnitude as little-endian bytes, as few as hold it: zero is no
   *  bytes at all. The sign is not written. */
  std::vector<uint8_t> toMemoryBlock() const {
    const size_t count = (size_t) (getHighestBit() + 8) / 8;
    std::vector<uint8_t> bytes(count);
    for (size_t i = 0; i < count; ++i) bytes[i] = (uint8_t) (limbs_[i / 4] >> (8 * (i % 4)));
    return bytes;
  }

  /** Little-endian bytes, read as a non-negative number. */
  void loadFromMemoryBlock(const void* data, size_t bytes) {
    clear();
    if (!data || bytes == 0) return;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    limbs_.assign((bytes + 3) / 4, 0u);
    for (size_t i = 0; i < bytes; ++i) limbs_[i / 4] |= (uint32_t) p[i] << (8 * (i % 4));
    bigintdetail::trim(limbs_);
  }

  /** The same, from a vector's bytes. */
  void loadFromMemoryBlock(const std::vector<uint8_t>& bytes) {
    loadFromMemoryBlock(bytes.data(), bytes.size());
  }

private:
  static uint64_t magnitudeOf(long long value, bool& negative, std::true_type) {
    negative = value < 0;
    // Through unsigned so the most negative value negates without overflow.
    return negative ? 0 - (uint64_t) value : (uint64_t) value;
  }
  static uint64_t magnitudeOf(unsigned long long value, bool& negative, std::false_type) {
    negative = false;
    return (uint64_t) value;
  }

  /** this += (otherNegative ? -1 : 1) * other. other must not be limbs_. */
  BigInteger& addSigned(const bigintdetail::Limbs& other, bool otherNegative) {
    if (negative_ == otherNegative) {
      bigintdetail::addMagnitude(limbs_, other);
    } else if (bigintdetail::compareMagnitude(limbs_, other) >= 0) {
      bigintdetail::subtractMagnitude(limbs_, other);
    } else {
      bigintdetail::Limbs difference = other;
      bigintdetail::subtractMagnitude(difference, limbs_);
      limbs_.swap(difference);
      negative_ = otherNegative;
    }
    setNegative(negative_);
    return *this;
  }

  /** this = this mod m, in [0, m), for a positive m. */
  void reduceInto(const BigInteger& m) {
    if (bigintdetail::compareMagnitude(limbs_, m.limbs_) >= 0) {
      bigintdetail::Limbs r;
      bigintdetail::divideMagnitude(limbs_, m.limbs_, nullptr, &r);
      limbs_.swap(r);
    }
    if (negative_ && !limbs_.empty()) {
      bigintdetail::Limbs r = m.limbs_;
      bigintdetail::subtractMagnitude(r, limbs_);
      limbs_.swap(r);
    }
    negative_ = false;
  }

  bigintdetail::Limbs limbs_; // little-endian, no zero limb at the top
  bool negative_ = false;     // never true for zero
};

} // namespace sonore
