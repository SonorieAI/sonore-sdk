// SPDX-License-Identifier: Apache-2.0
// Mutation fuzzing for the text and keys that bigint.h and crypto.h read.
//
// What arrives from outside: a number written in some base (a key file, a
// config value, a field in a JSON document), an RSAKey string, a Blowfish key
// and a buffer that may or may not be ciphertext. Each target starts from
// valid seeds and mutates them -- flips, inserted characters from the
// alphabet that confuses a number parser, truncation, repetition -- and every
// case must come back. Beyond "no crash, no hang, and under ASan no report",
// each has a property that is checked, not just survived:
//
//   bigint-parse   a string is accepted exactly when an independent
//                  canonicaliser says it is a number in that base, and then
//                  toString writes back that canonical form; refused, the
//                  value is zero
//   bigint-bytes   random bytes round-trip, minus their high zero bytes
//   bigint-arith   a == q*b + r with |r| < |b|, for operands built from the
//                  fuzzed strings and bytes, signs and all
//   blowfish       any key, any data: encrypt then decrypt is the identity;
//                  decrypting garbage is refused with the buffer untouched,
//                  or accepted with a size that fits
//   rsakey         a mutated key string is either invalid or a key whose own
//                  toString reads back as itself, and applying it does not
//                  fault
//
//   fuzz_core_crypto                     a quick pass (ctest)
//   fuzz_core_crypto --iterations 5000   the deep pass, under ASan
//   fuzz_core_crypto --target blowfish   one target
#include <sonore/bigint.h>
#include <sonore/crypto.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

using sonore::BigInteger;
using sonore::crypto::BlowFish;
using sonore::crypto::RSAKey;

namespace {

// ── The watchdog: a case that does not come back is named, not waited on ──

std::atomic<long long> g_deadlineMs{0};
std::atomic<bool> g_done{false};
const char* g_target = "";
size_t g_case = 0;
std::string g_input;

long long nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void watchdog() {
  while (!g_done.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const long long deadline = g_deadlineMs.load();
    if (deadline != 0 && nowMs() > deadline) {
      std::fprintf(stderr, "  HANG %s case=%zu input(%zu)=%.200s\n", g_target, g_case, g_input.size(),
                   g_input.c_str());
      std::fflush(stderr);
      std::_Exit(99);
    }
  }
}

void arm(const char* target, size_t index, const std::string& input) {
  g_target = target;
  g_case = index;
  g_input = input;
  // Generous: an ASan build is several times slower, and the slowest case
  // here (a 512-bit key applied to a value several moduli long) is a few
  // hundred milliseconds there.
  g_deadlineMs.store(nowMs() + 20000);
}

int g_failures = 0;
void fail(const char* what) {
  if (g_failures < 20)
    std::printf("  FAIL %s case=%zu: %s\n       input(%zu)=%.160s\n", g_target, g_case, what, g_input.size(),
                g_input.c_str());
  ++g_failures;
}

std::mt19937_64 rng(0xC0FFEE);

size_t below(size_t n) { return n == 0 ? 0 : (size_t) (rng() % n); }

// ── Mutation ──────────────────────────────────────────────────────────────

// The characters that matter to a number parser: every digit of every base,
// both cases, the signs, white space of each kind, the 'x' of a "0x", a
// comma (an RSAKey separator), a NUL and a byte that is not ASCII.
const char kAlphabet[] = "0123456789abcdefABCDEFgGxX+- \t\n\r\v\f,.;_\x80\xff";

std::string mutate(const std::string& seed) {
  std::string s = seed;
  const size_t edits = 1 + below(6);
  for (size_t e = 0; e < edits; ++e) {
    switch (below(7)) {
    case 0: // flip one byte to anything
      if (!s.empty()) s[below(s.size())] = (char) (rng() & 0xff);
      break;
    case 1: // replace with an interesting character
      if (!s.empty()) s[below(s.size())] = kAlphabet[below(sizeof(kAlphabet) - 1)];
      break;
    case 2: // insert one
      s.insert(s.begin() + (std::ptrdiff_t) below(s.size() + 1), kAlphabet[below(sizeof(kAlphabet) - 1)]);
      break;
    case 3: // delete one
      if (!s.empty()) s.erase(below(s.size()), 1);
      break;
    case 4: // truncate
      s.resize(below(s.size() + 1));
      break;
    case 5: // repeat a slice: long numbers
      if (!s.empty() && s.size() < 2000) {
        const size_t from = below(s.size()), len = 1 + below(s.size() - from);
        s.insert(below(s.size() + 1), s.substr(from, len));
      }
      break;
    default: // an embedded NUL, which a C-string parser would stop at
      s.insert(s.begin() + (std::ptrdiff_t) below(s.size() + 1), '\0');
      break;
    }
  }
  if (s.size() > 3000) s.resize(3000);
  return s;
}

// ── An independent canonicaliser: what parseString SHOULD accept ──────────

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

int digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'z') return c - 'a' + 10;
  if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
  return 99;
}

