// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: opening files by UTF-8 path, on every platform.
//
// Every path in this SDK is UTF-8: what the file dialog returns, what files.h
// lists, what a plugin saves in its state. On macOS and Linux the C library
// agrees. On Windows it does not -- fopen, remove, rename and _mkdir read their
// argument in the ANSI code page -- so a path through a folder called "José"
// named a different file, and one through "Музыка" or "音楽" named nothing at
// all: a sample that would not load, a recording that could not be written, a
// settings file silently never saved, for exactly the users whose names or
// languages are not English.
//
// These are the C library's own calls, with the path converted at the edge.
// Deliberately free of <windows.h>: wav.h and friends include this, and a
// header that writes a WAV file should not drag the Windows API in to do it.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <sys/stat.h>
#include <sys/types.h>
#if defined(_WIN32)
#include <direct.h>
#include <share.h>
#include <wchar.h>
#endif

namespace sonore {
namespace pathio {

#if defined(_WIN32)
/** UTF-8 to UTF-16, by hand so this header needs no Windows API. A malformed
 *  sequence becomes U+FFFD, which names no file -- an open that fails rather
 *  than one that quietly reaches a different file. */
inline std::wstring widen(const char* utf8) {
  std::wstring out;
  if (!utf8) return out;
  const unsigned char* p = (const unsigned char*) utf8;
  while (*p) {
    uint32_t cp = 0xFFFD;
    int extra = 0;
    uint32_t least = 0; // the smallest code point this length may carry
    if (*p < 0x80) {
      cp = *p++;
    } else if ((*p & 0xE0) == 0xC0) {
      cp = *p++ & 0x1F;
      extra = 1;
      least = 0x80;
    } else if ((*p & 0xF0) == 0xE0) {
      cp = *p++ & 0x0F;
      extra = 2;
      least = 0x800;
    } else if ((*p & 0xF8) == 0xF0) {
      cp = *p++ & 0x07;
      extra = 3;
      least = 0x10000;
    } else {
      ++p; // a stray continuation byte
    }
    bool broken = false;
    for (int i = 0; i < extra; ++i) {
      if ((*p & 0xC0) != 0x80) {
        broken = true;
        break;
      }
      cp = (cp << 6) | (*p++ & 0x3F);
    }
    // OVERLONG forms are malformed too: C0 AE is a '.' spelled in two bytes,
    // and decoding it as one turned an archive entry "C0AE C0AE C0AF evil"
    // -- which a byte-level check rightly saw as no ".." at all -- into
    // "../evil" at the moment the file was opened: a zip slip. C0 80 became
    // a NUL in the middle of a wide name, so "run.bat<C0 80>.wav" passed a
    // .wav filter and created run.bat. Every malformed form is U+FFFD.
    if (broken || cp < least || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp >= 0x10000) {
      cp -= 0x10000;
      out.push_back((wchar_t) (0xD800 + (cp >> 10)));
      out.push_back((wchar_t) (0xDC00 + (cp & 0x3FF)));
    } else {
      out.push_back((wchar_t) cp);
    }
  }
  return out;
}

/** UTF-16 to UTF-8, for names the wide API hands back. An unpaired
 *  surrogate becomes U+FFFD. */
inline std::string narrow(const wchar_t* wide) {
  std::string out;
  if (!wide) return out;
  for (const wchar_t* p = wide; *p; ++p) {
    uint32_t cp = (uint32_t) (uint16_t) *p;
    if (cp >= 0xD800 && cp <= 0xDBFF && p[1] >= 0xDC00 && p[1] <= 0xDFFF) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + ((uint32_t) (uint16_t) p[1] - 0xDC00);
      ++p;
    } else if (cp >= 0xD800 && cp <= 0xDFFF) {
      cp = 0xFFFD;
    }
    if (cp < 0x80) {
      out.push_back((char) cp);
    } else if (cp < 0x800) {
      out.push_back((char) (0xC0 | (cp >> 6)));
      out.push_back((char) (0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back((char) (0xE0 | (cp >> 12)));
      out.push_back((char) (0x80 | ((cp >> 6) & 0x3F)));
      out.push_back((char) (0x80 | (cp & 0x3F)));
    } else {
      out.push_back((char) (0xF0 | (cp >> 18)));
      out.push_back((char) (0x80 | ((cp >> 12) & 0x3F)));
      out.push_back((char) (0x80 | ((cp >> 6) & 0x3F)));
      out.push_back((char) (0x80 | (cp & 0x3F)));
    }
  }
  return out;
}

/** narrow() of a fixed-size field, such as a driver's name in a caps
 *  struct: at most `capacity` units, so a name that fills its field with no
 *  terminator stops at the field's end instead of reading on into the
 *  struct. */
inline std::string narrow(const wchar_t* wide, size_t capacity) {
  if (!wide) return std::string();
  size_t length = 0;
  while (length < capacity && wide[length]) ++length;
  return narrow(std::wstring(wide, length).c_str());
}
#endif

/** An environment variable as UTF-8, or empty when it is not set. On Windows
 *  getenv answers in the ANSI code page, so a profile folder named after its
 *  user -- `C:\Users\Олег` -- came back as question marks, and every settings
 *  file, log and licence under %APPDATA% went to a path that cannot exist. */
inline std::string environment(const char* name) {
  if (!name) return std::string();
#if defined(_WIN32)
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996) // copied into the string below before anything can change it
#endif
  const wchar_t* value = _wgetenv(widen(name).c_str());
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
  return value ? narrow(value) : std::string();
#else
  const char* value = std::getenv(name);
  return value ? std::string(value) : std::string();
#endif
}

#if defined(_WIN32)
/** A UTF-8 path as the wide API should be handed it: with the long-path
 *  prefix when it is too long for MAX_PATH. A plugin runs inside a DAW that
 *  has no long-path manifest, so even with LongPathsEnabled set every open,
 *  stat and mkdir under a deep folder failed at around 260 characters -- a
 *  sample library a few folders down a long user name is enough. The prefix
 *  turns off the API's normalising, so the path is made absolute and its
 *  separators backslashes first -- by _wfullpath, which takes long input
 *  when it is given the room, and keeps this header free of windows.h. */
inline std::wstring widenPath(const char* utf8) {
  std::wstring wide = widen(utf8);
  if (wide.size() < 240 || wide.compare(0, 4, L"\\\\?\\") == 0) return wide;
  std::wstring full(32768, L'\0');
  if (!_wfullpath(&full[0], wide.c_str(), full.size())) return wide;
  full.resize(wcslen(full.c_str()));
  if (full.compare(0, 2, L"\\\\") == 0) return L"\\\\?\\UNC\\" + full.substr(2); // a share
  if (full.size() >= 3 && full[1] == L':') return L"\\\\?\\" + full;
  return wide;
}
#endif

/** fopen, by UTF-8 path. Shared for reading and writing as fopen is: a log
 *  the user is told to send must stay deletable while the plugin runs. */
inline std::FILE* open(const char* path, const char* mode) {
  if (!path || !mode) return nullptr;
#if defined(_WIN32)
  return _wfsopen(widenPath(path).c_str(), widen(mode).c_str(), _SH_DENYNO);
#else
  return std::fopen(path, mode);
#endif
}

/** remove, by UTF-8 path. Zero on success, as remove returns. */
inline int remove(const char* path) {
  if (!path) return -1;
#if defined(_WIN32)
  return _wremove(widenPath(path).c_str());
#else
  return std::remove(path);
#endif
}

/** rename, by UTF-8 path. Fails if `to` exists on Windows, as rename does
 *  there; a caller that means to replace removes it first, or uses the
 *  platform's atomic replace. */
inline int rename(const char* from, const char* to) {
  if (!from || !to) return -1;
#if defined(_WIN32)
  return _wrename(widenPath(from).c_str(), widenPath(to).c_str());
#else
  return std::rename(from, to);
#endif
}

/** One directory level, by UTF-8 path. True if it now exists as a directory,
 *  whether this made it or it was there already. */
inline bool makeDirectory(const char* path) {
  if (!path || !path[0]) return false;
#if defined(_WIN32)
  const std::wstring wide = widenPath(path);
  _wmkdir(wide.c_str());
  struct _stat64 st {};
  return _wstat64(wide.c_str(), &st) == 0 && (st.st_mode & _S_IFDIR) != 0;
#else
  ::mkdir(path, 0755);
  struct stat st {};
  return ::stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

/** Is there a directory at this UTF-8 path? */
inline bool isDirectoryAt(const char* path) {
  if (!path || !path[0]) return false;
#if defined(_WIN32)
  struct _stat64 st {};
  return _wstat64(widenPath(path).c_str(), &st) == 0 && (st.st_mode & _S_IFDIR) != 0;
#else
  struct stat st {};
  return ::stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

} // namespace pathio
} // namespace sonore
