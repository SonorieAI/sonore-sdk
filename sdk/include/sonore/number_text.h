// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: numbers in text, whatever the C locale says.
//
// A plugin does not own its process. Hosts call setlocale() -- a German or a
// Spanish DAW sets LC_NUMERIC to a comma locale -- and from then on every
// printf("%g") in the plugin writes "0,5" and every strtod reads "0.5" as 0.
// In a JSON settings file that turns [0.5] into [0,5], two numbers; in an SVG
// it truncates every coordinate at the point; in a saved state it puts a
// knob back at zero. Nobody sees it on the developer's machine.
//
// The fix is the one every portable parser uses: format with the C library,
// which rounds correctly, then turn the locale's decimal point into '.'; and
// parse by turning '.' into the locale's decimal point first. The library
// does the arithmetic -- exactly, which a hand-written digit loop does not --
// and only the one character that varies is translated.
#pragma once

#include <clocale>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace sonore {
namespace numtext {

/** The decimal point the C library is using right now. */
inline char localeDecimalPoint() {
  const std::lconv* lc = std::localeconv();
  return (lc && lc->decimal_point && lc->decimal_point[0]) ? lc->decimal_point[0] : '.';
}

/** Format with `format` (a single floating conversion, "%.17g" by default --
 *  enough digits that the value reads back exactly) and '.' as the point. */
inline void format(char* out, size_t capacity, double value, const char* format = "%.17g") {
  if (!out || capacity == 0) return;
  std::snprintf(out, capacity, format, value);
  const char point = localeDecimalPoint();
  if (point == '.') return;
  for (char* p = out; *p; ++p)
    if (*p == point) *p = '.';
}

/** Parse a number written with '.' from the start of `text` (at most
 *  `length` characters). Returns how many characters it used, 0 if there was
 *  no number there; `*out` is untouched then. Correctly rounded, because the
 *  C library does the conversion. */
inline size_t parse(const char* text, size_t length, double* out) {
  if (!text || !out || length == 0) return 0;
  // Only the characters a number can contain are copied: the library is
  // never shown more than the number, so it cannot run on past it.
  size_t n = 0;
  while (n < length) {
    const char c = text[n];
    if ((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E')
      ++n;
    else
      break;
  }
  if (n == 0) return 0;
  const char point = localeDecimalPoint();
  char stackBuf[64];  // not `small`: rpcndr.h, via windows.h, makes that a macro for `char`
  std::string large;
  char* buffer = stackBuf;
  if (n >= sizeof(stackBuf)) {
    large.assign(text, n);
    buffer = &large[0];
  } else {
    for (size_t i = 0; i < n; ++i) stackBuf[i] = text[i];
    stackBuf[n] = '\0';
  }
  if (point != '.')
    for (size_t i = 0; i < n; ++i)
      if (buffer[i] == '.') buffer[i] = point;
  char* end = nullptr;
  const double value = std::strtod(buffer, &end);
  if (end == buffer) return 0;
  *out = value;
  return (size_t) (end - buffer);
}

/** The number `text` starts with, as parse() reads it, or `fallback` when it
 *  does not start with one. Text after the number is ignored, so "12abc"
 *  is 12 and "2.5 dB" is 2.5 -- what ApplicationProperties::getDoubleValue
 *  promises, as the reference's string-to-double does; leading spaces are
 *  not skipped. A caller that must refuse trailing text uses parse() and
 *  compares the count it returns with the length. */
inline double parseOr(const std::string& text, double fallback) {
  double value = 0.0;
  return parse(text.c_str(), text.size(), &value) > 0 ? value : fallback;
}

} // namespace numtext
} // namespace sonore