/** The canonical spelling -- sign only when negative, no leading zeros,
 *  lower case -- or false if the text is not a number in that base. */
bool canonical(const std::string& text, int base, std::string& out) {
  if (base != 2 && base != 8 && base != 10 && base != 16) return false;
  size_t b = 0, e = text.size();
  while (b < e && isSpace(text[b])) ++b;
  while (e > b && isSpace(text[e - 1])) --e;
  bool negative = false;
  if (b < e && (text[b] == '+' || text[b] == '-')) negative = text[b++] == '-';
  if (b == e) return false;
  std::string digits;
  for (size_t i = b; i < e; ++i) {
    const int d = digit(text[i]);
    if (d >= base) return false;
    digits += "0123456789abcdef"[d];
  }
  const size_t first = digits.find_first_not_of('0');
  if (first == std::string::npos) {
    out = "0";
    return true;
  }
  out = (negative ? "-" : "") + digits.substr(first);
  return true;
}

// ── Targets ───────────────────────────────────────────────────────────────

std::vector<std::string> numberSeeds() {
  return {"0", "-0", "+1", "  42  ", "-1234567890123456789012345678901234567890", "ffffffffffffffffffffffffffffffff",
          "DeadBeefCafeBabe", "101010101010101010101010101010101010101010101010101010101010101011",
          "7777777777777777777777", "\t-0000000000123\n", "18446744073709551616", "-9223372036854775808",
          "1", "z", "", "0x1f", "1e10", "12,34"};
}

void fuzzParse(size_t iterations) {
  const std::vector<std::string> seeds = numberSeeds();
  const int bases[] = {2, 8, 10, 16, 0, 7, 36, -16};
  size_t accepted = 0;
  for (size_t i = 0; i < iterations; ++i) {
    const std::string text = mutate(seeds[below(seeds.size())]);
    arm("bigint-parse", i, text);
    for (int base : bases) {
      BigInteger x(12345);
      const bool ok = x.parseString(text, base);
      std::string want;
      const bool should = canonical(text, base, want);
      if (ok != should) {
        fail(ok ? "accepted text the canonicaliser refuses" : "refused a well-formed number");
        continue;
      }
      if (!ok) {
        if (!x.isZero()) fail("a refused string left a non-zero value");
        continue;
      }
      ++accepted;
      if (x.toString(base) != want) fail("toString is not the canonical spelling of what was read");
      BigInteger y;
      if (!y.parseString(x.toString(base), base) || y != x) fail("the canonical spelling does not read back");
      // And through another base, so a parse that is wrong in the same way
      // as the printer cannot hide.
      const int other = base == 10 ? 16 : 10;
      BigInteger z;
      if (!z.parseString(x.toString(other), other) || z != x) fail("a different base disagrees");
    }
  }
  std::printf("  bigint-parse   %zu cases, %zu (string, base) pairs accepted\n", iterations, accepted);
}

