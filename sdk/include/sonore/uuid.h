// SPDX-License-Identifier: Apache-2.0
//
// A unique identifier.
//
// ── What it is for ──────────────────────────────────────────────────────────
//
// A preset needs a name a user can change and an id that never does, or
// renaming a preset breaks every project that referenced it. A sample library
// needs to know that two files with the same name in different folders are
// different samples. Anything that has to say "this one, not that one" across a
// save and a reload needs one.
//
// ── Why not the SDK's own random ────────────────────────────────────────────
//
// random.h is xoshiro128+ seeded through splitmix64, and it is deliberately
// DETERMINISTIC: a plugin whose noise differed between runs could not be nulled
// against a reference render, and that is the whole reason it is written that
// way.
//
// Which makes it exactly wrong here. Two instances of a plugin, seeded
// identically because they were constructed identically, would mint identical
// "unique" ids -- and the failure is invisible until somebody has two of the
// plugin in one project and their presets start overwriting each other.
//
// ── Where the entropy comes from ────────────────────────────────────────────
//
// std::random_device is the standard answer, and on every platform this SDK
// targets it is a real entropy source. It is NOT guaranteed to be: the standard
// permits a deterministic implementation, and some older MinGW builds shipped
// one that returns the same sequence every run.
//
// So it is mixed with two things that cannot both be constant across runs: the
// high-resolution clock, and the address of a stack object, which moves with
// ASLR. If random_device is real this changes nothing; if it is not, the ids
// are still distinct. Belt and braces, on the one function where a collision is
// silent and permanent.
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>

namespace sonore {

/** 128 bits, as sixteen bytes. */
struct Uuid {
  uint8_t bytes[16] = {0};

  bool operator==(const Uuid& other) const {
    for (int i = 0; i < 16; ++i)
      if (bytes[i] != other.bytes[i]) return false;
    return true;
  }
  bool operator!=(const Uuid& other) const { return !(*this == other); }

  bool isNull() const {
    for (int i = 0; i < 16; ++i)
      if (bytes[i] != 0) return false;
    return true;
  }
};

namespace uuiddetail {

inline uint64_t mix(uint64_t x) {
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
  return x ^ (x >> 31);
}

} // namespace uuiddetail

/**
 * A fresh identifier.
 *
 * Version 4 (random), with the version and variant bits set as RFC 4122
 * requires -- not decoration: a tool reading one of these should be able to
 * tell it is a random UUID rather than a time-based one, and a library that
 * validates the variant will reject sixteen raw random bytes.
 */
inline Uuid makeUuid() {
  static std::random_device device;
  std::uniform_int_distribution<uint64_t> spread;

  // Three sources, mixed. See the header: random_device is permitted to be
  // deterministic, and a collision here is silent and permanent.
  uint64_t counter = 0;
  const uint64_t clock =
      (uint64_t) std::chrono::high_resolution_clock::now().time_since_epoch().count();
  const uint64_t address = (uint64_t) (uintptr_t) &counter;

  uint64_t high = uuiddetail::mix(((uint64_t) device() << 32) ^ device() ^ clock);
  uint64_t low = uuiddetail::mix(((uint64_t) device() << 32) ^ device() ^ address ^ (clock << 7));

  Uuid out;
  for (int i = 0; i < 8; ++i) out.bytes[i] = (uint8_t) (high >> (i * 8));
  for (int i = 0; i < 8; ++i) out.bytes[8 + i] = (uint8_t) (low >> (i * 8));

  out.bytes[6] = (uint8_t) ((out.bytes[6] & 0x0F) | 0x40); // version 4
  out.bytes[8] = (uint8_t) ((out.bytes[8] & 0x3F) | 0x80); // variant 1
  return out;
}

/** The canonical 8-4-4-4-12, lowercase. What goes in a state file. */
inline std::string uuidToString(const Uuid& id) {
  char buffer[37];
  std::snprintf(buffer, sizeof(buffer),
                "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                id.bytes[0], id.bytes[1], id.bytes[2], id.bytes[3], id.bytes[4], id.bytes[5],
                id.bytes[6], id.bytes[7], id.bytes[8], id.bytes[9], id.bytes[10], id.bytes[11],
                id.bytes[12], id.bytes[13], id.bytes[14], id.bytes[15]);
  return buffer;
}

/**
 * Back from text. False for anything that is not one.
 *
 * Hyphens are OPTIONAL and case does not matter, because an id that has been
 * through a config file, a URL or somebody's clipboard arrives in every
 * variation -- and refusing a string that is unmistakably the right id, over
 * punctuation, is a preset the user can see and the plugin will not load.
 */
inline bool uuidFromString(const std::string& text, Uuid* out) {
  if (!out) return false;
  int nibbles = 0;
  uint8_t bytes[16] = {0};
  for (char c : text) {
    if (c == '-' || c == '{' || c == '}') continue;
    int value;
    if (c >= '0' && c <= '9') value = c - '0';
    else if (c >= 'a' && c <= 'f') value = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') value = c - 'A' + 10;
    else return false;
    if (nibbles >= 32) return false;
    if ((nibbles & 1) == 0) bytes[nibbles / 2] = (uint8_t) (value << 4);
    else bytes[nibbles / 2] = (uint8_t) (bytes[nibbles / 2] | value);
    ++nibbles;
  }
  if (nibbles != 32) return false;
  for (int i = 0; i < 16; ++i) out->bytes[i] = bytes[i];
  return true;
}

} // namespace sonore
