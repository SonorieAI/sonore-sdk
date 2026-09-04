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
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
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

/** What an argument can be. OSC defines more, but these four are what every
 *  controller sends and what a plugin can act on. */
enum class Type : char {
  Int32 = 'i',
  Float32 = 'f',
  String = 's',
  Blob = 'b',
};

struct Argument {
  Type type = Type::Float32;
  int32_t intValue = 0;
  float floatValue = 0.0f;
  std::string stringValue;
  std::vector<uint8_t> blobValue;

  static Argument makeInt(int32_t v) {
    Argument a;
    a.type = Type::Int32;
    a.intValue = v;
    return a;
  }
  static Argument makeFloat(float v) {
    Argument a;
    a.type = Type::Float32;
    a.floatValue = v;
    return a;
  }
  static Argument makeString(const std::string& v) {
    Argument a;
    a.type = Type::String;
    a.stringValue = v;
    return a;
  }
  static Argument makeBlob(const std::vector<uint8_t>& v) {
    Argument a;
    a.type = Type::Blob;
    a.blobValue = v;
    return a;
  }
};

struct Message {
  std::string address; // e.g. "/sonore/param/drive"
  std::vector<Argument> arguments;
};

namespace detail {

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

/** Strings are null-terminated and then padded to a 4-byte boundary: with at
 *  least ONE null, so a string whose length is already a multiple of four
 *  still gains four padding bytes. Getting that edge wrong shifts everything
 *  after it. */
inline void writeString(std::vector<uint8_t>& out, const std::string& s) {
  out.insert(out.end(), s.begin(), s.end());
  const size_t padded = (s.size() / 4 + 1) * 4;
  out.resize(out.size() + (padded - s.size()), 0);
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
        detail::writeString(out, a.stringValue);
        break;
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
      case 's': {
        if (!detail::readString(data, size, pos, &a.stringValue)) return false;
        a.type = Type::String;
        break;
      }
      case 'b': {
        uint32_t length = 0;
        if (!detail::readU32(data, size, pos, &length)) return false;
        if (pos + length > size) return false;
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

/** Decode a datagram that may be a bundle, flattening it to the messages
 *  inside. Bundles nest, so this recurses: bounded, because each level must
 *  consume bytes. */
inline bool decodePacket(const uint8_t* data, size_t size, std::vector<Message>* out) {
  if (!data || !out || size < 8) return false;
  if (std::memcmp(data, "#bundle", 7) == 0) {
    size_t pos = 16; // "#bundle\0" plus the 8-byte time tag
    while (pos + 4 <= size) {
      uint32_t length = 0;
      if (!detail::readU32(data, size, pos, &length)) return false;
      if (length == 0 || pos + length > size) return false;
      if (!decodePacket(data + pos, length, out)) return false;
      pos += length;
    }
    return true;
  }
  Message m;
  if (!decodeMessage(data, size, &m)) return false;
  out->push_back(std::move(m));
  return true;
}

// ── UDP transport ────────────────────────────────────────────────────────────
//
// Optional, and deliberately separate from the codec: a plugin that only wants
// to parse packets someone else delivered never opens a socket.

/** A non-blocking UDP receiver. poll() returns whatever arrived since the last
 *  call and never waits, so it is safe to call from a UI timer. */
class Receiver {
public:
  ~Receiver() { close(); }

  bool open(uint16_t port) {
#if defined(_WIN32)
    WSADATA wsa;
    if (!wsaStarted_ && WSAStartup(MAKEWORD(2, 2), &wsa) == 0) wsaStarted_ = true;
#endif
    socket_ = (int) ::socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_ < 0) {
      error_ = "could not create a UDP socket";
      return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind((SocketHandle) socket_, (sockaddr*) &addr, sizeof(addr)) != 0) {
      error_ = "the port is already in use";
      close();
      return false;
    }
    setNonBlocking();
    // Ask the OS which port it actually gave us, so port 0 (pick one) works.
    socklen_t len = sizeof(addr);
    if (::getsockname((SocketHandle) socket_, (sockaddr*) &addr, &len) == 0)
      port_ = ntohs(addr.sin_port);
    else
      port_ = port;
    return true;
  }

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
    uint8_t buffer[4096];
    for (int i = 0; i < 64; ++i) { // bounded: never spin on a flood
      const int got = (int) ::recv((SocketHandle) socket_, (char*) buffer, sizeof(buffer), 0);
      if (got <= 0) break;
      decodePacket(buffer, (size_t) got, &messages);
    }
    return messages;
  }

  uint16_t port() const { return port_; }
  const std::string& error() const { return error_; }

private:
#if defined(_WIN32)
  using SocketHandle = SOCKET;
#else
  using SocketHandle = int;
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
  std::string error_;
#if defined(_WIN32)
  bool wsaStarted_ = false;
#endif
};

/** Fire-and-forget UDP sender. */
class Sender {
public:
  ~Sender() { close(); }

  bool open(const char* host, uint16_t port) {
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    socket_ = (int) ::socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_ < 0) return false;
    std::memset(&target_, 0, sizeof(target_));
    target_.sin_family = AF_INET;
    target_.sin_port = htons(port);
    if (::inet_pton(AF_INET, host ? host : "127.0.0.1", &target_.sin_addr) != 1) {
      close();
      return false;
    }
    return true;
  }

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
  int socket_ = -1;
  sockaddr_in target_{};
};

} // namespace osc
} // namespace sonore