void fuzzBytes(size_t iterations) {
  for (size_t i = 0; i < iterations; ++i) {
    std::vector<uint8_t> bytes(below(300));
    for (uint8_t& b : bytes) b = (uint8_t) (below(4) == 0 ? 0 : rng());
    arm("bigint-bytes", i, std::string(bytes.begin(), bytes.end()));
    BigInteger x;
    x.loadFromMemoryBlock(bytes);
    std::vector<uint8_t> trimmed = bytes;
    while (!trimmed.empty() && trimmed.back() == 0) trimmed.pop_back();
    if (x.toMemoryBlock() != trimmed) fail("bytes do not round-trip");
    if (x.isNegative()) fail("bytes made a negative number");
    int highest = -1;
    if (!trimmed.empty()) {
      highest = (int) (trimmed.size() * 8 - 1);
      for (uint8_t top = trimmed.back(); (top & 0x80) == 0; top = (uint8_t) (top << 1)) --highest;
    }
    if (x.getHighestBit() != highest) fail("getHighestBit disagrees with the bytes");
  }
  std::printf("  bigint-bytes   %zu cases\n", iterations);
}

BigInteger fuzzedNumber() {
  // Half from text, half from bytes, with a sign either way.
  BigInteger x;
  if (below(2) == 0) {
    const std::vector<std::string> seeds = numberSeeds();
    if (!x.parseString(mutate(seeds[below(seeds.size())]), 16)) x = (int64_t) rng();
  } else {
    std::vector<uint8_t> bytes(below(160));
    for (uint8_t& b : bytes) b = (uint8_t) rng();
    x.loadFromMemoryBlock(bytes);
  }
  if (below(2) == 0) x.negate();
  return x;
}

void fuzzArith(size_t iterations) {
  for (size_t i = 0; i < iterations; ++i) {
    const BigInteger a = fuzzedNumber(), b = fuzzedNumber();
    arm("bigint-arith", i, a.toString(16) + " / " + b.toString(16));
    BigInteger q(a), r;
    q.divideBy(b, r);
    if (b.isZero()) {
      if (!q.isZero() || !r.isZero()) fail("division by zero is not zero and zero");
      continue;
    }
    if (q * b + r != a) fail("a != q*b + r");
    if (r.compareAbsolute(b) >= 0) fail("|r| >= |b|");
    if (!r.isZero() && r.isNegative() != a.isNegative()) fail("the remainder's sign is not the dividend's");
    if ((a + b) - b != a || (a * b) / b != a) fail("(a+b)-b or (a*b)/b is not a");
    const BigInteger g = a.findGreatestCommonDivisor(b);
    if (g.isZero() || !(a % g).isZero() || !(b % g).isZero()) fail("the gcd does not divide both");
    // Modular arithmetic with a fuzzed modulus, kept to 256 bits so the
    // exponentiation stays cheap under a sanitizer.
    BigInteger m = b.getBitRange(0, 256);
    if (m > 1) {
      BigInteger inv(a);
      inv.inverseModulo(m);
      if (!inv.isZero()) {
        BigInteger one = a * inv;
        one %= m;
        if (one.isNegative()) one += m;
        if (!one.isOne()) fail("a * inverse is not 1 mod m");
      } else if (a.findGreatestCommonDivisor(m).isOne()) {
        fail("no inverse reported for a value coprime to m");
      }
      BigInteger p(a);
      p.exponentModulo(a.getBitRange(0, 256), m);
      if (p.isNegative() || p >= m) fail("exponentModulo out of [0, m)");
    }
  }
  std::printf("  bigint-arith   %zu cases\n", iterations);
}

void fuzzBlowfish(size_t iterations) {
  size_t refused = 0;
  for (size_t i = 0; i < iterations; ++i) {
    std::vector<uint8_t> key(below(90)), data(below(140));
    for (uint8_t& b : key) b = (uint8_t) rng();
    for (uint8_t& b : data) b = (uint8_t) rng();
    arm("blowfish", i, std::string(key.begin(), key.end()));
    const BlowFish bf(key.empty() ? nullptr : key.data(), key.size());

    std::vector<uint8_t> sealed = data;
    bf.encrypt(sealed);
    if (sealed.size() != (data.size() / 8 + 1) * 8) fail("the padded size is wrong");
    std::vector<uint8_t> opened = sealed;
    if (!bf.decrypt(opened) || opened != data) fail("encrypt then decrypt is not the identity");

    // Garbage, of any length, through both forms.
    std::vector<uint8_t> garbage(below(70));
    for (uint8_t& b : garbage) b = (uint8_t) rng();
    const std::vector<uint8_t> before = garbage;
    if (bf.decrypt(garbage)) {
      if (garbage.size() > before.size() || before.size() - garbage.size() > 8)
        fail("accepted garbage came back with an impossible size");
    } else {
      ++refused;
      if (garbage != before) fail("refused garbage was changed");
    }
    std::vector<uint8_t> raw = before;
    raw.resize(raw.size() + 8);
    const size_t claimed = below(raw.size() + 1);
    const int got = bf.decrypt(raw.data(), claimed);
    if (got > (int) claimed) fail("the raw form returned more than it was given");
    const int grown = bf.encrypt(raw.data(), claimed, raw.size());
    if (grown != -1 && (grown < (int) claimed || grown > (int) claimed + 8)) fail("the raw encrypt size is wrong");
  }
  std::printf("  blowfish       %zu cases, %zu garbage buffers refused\n", iterations, refused);
}

