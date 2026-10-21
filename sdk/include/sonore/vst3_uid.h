// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: a VST3 class id, between its bytes and its text.
//
// A VST3 class is named by 16 bytes, and everything a person or another
// program sees of it is 32 hex characters: a DAW's session, a .vstpreset's
// header, Steinberg's FUID::toString. On macOS and Linux the text is the bytes
// in order. On Windows the VST3 SDK is built COM-compatible, and SMTG_INLINE_UID
// stores a GUID's first three fields -- 4, 2 and 2 bytes -- little-endian, the
// way COM's GUID struct does. So a class declared with one GUID has DIFFERENT
// bytes on the two platforms and the SAME text, and the text is what a session
// carries from one to the other.
//
// Ours were the same bytes on both, which made the text differ: a session saved
// on Windows named a class a Mac build of the same plugin did not answer to.
// Every id here is now worked out in the GUID's own order and stored the way
// the VST3 SDK stores it on the platform -- the bytes vst3_c_api.h's own
// SMTG_INLINE_UID produces, which the tests compare against -- and turned into
// text through the same rule, so a .vstpreset another host wrote for a plugin
// matches it.

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

namespace sonore {
namespace vst3uid {

/** Whether this platform's VST3 SDK stores ids COM-compatible: the rule
 *  vst3_c_api.h applies, _WIN32. */
constexpr bool kComOrder =
#if defined(_WIN32)
    true;
#else
    false;
#endif

/** The three leading fields swapped between a GUID's own order and COM's.
 *  Its own inverse. */
inline void swapComFields(uint8_t b[16]) {
  std::swap(b[0], b[3]);
  std::swap(b[1], b[2]);
  std::swap(b[4], b[5]);
  std::swap(b[6], b[7]);
}

/** The bytes this platform's VST3 SDK stores for a GUID given in its own
 *  (text) order. */
inline void canonicalToNative(const uint8_t canonical[16], uint8_t native[16]) {
  for (int i = 0; i < 16; ++i) native[i] = canonical[i];
  if (kComOrder) swapComFields(native);
}

/** The GUID's own order from the bytes this platform's VST3 SDK stores. */
inline void nativeToCanonical(const uint8_t native[16], uint8_t canonical[16]) {
  for (int i = 0; i < 16; ++i) canonical[i] = native[i];
  if (kComOrder) swapComFields(canonical);
}

/** FUID::toString: 32 upper-case hex digits of the GUID's own order -- the
 *  same text for the same class on every platform. */
inline std::string toText(const void* nativeBytes) {
  uint8_t c[16];
  nativeToCanonical(static_cast<const uint8_t*>(nativeBytes), c);
  char hex[33] = {};
  for (int b = 0; b < 16; ++b) std::snprintf(hex + b * 2, 3, "%02X", (unsigned) c[b]);
  return std::string(hex, 32);
}

} // namespace vst3uid
} // namespace sonore
