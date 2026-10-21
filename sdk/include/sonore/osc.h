// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: Open Sound Control.
//
// The protocol control surfaces speak: TouchOSC, Lemur, Open Stage Control,
// lighting desks, anything that wants to move a parameter from another machine
// without MIDI's 7-bit ceiling.
//
// OSC 1.0, the part that is actually used: messages with int32, float32,
// string and blob arguments, and bundles. Everything is 4-byte aligned and
// BIG-endian, which is the single most common source of "it works on my
// machine" in a hand-rolled implementation, so the alignment and byte order
// are asserted in the tests rather than assumed.
//
// The codec has no networking in it at all: encode()/decode() move bytes, and
// the UDP socket below is a separate, optional convenience. That split is what
// lets the codec be tested exhaustively without a network.

#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// A <windows.h> included earlier WITHOUT WIN32_LEAN_AND_MEAN has already
// pulled in the obsolete <winsock.h>, and <winsock2.h> cannot follow it:
// every type is declared twice, a hundred redefinition errors decided by
// somebody else's include order. So when winsock.h is in and winsock2.h is
// not, winsock.h is what this file uses. Everything the transport below
// calls -- socket, bind, getsockname, recv, sendto, ioctlsocket,
// closesocket, WSAStartup -- it declares too, with the same signatures and
// the same ws2_32 exports behind them; the two things it lacks are not used
// (the address is parsed by detail::parseIPv4, not inet_pton, and a socket
// length is an int, not socklen_t). net.h solves the same clash by declaring
// its own structures; this file needs far less, and neither cares about the
// order any more.
#if !defined(_WINSOCKAPI_) || defined(_WINSOCK2API_)
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#if defined(_MSC_VER)
// Link the socket library from the header itself: a plugin that includes
// osc.h should not also have to know it needs ws2_32 in its build file.
#pragma comment(lib, "ws2_32.lib")
#endif
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace sonore {
namespace osc {

/** What an argument can be: the OSC 1.0 core types, and the 1.1 / common
 *  extensions controllers actually send. i, f, s and b are what every
 *  controller sends, and once they were all that was understood: a message
 *  with any other tag was refused -- and a bundle is all or nothing, so one
 *  TouchOSC toggle (",T") in a bundle lost every message beside it. A tag
 *  outside this list is still refused, never skipped blindly. */
enum class Type : char {
  Int32 = 'i',     ///< intValue.
  Float32 = 'f',   ///< floatValue.
  String = 's',    ///< stringValue.
  Blob = 'b',      ///< blobValue.
  Int64 = 'h',     ///< int64Value.
  TimeTag = 't',   ///< int64Value, the raw 64-bit NTP time.
  Double = 'd',    ///< doubleValue.
  Symbol = 'S',    ///< stringValue.
  Char = 'c',      ///< intValue.
  Rgba = 'r',      ///< intValue, 0xRRGGBBAA.
  Midi = 'm',      ///< intValue: port id, status, data1, data2, high byte first.
  BoolTrue = 'T',  ///< No payload; intValue 1 (not "True": Xlib defines that as a macro).
  BoolFalse = 'F', ///< No payload; intValue 0.
  Nil = 'N',       ///< No payload.
  Impulse = 'I',   ///< No payload ("bang").
};

/**
 * The 'r' argument: a 32-bit colour, red in the high byte and alpha in the
 * low, big-endian on the wire like every other OSC word.
 *
 * Its own type rather than an int an application has to take apart, because
 * the byte order is exactly the thing that gets taken apart wrongly: ARGB
 * habits from every graphics API put alpha on top, and a lighting desk sent
 * a colour read that way shows a different one.
 */
struct OSCColour {
  uint8_t red = 0, ///< 0..255.
      green = 0,   ///< 0..255.
      blue = 0,    ///< 0..255.
      alpha = 255; ///< 0..255, 255 opaque.

  /** From 0xRRGGBBAA: red in the high byte, alpha in the low. */
  static OSCColour fromInt32RGBA(uint32_t rgba) {
    OSCColour c;
    c.red = (uint8_t) (rgba >> 24);
    c.green = (uint8_t) ((rgba >> 16) & 0xff);
    c.blue = (uint8_t) ((rgba >> 8) & 0xff);
    c.alpha = (uint8_t) (rgba & 0xff);
    return c;
  }
  /** As 0xRRGGBBAA, the 'r' argument's word. */
  uint32_t toInt32() const {
    return ((uint32_t) red << 24) | ((uint32_t) green << 16) | ((uint32_t) blue << 8) |
           (uint32_t) alpha;
  }
  bool operator==(const OSCColour& o) const { return toInt32() == o.toInt32(); } ///< All four bytes equal.
  bool operator!=(const OSCColour& o) const { return !(*this == o); }            ///< Not ==.
};

/**
 * One OSC argument: its type tag and its value.
 *
 * Flat rather than a union: every value field is there, and `type` says
 * which one is meant -- intValue, floatValue, stringValue and blobValue for
 * the four core types, and the comment on each other Type names its field.
 * The make functions set both, and decodeMessage() fills both, so read the
 * field the type names and no other: an Int32 leaves floatValue at 0, it
 * does not convert.
 * A plain value that allocates for strings and blobs, so building one is not
 * for the audio thread.
 */
struct Argument {
  Type type = Type::Float32;      ///< Which field below holds the value.
  int32_t intValue = 0;           ///< Int32, Char, Rgba, Midi, and 1 or 0 for the booleans.
  float floatValue = 0.0f;        ///< Float32.
  int64_t int64Value = 0;         ///< Int64 and TimeTag.
  double doubleValue = 0.0;       ///< Double.
  std::string stringValue;        ///< String and Symbol, UTF-8.
  std::vector<uint8_t> blobValue; ///< Blob.

  /** An 'i' argument. */
  static Argument makeInt(int32_t v) {
    Argument a;
    a.type = Type::Int32;
    a.intValue = v;
    return a;
  }
  /** An 'f' argument. */
  static Argument makeFloat(float v) {
    Argument a;
    a.type = Type::Float32;
    a.floatValue = v;
    return a;
  }
  /** An 's' argument; allocates. */
  static Argument makeString(const std::string& v) {
    Argument a;
    a.type = Type::String;
    a.stringValue = v;
    return a;
  }
  /** A 'b' argument, a copy of `v`; allocates. */
  static Argument makeBlob(const std::vector<uint8_t>& v) {
    Argument a;
    a.type = Type::Blob;
    a.blobValue = v;
    return a;
  }
  /** An 'r' argument. Carried in intValue as 0xRRGGBBAA, which is what the
   *  codec writes; getColour() is the typed way back. */
  static Argument makeColour(const OSCColour& c) {
    Argument a;
    a.type = Type::Rgba;
    a.intValue = (int32_t) c.toInt32();
    return a;
  }

  /** True for an 'r' argument. */
  bool isColour() const { return type == Type::Rgba; }
  /** The colour, for an 'r' argument; opaque black for anything else, rather
   *  than an int of some other type reinterpreted as a colour. */
  OSCColour getColour() const {
    return isColour() ? OSCColour::fromInt32RGBA((uint32_t) intValue) : OSCColour{};
  }
};

/** One OSC message: an address starting with '/' and its arguments in
 *  order. What encode() writes into a datagram and decodeMessage() and
 *  decodePacket() read out of one -- a bundle arrives as the messages inside
 *  it. encode() refuses an address without the leading '/' by returning no
 *  bytes. */
struct Message {
  std::string address;             ///< E.g. "/sonore/param/drive"; may be a pattern from a sender.
  std::vector<Argument> arguments; ///< In order.
};

namespace detail {

/** Kept out of child processes. Winsock's sockets are inheritable handles and
 *  POSIX's descriptors survive exec unless told otherwise, so a process the
 *  host (or another plugin) starts while this one listens would hold the port
 *  open after close(): net.h's sockets were caught doing exactly that. */
inline void socketNotInherited(int s) {
  if (s < 0) return;
#if defined(_WIN32)
  SetHandleInformation((HANDLE) (SOCKET) s, HANDLE_FLAG_INHERIT, 0);
#else
  const int fd = ::fcntl(s, F_GETFD, 0);
  if (fd >= 0) ::fcntl(s, F_SETFD, fd | FD_CLOEXEC);
#endif
}

/** A dotted-quad IPv4 address, "192.168.1.20", as its 32-bit value in host
 *  order. Exactly four decimal parts of 0..255 and nothing around them; a
 *  part with a leading zero ("010") is refused rather than guessed at, since
 *  some parsers read it as octal. That is inet_pton(AF_INET)'s rule on Linux
 *  and macOS, written out so Windows keeps it too -- and so this file needs
 *  nothing from ws2tcpip.h, which is not there after a plain <windows.h>. */
inline bool parseIPv4(const char* text, uint32_t* out) {
  if (!text) return false;
  uint32_t value = 0;
  for (int part = 0; part < 4; ++part) {
    if (part > 0 && *text++ != '.') return false;
    if (*text < '0' || *text > '9') return false;
    uint32_t octet = 0;
    for (bool first = true; *text >= '0' && *text <= '9'; ++text, first = false) {
      if (!first && octet == 0) return false; // "00", "01": a leading zero
      octet = octet * 10u + (uint32_t) (*text - '0');
      if (octet > 255u) return false;
    }
    value = (value << 8) | octet;
  }
  if (*text != '\0') return false;
  if (out) *out = value;
  return true;
}

/** OSC is big-endian on the wire regardless of the host, so the conversion is
 *  explicit rather than borrowed from htonl: this file must behave the same
 *  whether or not a socket header was included. */
inline void writeU32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back((uint8_t) ((v >> 24) & 0xff));
  out.push_back((uint8_t) ((v >> 16) & 0xff));
  out.push_back((uint8_t) ((v >> 8) & 0xff));
  out.push_back((uint8_t) (v & 0xff));
}

inline bool readU32(const uint8_t* data, size_t size, size_t& pos, uint32_t* out) {
  if (pos + 4 > size) return false;
  *out = ((uint32_t) data[pos] << 24) | ((uint32_t) data[pos + 1] << 16) |
         ((uint32_t) data[pos + 2] << 8) | (uint32_t) data[pos + 3];
  pos += 4;
  return true;
}

/** Strings are null-terminated and then padded to a 4-byte boundary, with at
 *  least ONE null, so a string whose length is already a multiple of four
 *  still gains four padding bytes. Getting that edge wrong shifts everything
 *  after it. */
inline void writeString(std::vector<uint8_t>& out, const std::string& s) {
  // Up to the first NUL, which is where every reader will stop. Written
  // verbatim, "ab\0cdefgh" ended at "ab" on the other side and its padding
  // was then read as the NEXT argument: an int of 1684366951.
  const size_t nul = s.find('\0');
  const size_t length = nul == std::string::npos ? s.size() : nul;
  out.insert(out.end(), s.begin(), s.begin() + (std::ptrdiff_t) length);
  const size_t padded = (length / 4 + 1) * 4;
  out.resize(out.size() + (padded - length), 0);
}

inline void writeU64(std::vector<uint8_t>& out, uint64_t v) {
  writeU32(out, (uint32_t) (v >> 32));
  writeU32(out, (uint32_t) (v & 0xffffffffu));
}

inline bool readString(const uint8_t* data, size_t size, size_t& pos, std::string* out) {
  const size_t start = pos;
  while (pos < size && data[pos] != 0) ++pos;
  if (pos >= size) return false; // unterminated
  out->assign((const char*) data + start, pos - start);
  const size_t length = pos - start;
  pos = start + (length / 4 + 1) * 4;
  return pos <= size;
}

} // namespace detail

/** Encode one message. Returns the bytes to put in a datagram. */
inline std::vector<uint8_t> encode(const Message& message) {
  std::vector<uint8_t> out;
  if (message.address.empty() || message.address[0] != '/') return out; // not an address
  detail::writeString(out, message.address);

  std::string tags = ",";
  for (const Argument& a : message.arguments) tags.push_back((char) a.type);
  detail::writeString(out, tags);

  for (const Argument& a : message.arguments) {
    switch (a.type) {
      case Type::Int32:
        detail::writeU32(out, (uint32_t) a.intValue);
        break;
      case Type::Float32: {
        // The bit pattern, not a cast: OSC carries IEEE 754 floats verbatim.
        uint32_t bits = 0;
        std::memcpy(&bits, &a.floatValue, 4);
        detail::writeU32(out, bits);
        break;
      }
      case Type::String:
      case Type::Symbol:
        detail::writeString(out, a.stringValue);
        break;
      case Type::Int64:
      case Type::TimeTag:
        detail::writeU64(out, (uint64_t) a.int64Value);
        break;
      case Type::Double: {
        uint64_t bits = 0;
        std::memcpy(&bits, &a.doubleValue, 8);
        detail::writeU64(out, bits);
        break;
      }
      case Type::Char:
      case Type::Rgba:
      case Type::Midi:
        detail::writeU32(out, (uint32_t) a.intValue);
        break;
      case Type::BoolTrue:
      case Type::BoolFalse:
      case Type::Nil:
      case Type::Impulse:
        break; // the tag is the whole value
      case Type::Blob: {
        detail::writeU32(out, (uint32_t) a.blobValue.size());
        out.insert(out.end(), a.blobValue.begin(), a.blobValue.end());
        const size_t pad = (4 - (a.blobValue.size() % 4)) % 4;
        out.resize(out.size() + pad, 0);
        break;
      }
    }
  }
  return out;
}

/** Decode one message. Returns false on anything malformed rather than
 *  guessing: a datagram from the network is untrusted input. */
inline bool decodeMessage(const uint8_t* data, size_t size, Message* out) {
  if (!data || !out || size < 8) return false;
  size_t pos = 0;
  if (!detail::readString(data, size, pos, &out->address)) return false;
  if (out->address.empty() || out->address[0] != '/') return false;

  std::string tags;
  if (!detail::readString(data, size, pos, &tags)) return false;
  if (tags.empty() || tags[0] != ',') return false;

  out->arguments.clear();
  for (size_t i = 1; i < tags.size(); ++i) {
    Argument a;
    switch (tags[i]) {
      case 'i': {
        uint32_t v = 0;
        if (!detail::readU32(data, size, pos, &v)) return false;
        a.type = Type::Int32;
        a.intValue = (int32_t) v;
        break;
      }
      case 'f': {
        uint32_t bits = 0;
        if (!detail::readU32(data, size, pos, &bits)) return false;
        a.type = Type::Float32;
        std::memcpy(&a.floatValue, &bits, 4);
        break;
      }
      case 's':
      case 'S': {
        if (!detail::readString(data, size, pos, &a.stringValue)) return false;
        a.type = tags[i] == 's' ? Type::String : Type::Symbol;
        break;
      }
      case 'h':
      case 't':
      case 'd': {
        uint32_t high = 0, low = 0;
        if (!detail::readU32(data, size, pos, &high) || !detail::readU32(data, size, pos, &low))
          return false;
        const uint64_t v = ((uint64_t) high << 32) | low;
        if (tags[i] == 'd') {
          a.type = Type::Double;
          std::memcpy(&a.doubleValue, &v, 8);
        } else {
          a.type = tags[i] == 'h' ? Type::Int64 : Type::TimeTag;
          a.int64Value = (int64_t) v;
        }
        break;
      }
      case 'c':
      case 'r':
      case 'm': {
        uint32_t v = 0;
        if (!detail::readU32(data, size, pos, &v)) return false;
        a.type = tags[i] == 'c' ? Type::Char : (tags[i] == 'r' ? Type::Rgba : Type::Midi);
        a.intValue = (int32_t) v;
        break;
      }
      case 'T':
      case 'F':
        a.type = tags[i] == 'T' ? Type::BoolTrue : Type::BoolFalse;
        a.intValue = tags[i] == 'T' ? 1 : 0;
        break;
      case 'N':
        a.type = Type::Nil;
        break;
      case 'I':
        a.type = Type::Impulse;
        break;
      case 'b': {
        uint32_t length = 0;
        if (!detail::readU32(data, size, pos, &length)) return false;
        // As what is LEFT. pos + length wraps on a 32-bit build for a length
        // near 4 GB -- in a datagram anyone on the network can send.
        if (length > size - pos) return false;
        a.type = Type::Blob;
        a.blobValue.assign(data + pos, data + pos + length);
        pos += length + (4 - (length % 4)) % 4;
        if (pos > size) return false;
        break;
      }
      default:
        return false; // a type we do not model: refuse, never skip blindly
    }
    out->arguments.push_back(std::move(a));
  }
  return true;
}

namespace detail {

/** How deep bundles may sit inside one another. OSC names no limit, and the
 *  senders that nest at all go a level or two down. Without one, a datagram
 *  of bundles each holding only the next -- 3275 of them fit in 64 KB --
 *  recursed once per level, and that one datagram overflowed the stack of a
 *  512 KB thread (what std::thread gets on macOS), or of the main thread in
 *  a Debug build: the host gone, from anyone who could reach the port. */
const int kMaxBundleDepth = 32;

/** decodePacket, appending to `out`; the caller takes back what was appended
 *  when this fails. One vector for every level rather than one per bundle
 *  moved into the one around it, which moved each message once per level
 *  above it: 1600 levels around 2700 messages, one datagram, took 190 ms --
 *  twelve seconds of the UI thread for the 64 datagrams a poll reads. */
inline bool decodePacketAt(const uint8_t* data, size_t size, std::vector<Message>* out, int depth) {
  if (size < 8) return false;
  if (std::memcmp(data, "#bundle", 7) == 0) {
    if (depth >= kMaxBundleDepth) return false;
    size_t pos = 16; // "#bundle\0" plus the 8-byte time tag
    while (pos + 4 <= size) {
      uint32_t length = 0;
      if (!readU32(data, size, pos, &length)) return false;
      if (length == 0 || length > size - pos) return false;
      if (!decodePacketAt(data + pos, length, out, depth + 1)) return false;
      pos += length;
    }
    return true;
  }
  Message m;
  if (!decodeMessage(data, size, &m)) return false;
  out->push_back(std::move(m));
  return true;
}

} // namespace detail

/** Decode a datagram that may be a bundle, flattening it to the messages
 *  inside. Bundles nest, up to detail::kMaxBundleDepth deep; a deeper packet
 *  is refused like any other malformed one. */
inline bool decodePacket(const uint8_t* data, size_t size, std::vector<Message>* out) {
  if (!data || !out) return false;
  // All or nothing. A bundle is one packet whose messages are meant to take
  // effect together; decoding straight into `out` handed over every message
  // before a broken one, and the receiver delivered them -- half a bundle
  // applied, from a packet that was refused.
  const size_t before = out->size();
  if (detail::decodePacketAt(data, size, out, 0)) return true;
  out->erase(out->begin() + (std::ptrdiff_t) before, out->end());
  return false;
}

// ── Address patterns ─────────────────────────────────────────────────────────
//
// In OSC the SENDER writes a pattern and the receiver's methods have plain
// addresses: /synth/*/cutoff reaches /synth/1/cutoff and /synth/2/cutoff,
// /mixer/{bass,drums}/mute reaches two channels. A controller built for a rack
// of devices it did not configure -- TouchOSC templates, show-control
// software -- sends these as a matter of course, and an exact-match receiver
// answers none of them.
//
// OSC 1.0's rules, part by part (a wildcard never crosses a '/', and pattern
// and address must have the same number of parts):
//
//   ?        any one character
//   *        any run of characters, including none
//   [abc]    one of those characters; a-z inside is a range; a '!' first
//            negates; a '-' first or last is itself
//   {a,bc}   one of those strings, matched literally
//
// Matching is not backtracking. Each part is matched by carrying the SET of
// positions in the address the pattern so far can have consumed, one pattern
// element at a time, so "*a*a*a*a*b" against a long address costs pattern
// length times address length rather than exploding -- the pattern arrives
// from the network, and a matcher that can be made to spin by a datagram is
// a denial of service.
//
// A malformed pattern (an unclosed '[' or '{', an empty part, a ']' with no
// '[') matches NOTHING. Guessing what a broken pattern meant is how a stray
// byte moves the wrong parameter.
//
// Not here: OSC 1.1's '//' (match across any number of parts). 1.0 is what
// senders can rely on receivers to have.

namespace detail {

/** Characters no address may contain: OSC 1.0's reserved set, plus space
 *  and control characters. */
inline bool isAddressChar(unsigned char c) {
  if (c <= 0x20 || c == 0x7F) return false;
  switch (c) {
    case '#': case '*': case ',': case '/': case '?': case '[': case ']': case '{': case '}':
      return false;
    default:
      return true;
  }
}

/** Does `c` fall in the bracket expression between `open` and `close`
 *  (exclusive), '!' already stripped? */
inline bool inClass(const char* p, size_t n, unsigned char c) {
  for (size_t i = 0; i < n; ++i) {
    unsigned char lo = (unsigned char) p[i];
    // A range needs a character on both sides of the '-'; a '-' first or
    // last is a literal minus.
    if (i + 2 < n && p[i + 1] == '-') {
      unsigned char hi = (unsigned char) p[i + 2];
      if (hi < lo) {
        const unsigned char t = lo;
        lo = hi;
        hi = t;
      }
      if (c >= lo && c <= hi) return true;
      i += 2;
      continue;
    }
    if (c == lo) return true;
  }
  return false;
}

/** One part of a pattern against one part of an address. `p` is known
 *  well-formed (OSCAddressPattern checked it). */
inline bool matchPart(const char* p, size_t pn, const char* s, size_t sn) {
  // reach[k] != 0: the pattern so far can have consumed exactly k characters.
  uint8_t local[2][128];
  std::vector<uint8_t> big;
  uint8_t* cur = local[0];
  uint8_t* next = local[1];
  if (sn + 1 > sizeof(local[0])) {
    big.assign(2 * (sn + 1), 0);
    cur = big.data();
    next = big.data() + sn + 1;
  }
  std::memset(cur, 0, sn + 1);
  cur[0] = 1;

  size_t i = 0;
  while (i < pn) {
    std::memset(next, 0, sn + 1);
    bool any = false;
    const char c = p[i];
    if (c == '*') {
      while (i < pn && p[i] == '*') ++i; // a run of stars is one star
      size_t first = 0;
      while (first <= sn && !cur[first]) ++first;
      for (size_t k = first; k <= sn; ++k) next[k] = 1;
      any = first <= sn;
    } else if (c == '[') {
      size_t close = i + 1;
      while (p[close] != ']') ++close;
      const bool negate = p[i + 1] == '!';
      const char* set = p + i + 1 + (negate ? 1 : 0);
      const size_t setLength = close - (i + 1) - (negate ? 1 : 0);
      for (size_t k = 0; k < sn; ++k)
        if (cur[k] && inClass(set, setLength, (unsigned char) s[k]) != negate) {
          next[k + 1] = 1;
          any = true;
        }
      i = close + 1;
    } else if (c == '{') {
      size_t close = i + 1;
      while (p[close] != '}') ++close;
      size_t start = i + 1;
      for (size_t j = i + 1; j <= close; ++j) {
        if (j < close && p[j] != ',') continue;
        const size_t len = j - start;
        for (size_t k = 0; k + len <= sn; ++k)
          if (cur[k] && std::memcmp(s + k, p + start, len) == 0) {
            next[k + len] = 1;
            any = true;
          }
        start = j + 1;
      }
      i = close + 1;
    } else {
      for (size_t k = 0; k < sn; ++k)
        if (cur[k] && (c == '?' || s[k] == c)) {
          next[k + 1] = 1;
          any = true;
        }
      ++i;
    }
    if (!any) return false; // no way forward: no need to read the rest
    uint8_t* t = cur;
    cur = next;
    next = t;
  }
  return cur[sn] != 0;
}

/** The parts of a path: the text between slashes, as (offset, length). An
 *  empty part -- "//" or a trailing '/' -- makes the whole path malformed. */
inline bool splitParts(const std::string& path, std::vector<std::pair<size_t, size_t>>* parts) {
  parts->clear();
  if (path.size() < 2 || path[0] != '/') return false;
  size_t start = 1;
  for (size_t i = 1; i <= path.size(); ++i) {
    if (i < path.size() && path[i] != '/') continue;
    if (i == start) return false;
    parts->push_back({start, i - start});
    start = i + 1;
  }
  return true;
}

} // namespace detail

/** A plain OSC address: '/', then parts with none of the pattern or
 *  reserved characters in them. What a listener registers at. */
inline bool isValidAddress(const std::string& address) {
  std::vector<std::pair<size_t, size_t>> parts;
  if (!detail::splitParts(address, &parts)) return false;
  for (const auto& part : parts)
    for (size_t i = 0; i < part.second; ++i)
      if (!detail::isAddressChar((unsigned char) address[part.first + i])) return false;
  return true;
}

/**
 * An OSC address pattern, parsed and checked once so that matching it
 * against many addresses -- every listener a receiver has -- does not
 * re-parse it for each.
 */
class OSCAddressPattern {
public:
  /** Empty, and not valid: it matches nothing. */
  OSCAddressPattern() = default;
  /** Parse `pattern`: '/'-separated parts, with *, ?, [..], [!..] and {a,b}
   *  allowed in each. isValid() says whether it parsed. */
  explicit OSCAddressPattern(const std::string& pattern) : text_(pattern) {
    valid_ = parse();
  }

  /** False for a malformed pattern: an unclosed bracket, an empty part, a
   *  character an address may not hold. Such a pattern matches nothing. */
  bool isValid() const { return valid_; }
  /** False for a pattern that is a plain address -- the common case, and
   *  the one a receiver can answer with a lookup instead of a scan. */
  bool containsWildcards() const { return wildcards_; }
  /** The pattern as it was given. */
  const std::string& toString() const { return text_; }

  /** Does this pattern reach `address`? False if either is malformed. */
  bool matches(const std::string& address) const {
    if (!valid_) return false;
    if (!wildcards_) return address == text_; // a valid pattern with no wildcard is an address
    std::vector<std::pair<size_t, size_t>> parts;
    if (!detail::splitParts(address, &parts) || parts.size() != parts_.size()) return false;
    for (size_t i = 0; i < parts.size(); ++i) {
      for (size_t k = 0; k < parts[i].second; ++k)
        if (!detail::isAddressChar((unsigned char) address[parts[i].first + k])) return false;
      if (!detail::matchPart(text_.data() + parts_[i].first, parts_[i].second,
                             address.data() + parts[i].first, parts[i].second))
        return false;
    }
    return true;
  }

private:
  bool parse() {
    if (!detail::splitParts(text_, &parts_)) return false;
    for (const auto& part : parts_) {
      const char* p = text_.data() + part.first;
      const size_t n = part.second;
      for (size_t i = 0; i < n; ++i) {
        const unsigned char c = (unsigned char) p[i];
        if (c == '*' || c == '?') {
          wildcards_ = true;
        } else if (c == '[') {
          wildcards_ = true;
          size_t j = i + 1;
          if (j < n && p[j] == '!') ++j;
          const size_t first = j;
          while (j < n && p[j] != ']') ++j;
          if (j >= n || j == first) return false; // unclosed, or nothing in it
          for (size_t k = first; k < j; ++k)
            if ((unsigned char) p[k] <= 0x20 || (unsigned char) p[k] == 0x7F) return false;
          i = j;
        } else if (c == '{') {
          wildcards_ = true;
          size_t j = i + 1;
          while (j < n && p[j] != '}') {
            const unsigned char d = (unsigned char) p[j];
            // Alternatives are literal strings: separated by ',', and made of
            // what an address may contain.
            if (d != ',' && !detail::isAddressChar(d)) return false;
            ++j;
          }
          if (j >= n) return false; // unclosed
          i = j;
        } else if (!detail::isAddressChar(c)) {
          return false; // ']' or '}' with no opener, ',' or '#' or a space
        }
      }
    }
    return true;
  }

  std::string text_;
  std::vector<std::pair<size_t, size_t>> parts_;
  bool valid_ = false;
  bool wildcards_ = false;
};

/** Does `pattern` reach `address`? One call, for a caller with one of each. */
inline bool matchesPattern(const std::string& pattern, const std::string& address) {
  return OSCAddressPattern(pattern).matches(address);
}

/**
 * Hands each message to the listeners its address pattern reaches.
 *
 * Exact matching is still the FAST path: a message whose address has no
 * wildcard in it -- nearly all of them -- is one map lookup, exactly as
 * before patterns existed. Only a pattern pays for a scan, and it is parsed
 * once for the whole scan rather than once per listener.
 *
 * On whatever thread calls dispatch(), which for a Receiver is the thread
 * that polls it: the UI timer, never the audio thread -- a handler is a
 * std::function and may do anything.
 */
class Dispatcher {
public:
  /** Called with each message that reaches its address. */
  using Handler = std::function<void(const Message&)>;

  /** Listen at `address`, a plain address (patterns are what senders
   *  write). False, and nothing added, if it is not a valid one. */
  bool addListener(const std::string& address, Handler handler) {
    if (!handler || !isValidAddress(address)) return false;
    listeners_[address].push_back(std::move(handler));
    return true;
  }

  /** Drop every handler at `address`; nothing happens for one with none. */
  void removeListeners(const std::string& address) { listeners_.erase(address); }
  /** Drop every handler. */
  void clear() { listeners_.clear(); }
  /** How many addresses have a handler. */
  size_t numAddresses() const { return listeners_.size(); }

  /** Deliver one message. Returns how many handlers ran. A handler may add
   *  or remove listeners: the ones to call are chosen before any is called. */
  int dispatch(const Message& message) const {
    std::vector<Handler> chosen;
    const OSCAddressPattern pattern(message.address);
    if (!pattern.isValid()) return 0;
    if (!pattern.containsWildcards()) {
      const auto it = listeners_.find(message.address);
      if (it == listeners_.end()) return 0;
      chosen = it->second;
    } else {
      for (const auto& entry : listeners_)
        if (pattern.matches(entry.first))
          chosen.insert(chosen.end(), entry.second.begin(), entry.second.end());
    }
    for (const Handler& h : chosen) h(message);
    return (int) chosen.size();
  }

  /** Each message in turn, as poll() returns them. Returns how many
   *  handlers ran in all. */
  int dispatch(const std::vector<Message>& messages) const {
    int calls = 0;
    for (const Message& m : messages) calls += dispatch(m);
    return calls;
  }

private:
  std::map<std::string, std::vector<Handler>> listeners_;
};

// ── UDP transport ────────────────────────────────────────────────────────────
//
// Optional, and deliberately separate from the codec: a plugin that only wants
// to parse packets someone else delivered never opens a socket.

/** A non-blocking UDP receiver. poll() returns whatever arrived since the last
 *  call and never waits, so it is safe to call from a UI timer. */
class Receiver {
public:
  /** Not listening: poll() returns nothing until open(). */
  Receiver() = default;
  // One socket and one Winsock startup, owned: a copy closed the socket and
  // cleaned Winsock up a second time when it died. Moved, the ownership goes
  // with it -- returned from a function, kept in a vector -- and the source
  // is left closed.
  Receiver(const Receiver&) = delete;            ///< Not copyable: it owns a socket.
  Receiver& operator=(const Receiver&) = delete; ///< Not copyable: it owns a socket.
  /** Takes the socket; `other` is left closed. */
  Receiver(Receiver&& other) noexcept { take(other); }
  /** Closes this one's socket, then takes `other`'s; `other` is left closed. */
  Receiver& operator=(Receiver&& other) noexcept {
    if (this != &other) {
      release();
      take(other);
    }
    return *this;
  }
  /** Closes the socket and, on Windows, ends its Winsock startup. */
  ~Receiver() { release(); }

  /**
   * Listen on `port` (0 lets the system choose; port() says which).
   *
   * Only THIS MACHINE can reach it unless `fromNetwork` is true. It used to
   * bind every interface, so anybody on the same network -- a café's Wi-Fi, a
   * venue's -- could move a plugin's parameters with one datagram, and nothing
   * on screen said so. A controller app on the same computer needs nothing
   * more; a phone or a second machine is something the plugin chooses, ideally
   * behind a setting the user can see.
   */
  bool open(uint16_t port, bool fromNetwork = false) {
    close(); // opened again: the first socket is closed, not leaked
    // And the last failure forgotten: a receiver that opened after "the port
    // is already in use" went on saying so.
    error_.clear();
#if defined(_WIN32)
    WSADATA wsa;
    if (!wsaStarted_ && WSAStartup(MAKEWORD(2, 2), &wsa) == 0) wsaStarted_ = true;
#endif
    socket_ = (int) ::socket(AF_INET, SOCK_DGRAM, 0);
    detail::socketNotInherited(socket_);
    if (socket_ < 0) {
      error_ = "could not create a UDP socket";
      return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(fromNetwork ? INADDR_ANY : INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (::bind((SocketHandle) socket_, (sockaddr*) &addr, sizeof(addr)) != 0) {
      error_ = "the port is already in use";
      close();
      return false;
    }
    setNonBlocking();
    // Ask the OS which port it actually gave us, so port 0 (pick one) works.
    SockLen len = (SockLen) sizeof(addr);
    if (::getsockname((SocketHandle) socket_, (sockaddr*) &addr, &len) == 0)
      port_ = ntohs(addr.sin_port);
    else
      port_ = port;
    return true;
  }

  /** Stop listening; harmless when not open. open() may follow. */
  void close() {
    if (socket_ >= 0) {
#if defined(_WIN32)
      ::closesocket((SocketHandle) socket_);
#else
      ::close(socket_);
#endif
      socket_ = -1;
    }
  }

  /** Everything that arrived, decoded. Malformed datagrams are dropped, not
   *  reported as messages: the network is untrusted. */
  std::vector<Message> poll() {
    std::vector<Message> messages;
    if (socket_ < 0) return messages;
    // Room for the largest UDP datagram there is. 4 KB truncated anything
    // bigger -- a blob, a long bundle -- and on Windows the truncation is an
    // ERROR (WSAEMSGSIZE) that ended this loop, so the messages queued
    // behind it waited for the next poll as well.
    if (buffer_.empty()) buffer_.resize(65536);
    for (int i = 0; i < 64; ++i) { // bounded: never spin on a flood
      const int got =
          (int) ::recv((SocketHandle) socket_, (char*) buffer_.data(), (int) buffer_.size(), 0);
      if (got <= 0) break;
      decodePacket(buffer_.data(), (size_t) got, &messages);
    }
    return messages;
  }

  /** poll(), then each message to the listeners its address reaches.
   *  Returns how many handlers ran. */
  int pollAndDispatch(const Dispatcher& dispatcher) { return dispatcher.dispatch(poll()); }

  /** The port it listens on, the system's choice when open() was given 0;
   *  0 before the first open(). */
  uint16_t port() const { return port_; }
  /** Why the latest open() failed; empty after one that succeeded. */
  const std::string& error() const { return error_; }

private:
  void release() {
    close();
#if defined(_WIN32)
    // Winsock counts its startups and is freed by the last cleanup. Never
    // cleaned up, it stayed initialised in the host after the plugin left.
    if (wsaStarted_) WSACleanup();
    wsaStarted_ = false;
#endif
  }
  void take(Receiver& other) noexcept {
    socket_ = other.socket_;
    other.socket_ = -1;
    port_ = other.port_;
    other.port_ = 0;
    buffer_ = std::move(other.buffer_);
    error_ = std::move(other.error_);
#if defined(_WIN32)
    wsaStarted_ = other.wsaStarted_;
    other.wsaStarted_ = false;
#endif
  }
#if defined(_WIN32)
  using SocketHandle = SOCKET;
  using SockLen = int; // ws2tcpip's socklen_t is this int, and winsock.h has none
#else
  using SocketHandle = int;
  using SockLen = socklen_t;
#endif

  void setNonBlocking() {
#if defined(_WIN32)
    u_long mode = 1;
    ::ioctlsocket((SocketHandle) socket_, FIONBIO, &mode);
#else
    const int flags = ::fcntl(socket_, F_GETFL, 0);
    ::fcntl(socket_, F_SETFL, flags | O_NONBLOCK);
#endif
  }

  int socket_ = -1;
  uint16_t port_ = 0;
  std::vector<uint8_t> buffer_; // one datagram, sized on the first poll
  std::string error_;
#if defined(_WIN32)
  bool wsaStarted_ = false;
#endif
};

/** Fire-and-forget UDP sender. */
class Sender {
public:
  /** Not open: send() returns false until open(). */
  Sender() = default;
  Sender(const Sender&) = delete; ///< Not copyable: it owns its socket and Winsock, as Receiver does.
  Sender& operator=(const Sender&) = delete; ///< Not copyable: it owns a socket.
  /** Takes the socket and target; `other` is left closed. */
  Sender(Sender&& other) noexcept { take(other); }
  /** Closes this one's socket, then takes `other`'s; `other` is left closed. */
  Sender& operator=(Sender&& other) noexcept {
    if (this != &other) {
      release();
      take(other);
    }
    return *this;
  }
  /** Closes the socket and, on Windows, ends its Winsock startup. */
  ~Sender() { release(); }

  /** Close whatever was open, then aim at `host`:`port`. `host` is a dotted
   *  IPv4 address, "192.168.1.20" -- no names are looked up -- and null means
   *  127.0.0.1. False for an address that does not parse or a socket that
   *  cannot be made. */
  bool open(const char* host, uint16_t port) {
    close(); // as Receiver::open
#if defined(_WIN32)
    // Once per object, as the Receiver does, and cleaned up with it. Every
    // open() started Winsock again and nothing ever stopped it: a sender
    // reopened a thousand times left a thousand startups behind.
    WSADATA wsa;
    if (!wsaStarted_ && WSAStartup(MAKEWORD(2, 2), &wsa) == 0) wsaStarted_ = true;
#endif
    socket_ = (int) ::socket(AF_INET, SOCK_DGRAM, 0);
    detail::socketNotInherited(socket_);
    if (socket_ < 0) return false;
    std::memset(&target_, 0, sizeof(target_));
    target_.sin_family = AF_INET;
    target_.sin_port = htons(port);
    uint32_t address = 0;
    if (!detail::parseIPv4(host ? host : "127.0.0.1", &address)) {
      close();
      return false;
    }
    target_.sin_addr.s_addr = htonl(address);
    return true;
  }

  /** Close the socket; harmless when not open. */
  void close() {
    if (socket_ >= 0) {
#if defined(_WIN32)
      ::closesocket((SOCKET) socket_);
#else
      ::close(socket_);
#endif
      socket_ = -1;
    }
  }

  /** Encode `message` and send it as one datagram, without waiting for an
   *  answer. False when not open, when the message does not encode (an
   *  address without its '/'), or when the system did not take all of it.
   *  Allocates: not for the audio thread. */
  bool send(const Message& message) {
    if (socket_ < 0) return false;
    const std::vector<uint8_t> bytes = encode(message);
    if (bytes.empty()) return false;
    const int sent = (int) ::sendto(
#if defined(_WIN32)
        (SOCKET) socket_,
#else
        socket_,
#endif
        (const char*) bytes.data(), (int) bytes.size(), 0, (const sockaddr*) &target_,
        sizeof(target_));
    return sent == (int) bytes.size();
  }

private:
  void release() {
    close();
#if defined(_WIN32)
    if (wsaStarted_) WSACleanup();
    wsaStarted_ = false;
#endif
  }
  void take(Sender& other) noexcept {
    socket_ = other.socket_;
    other.socket_ = -1;
    target_ = other.target_;
    other.target_ = sockaddr_in{};
#if defined(_WIN32)
    wsaStarted_ = other.wsaStarted_;
    other.wsaStarted_ = false;
#endif
  }

  int socket_ = -1;
  sockaddr_in target_{};
#if defined(_WIN32)
  bool wsaStarted_ = false;
#endif
};

} // namespace osc
} // namespace sonore