void fuzzRsa(size_t iterations) {
  // Seeds: real keys of a few sizes, in the reference's text form.
  std::vector<std::string> seeds;
  for (int bits : {32, 64, 128, 256}) {
    RSAKey pub, priv;
    const int seed[] = {bits, 7, 11};
    RSAKey::createKeyPair(pub, priv, bits, seed, 3);
    seeds.push_back(pub.toString());
    seeds.push_back(priv.toString());
  }
  seeds.push_back("10001,d3e24d84cf40afb72b6a4553f59b461f1f72a2368fa973479842b549b8153e9a");
  seeds.push_back("3,21");
  seeds.push_back(",");
  size_t valid = 0;
  for (size_t i = 0; i < iterations; ++i) {
    const std::string text = mutate(seeds[below(seeds.size())]);
    arm("rsakey", i, text);
    const RSAKey key(text);
    if (!(key == RSAKey::fromString(text))) fail("the constructor and fromString disagree");
    if (!key.isValid()) continue;
    ++valid;
    if (!(RSAKey::fromString(key.toString()) == key)) fail("a valid key's toString does not read back as itself");
    // Apply it, if it is small enough to be cheap: a mutated string can make
    // a 12,000-bit exponent, which is a slow key, not a broken one.
    if (key.exponent().getHighestBit() < 600 && key.modulus().getHighestBit() < 600) {
      BigInteger value = fuzzedNumber().getBitRange(0, 700);
      if (value.isZero()) value = 1;
      const BigInteger original(value);
      if (!key.applyToValue(value)) fail("a valid key refused a positive value");
      if (value.isNegative()) fail("applying a key made a negative number");
      BigInteger negative = -original;
      if (key.applyToValue(negative) || negative != -original) fail("a negative value was not refused untouched");
    }
  }
  std::printf("  rsakey         %zu cases, %zu parsed as valid keys\n", iterations, valid);
}

} // namespace

int main(int argc, char** argv) {
  size_t iterations = 1500; // the ctest pass: seconds
  std::string only;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--iterations" && i + 1 < argc) iterations = (size_t) std::atol(argv[++i]);
    else if (a == "--target" && i + 1 < argc) only = argv[++i];
    else {
      std::fprintf(stderr, "usage: fuzz_core_crypto [--iterations N] [--target name]\n");
      return 2;
    }
  }
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::thread dog(watchdog);
  dog.detach();

  std::printf("fuzz_core_crypto: %zu cases per target\n", iterations);
  struct Target {
    const char* name;
    void (*run)(size_t);
  };
  const Target targets[] = {{"bigint-parse", fuzzParse},
                            {"bigint-bytes", fuzzBytes},
                            {"bigint-arith", fuzzArith},
                            {"blowfish", fuzzBlowfish},
                            {"rsakey", fuzzRsa}};
  size_t total = 0;
  for (const Target& t : targets) {
    if (!only.empty() && only != t.name) continue;
    t.run(iterations);
    total += iterations;
  }
  g_deadlineMs.store(0);
  g_done.store(true);
  std::printf("\n%zu cases, %d failure(s)\n%s\n", total, g_failures,
              g_failures == 0 ? "FUZZ CORE CRYPTO PASSED" : "FUZZ CORE CRYPTO FAILED");
  return g_failures == 0 ? 0 : 1;
}
