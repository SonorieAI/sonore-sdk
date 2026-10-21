// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: networking: addresses, TCP and UDP sockets, URLs, and HTTP(S).
//
//   IPAddress, MACAddress          parse, format, and list this machine's
//   StreamingSocket                TCP: connect, listen, accept, read, write
//   DatagramSocket                 UDP: bind, send, receive, broadcast,
//                                  multicast
//   URL                            RFC 3986: parse, components, percent-
//                                  encoding, parameters, relative resolution
//   WebInputStream                 an HTTP(S) request read as a stream:
//                                  headers, POST bodies, timeouts, redirects,
//                                  progress, cancellation
//   URL::downloadToFile            the common case, in one call
//
// ── A plugin that talks to the network is doing something nobody asked for ──
//
// That position has not changed (see the feature map's networking row): a
// plugin opening a socket by itself, in a process the user did not start, on
// a studio machine that may have no business reaching the internet, is the
// kind of thing users find in a firewall log and do not forgive. So nothing
// in the SDK calls any of this on its own -- it is here for the plugin that
// genuinely needs it and says so to its user (a content downloader, a
// licence check, an editor talking to a companion app), and for the
// standalone.
//
// And the defaults lean shut: a listening socket binds the LOOPBACK address
// unless it is given another one, so a listener a plugin opened for its own
// helper process is not reachable from the café Wi-Fi the laptop is on --
// the same fix osc.h's receiver had. The reference listens on every
// interface by default; pass "0.0.0.0" (or "::") to do that.
//
// ── How each platform is reached ────────────────────────────────────────────
//
// Windows: Winsock, WinHTTP and iphlpapi are LOADED, not linked, and their
// handful of structures are declared here rather than by <winsock2.h>. That
// is not squeamishness about a link line. A project that includes
// <windows.h> without WIN32_LEAN_AND_MEAN before this header has already
// pulled in the obsolete <winsock.h>, after which <winsock2.h> cannot be
// included at all -- a hundred redefinition errors, decided by somebody
// else's include order. Declaring the dozen layouts we use (fixed in the
// Windows ABI since Windows 2000, and checked against the real headers by a
// static_assert in system_test.cpp) makes this header compile in any order
// with anything. HTTPS is WinHTTP: the system's TLS, the system's proxy
// settings and certificate store.
//
// macOS: BSD sockets, and NSURLSession for HTTP through the Objective-C
// runtime (no Objective-C++ needed, like the rest of the SDK's Cocoa code).
//
// Linux: BSD sockets, and libcurl for HTTP, opened with dlopen. Every desktop
// distribution has libcurl; a plugin that LINKED it would refuse to load on
// the one that does not, instead of failing one download with a message
// saying which library was missing.
//
// The parts that decide anything -- URL parsing and resolution, header
// parsing, redirect rules, address formatting -- are shared C++ and tested on
// every platform. What differs is the call at the bottom.
//
// ── Redirects are followed HERE, not by the platform ────────────────────────
//
// WinHTTP, NSURLSession and libcurl each follow redirects by their own rules
// and report them differently. All three are told not to, and the loop in
// WebInputStream::connect() follows them identically everywhere: Location
// resolved with RFC 3986 against the URL that answered, 303 (and 301/302
// after a POST) turned into a GET without its body, 307/308 repeated as
// they were, credentials dropped when the origin changes, at most
// numRedirectsToFollow hops -- after which the last 3xx is handed back as
// the response rather than an error, so a caller can see where it was going.
//
// ── NEVER ON THE AUDIO THREAD ───────────────────────────────────────────────
//
// Everything here blocks: name lookup (getaddrinfo cannot be given a
// timeout), connecting, reading, writing. Use a thread of your own, or a
// worker the editor polls. cancel() and the timeouts are how a blocked call
// is brought back; getaddrinfo is the one call they cannot shorten.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "file_types.h"
#include "streams.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <dlfcn.h>
#if defined(__APPLE__)
#include <net/if_dl.h>
#include <objc/message.h>
#include <objc/runtime.h>
#elif defined(__linux__)
#include <linux/if_packet.h>
#endif
#endif

namespace sonore {

// ═══════════════════════════════════════════════════════════════════════════
// Addresses
// ═══════════════════════════════════════════════════════════════════════════

/**
 * An IPv4 or IPv6 address.
 *
 * Text in, RFC 4291 forms: dotted quads ("192.0.2.1", no leading zeros --
 * "010.0.0.1" is octal to some parsers and decimal to others, so it is
 * refused rather than guessed), and IPv6 with "::" compression and an
 * embedded IPv4 tail ("::ffff:192.0.2.1"). Brackets and zone suffixes
 * ("%eth0") are not part of an address and are refused.
 *
 * Text out, RFC 5952 for IPv6: lowercase, no leading zeros, the LONGEST run
 * of two or more zero groups compressed (the first, on a tie), and an
 * IPv4-mapped address written with its dotted tail. Two programs printing
 * one address print the same string, which is what makes a string usable as
 * a key.
 */
class IPAddress {
public:
  /** Network byte order. IPv4 uses the first four. */
  uint8_t address[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  /** True for an IPv6 address, all sixteen bytes; false for IPv4. */
  bool isIPv6 = false;

  /** 0.0.0.0. */
  IPAddress() noexcept = default;
  /** The IPv4 address a0.a1.a2.a3. */
  IPAddress(uint8_t a0, uint8_t a1, uint8_t a2, uint8_t a3) noexcept {
    address[0] = a0;
    address[1] = a1;
    address[2] = a2;
    address[3] = a3;
  }
  /** An IPv4 address from a 32-bit value in the HOST's byte order:
   *  0x7f000001 is 127.0.0.1. */
  explicit IPAddress(uint32_t hostOrderIPv4) noexcept {
    for (int i = 0; i < 4; ++i) address[i] = (uint8_t) (hostOrderIPv4 >> (24 - 8 * i));
  }
  /** Four bytes, or sixteen when IPv6. */
  IPAddress(const uint8_t bytes[], bool IPv6) noexcept : isIPv6(IPv6) {
    if (bytes) std::memcpy(address, bytes, IPv6 ? 16 : 4);
  }
  /** Eight 16-bit groups, host order. */
  explicit IPAddress(const uint16_t groups[8]) noexcept : isIPv6(true) {
    for (int i = 0; i < 8; ++i) {
      address[2 * i] = (uint8_t) (groups[i] >> 8);
      address[2 * i + 1] = (uint8_t) groups[i];
    }
  }
  /** Parsed from text; 0.0.0.0 (isNull) when it is not an address -- use
   *  parse() to tell that apart from a real 0.0.0.0. */
  explicit IPAddress(const std::string& text) { parse(text, *this); }

  /** Text to address. False, and `out` untouched, when it is not one. */
  static bool parse(const std::string& text, IPAddress& out) {
    uint8_t v4[4];
    if (parseV4(text, v4)) {
      out = IPAddress(v4, false);
      return true;
    }
    uint8_t v6[16];
    if (parseV6(text, v6)) {
      out = IPAddress(v6, true);
      return true;
    }
    return false;
  }

  /** As text: dotted decimal for IPv4, RFC 5952 for IPv6 (see the class
   *  comment). No brackets and no port. */
  std::string toString() const {
    char buffer[64];
    if (!isIPv6) {
      std::snprintf(buffer, sizeof(buffer), "%u.%u.%u.%u", address[0], address[1], address[2], address[3]);
      return buffer;
    }
    if (isIPv4MappedAddress()) {
      std::snprintf(buffer, sizeof(buffer), "::ffff:%u.%u.%u.%u", address[12], address[13], address[14],
                    address[15]);
      return buffer;
    }
    uint16_t g[8];
    for (int i = 0; i < 8; ++i) g[i] = (uint16_t) ((address[2 * i] << 8) | address[2 * i + 1]);
    int bestStart = -1, bestLength = 0;
    for (int i = 0; i < 8;) {
      if (g[i] != 0) {
        ++i;
        continue;
      }
      int j = i;
      while (j < 8 && g[j] == 0) ++j;
      if (j - i > bestLength) {
        bestStart = i;
        bestLength = j - i;
      }
      i = j;
    }
    if (bestLength < 2) bestStart = -1; // one zero group is written, not compressed
    std::string out;
    for (int i = 0; i < 8;) {
      if (i == bestStart) {
        out += "::";
        i += bestLength;
        continue;
      }
      if (!out.empty() && out.back() != ':') out += ':';
      std::snprintf(buffer, sizeof(buffer), "%x", g[i]);
      out += buffer;
      ++i;
    }
    return out;
  }

  /** All zero: the "any" address of its family. */
  bool isNull() const noexcept {
    for (int i = 0; i < (isIPv6 ? 16 : 4); ++i)
      if (address[i]) return false;
    return true;
  }
  /** 127.0.0.0/8, ::1, or an IPv4-mapped 127.x.y.z. */
  bool isLoopback() const noexcept {
    if (!isIPv6) return address[0] == 127;
    if (isIPv4MappedAddress()) return address[12] == 127;
    for (int i = 0; i < 15; ++i)
      if (address[i]) return false;
    return address[15] == 1;
  }
  /** 224.0.0.0/4, ff00::/8, or an IPv4-mapped 224.0.0.0/4 address. */
  bool isMulticast() const noexcept {
    if (!isIPv6) return (address[0] & 0xF0) == 0xE0; // 224.0.0.0/4
    if (isIPv4MappedAddress()) return (address[12] & 0xF0) == 0xE0;
    return address[0] == 0xFF;
  }
  /** 169.254.0.0/16 or fe80::/10. */
  bool isLinkLocal() const noexcept {
    if (!isIPv6) return address[0] == 169 && address[1] == 254;
    return address[0] == 0xFE && (address[1] & 0xC0) == 0x80;
  }
  /** 255.255.255.255 only. A subnet's own broadcast address is not
   *  recognised here: getInterfaceBroadcastAddress() gives that. */
  bool isBroadcast() const noexcept {
    return !isIPv6 && address[0] == 255 && address[1] == 255 && address[2] == 255 && address[3] == 255;
  }
  /** \::ffff:a.b.c.d -- an IPv4 address in IPv6's clothes, as a dual-stack
   *  socket reports an IPv4 peer. */
  bool isIPv4MappedAddress() const noexcept {
    if (!isIPv6) return false;
    for (int i = 0; i < 10; ++i)
      if (address[i]) return false;
    return address[10] == 0xFF && address[11] == 0xFF;
  }

  /** The IPv4 address as a 32-bit value in host order (0 for IPv6 that is
   *  not IPv4-mapped). */
  uint32_t toIPv4() const noexcept {
    const uint8_t* a = isIPv6 ? (isIPv4MappedAddress() ? address + 12 : nullptr) : address;
    if (!a) return 0;
    return ((uint32_t) a[0] << 24) | ((uint32_t) a[1] << 16) | ((uint32_t) a[2] << 8) | a[3];
  }

  /** 0.0.0.0, or :: -- what binding to every interface means. */
  static IPAddress any(bool ipv6 = false) noexcept {
    IPAddress a;
    a.isIPv6 = ipv6;
    return a;
  }
  /** 255.255.255.255. */
  static IPAddress broadcast() noexcept { return IPAddress(255, 255, 255, 255); }
  /** The loopback: 127.0.0.1, or ::1. */
  static IPAddress local(bool ipv6 = false) noexcept {
    if (!ipv6) return IPAddress(127, 0, 0, 1);
    IPAddress a = any(true);
    a.address[15] = 1;
    return a;
  }
  /** \::ffff:a.b.c.d as the IPv4 address a.b.c.d; any other address comes
   *  back unchanged. */
  static IPAddress convertIPv4MappedAddressToIPv4(const IPAddress& mapped) noexcept {
    if (!mapped.isIPv4MappedAddress()) return mapped;
    return IPAddress(mapped.address[12], mapped.address[13], mapped.address[14], mapped.address[15]);
  }
  /** a.b.c.d as \::ffff:a.b.c.d; an IPv6 address comes back unchanged. */
  static IPAddress convertIPv4AddressToIPv4Mapped(const IPAddress& v4) noexcept {
    if (v4.isIPv6) return v4;
    IPAddress a = any(true);
    a.address[10] = a.address[11] = 0xFF;
    std::memcpy(a.address + 12, v4.address, 4);
    return a;
  }

  /** This machine's addresses on interfaces that are up, IPv4 first. */
  static std::vector<IPAddress> getAllAddresses(bool includeIPv6 = false);
  /** The first address that is not a loopback, or the loopback when there is
   *  none (a machine with no network). */
  static IPAddress getLocalAddress(bool includeIPv6 = false);
  /** The broadcast address of the IPv4 interface that has `interfaceAddress`
   *  (192.168.1.255 for 192.168.1.20/24), or a null address. */
  static IPAddress getInterfaceBroadcastAddress(const IPAddress& interfaceAddress);

  /** Same family and same bytes, so 127.0.0.1 and \::ffff:127.0.0.1 are
   *  different addresses here. */
  bool operator==(const IPAddress& other) const noexcept {
    return isIPv6 == other.isIPv6 && std::memcmp(address, other.address, isIPv6 ? 16 : 4) == 0;
  }
  /** Not operator==. */
  bool operator!=(const IPAddress& other) const noexcept { return !(*this == other); }
  /** IPv4 before IPv6, then byte by byte: an order for sorting and map
   *  keys. */
  bool operator<(const IPAddress& other) const noexcept {
    if (isIPv6 != other.isIPv6) return !isIPv6;
    return std::memcmp(address, other.address, isIPv6 ? 16 : 4) < 0;
  }

private:
  static bool parseV4(const std::string& s, uint8_t out[4]) {
    int part = 0, value = 0, digits = 0;
    for (char c : s) {
      if (c >= '0' && c <= '9') {
        if (digits > 0 && value == 0) return false; // a leading zero
        value = value * 10 + (c - '0');
        if (++digits > 3 || value > 255) return false;
      } else if (c == '.') {
        if (digits == 0 || part >= 3) return false;
        out[part++] = (uint8_t) value;
        value = digits = 0;
      } else {
        return false;
      }
    }
    if (digits == 0 || part != 3) return false;
    out[3] = (uint8_t) value;
    return true;
  }

  static int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  }

  static bool parseV6(const std::string& s, uint8_t out[16]) {
    if (s.size() < 2 || s.size() > 45) return false;
    uint16_t groups[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    int count = 0, gap = -1;
    size_t i = 0;
    if (s[0] == ':') {
      if (s[1] != ':') return false;
      gap = 0;
      i = 2;
    }
    while (i < s.size()) {
      if (count >= 8) return false;
      size_t end = s.find(':', i);
      if (end == std::string::npos) end = s.size();
      const std::string segment = s.substr(i, end - i);
      if (segment.find('.') != std::string::npos) {
        // The dotted IPv4 tail: last, and only where two groups are left.
        uint8_t v4[4];
        if (end != s.size() || count > 6 || !parseV4(segment, v4)) return false;
        groups[count++] = (uint16_t) ((v4[0] << 8) | v4[1]);
        groups[count++] = (uint16_t) ((v4[2] << 8) | v4[3]);
        i = s.size();
        break;
      }
      if (segment.empty() || segment.size() > 4) return false;
      int value = 0;
      for (char c : segment) {
        const int h = hexValue(c);
        if (h < 0) return false;
        value = value * 16 + h;
      }
      groups[count++] = (uint16_t) value;
      i = end;
      if (i < s.size()) {
        ++i; // the ':'
        if (i < s.size() && s[i] == ':') {
          if (gap >= 0) return false; // "::" twice
          gap = count;
          ++i;
        } else if (i == s.size()) {
          return false; // a trailing single ':'
        }
      }
    }
    if (gap < 0) {
      if (count != 8) return false;
    } else {
      if (count > 7) return false; // "::" stands for at least one group
      const int tail = count - gap;
      for (int k = 0; k < tail; ++k) groups[7 - k] = groups[count - 1 - k];
      for (int k = gap; k < 8 - tail; ++k) groups[k] = 0;
    }
    for (int k = 0; k < 8; ++k) {
      out[2 * k] = (uint8_t) (groups[k] >> 8);
      out[2 * k + 1] = (uint8_t) groups[k];
    }
    return true;
  }
};

/** A network adapter's hardware address: six bytes, written "aa-bb-cc-dd-ee-ff". */
class MACAddress {
public:
  /** The null address, 00-00-00-00-00-00. */
  MACAddress() noexcept = default;
  /** From six bytes; a null pointer gives the null address. */
  explicit MACAddress(const uint8_t bytes[6]) noexcept {
    if (bytes) std::memcpy(address_, bytes, 6);
  }
  /** From text: twelve hex digits, with any separators (or none) between --
   *  "aa:bb:cc:dd:ee:ff", "AA-BB-CC-DD-EE-FF", "aabb.ccdd.eeff". Anything else
   *  gives the null address. */
  explicit MACAddress(const std::string& text) {
    uint8_t bytes[6] = {0, 0, 0, 0, 0, 0};
    int nibbles = 0;
    for (char c : text) {
      int v = -1;
      if (c >= '0' && c <= '9') v = c - '0';
      else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
      if (v < 0) continue;
      if (nibbles >= 12) return;
      bytes[nibbles / 2] = (uint8_t) (bytes[nibbles / 2] | (v << ((nibbles & 1) ? 0 : 4)));
      ++nibbles;
    }
    if (nibbles == 12) std::memcpy(address_, bytes, 6);
  }

  /** The six bytes, valid while this object is. */
  const uint8_t* getBytes() const noexcept { return address_; }
  /** Lowercase hex pairs joined by `separator`: "aa-bb-cc-dd-ee-ff". */
  std::string toString(const std::string& separator = "-") const {
    std::string out;
    char hex[3];
    for (int i = 0; i < 6; ++i) {
      if (i) out += separator;
      std::snprintf(hex, sizeof(hex), "%02x", address_[i]);
      out += hex;
    }
    return out;
  }
  /** The six bytes as one number, first byte highest. */
  int64_t toInt64() const noexcept {
    int64_t v = 0;
    for (int i = 0; i < 6; ++i) v = (v << 8) | address_[i];
    return v;
  }
  /** All six bytes zero: no address, or text that did not parse. */
  bool isNull() const noexcept {
    for (uint8_t b : address_)
      if (b) return false;
    return true;
  }
  /** The same six bytes. */
  bool operator==(const MACAddress& other) const noexcept { return std::memcmp(address_, other.address_, 6) == 0; }
  /** Not operator==. */
  bool operator!=(const MACAddress& other) const noexcept { return !(*this == other); }

  /** The hardware addresses of this machine's network adapters (not the
   *  loopback, not tunnels; no duplicates). */
  static std::vector<MACAddress> getAllAddresses();

private:
  uint8_t address_[6] = {0, 0, 0, 0, 0, 0};
};

// ═══════════════════════════════════════════════════════════════════════════
// The platform's sockets
// ═══════════════════════════════════════════════════════════════════════════

namespace netdetail {

#if defined(_WIN32)
using SocketHandle = uintptr_t; // SOCKET
const SocketHandle kInvalidSocket = (SocketHandle) ~(uintptr_t) 0;

// Winsock's values, from winsock2.h / ws2def.h / ws2ipdef.h. Checked against
// those headers in system_test.cpp.
const int kAfUnspec = 0, kAfInet = 2, kAfInet6 = 23;
const int kSockStream = 1, kSockDgram = 2;
const int kIpprotoIp = 0, kIpprotoTcp = 6, kIpprotoUdp = 17, kIpprotoIpv6 = 41;
const int kSolSocket = 0xffff, kSoReuseAddr = 0x0004, kSoBroadcast = 0x0020, kSoError = 0x1007;
const int kSoExclusiveAddrUse = (int) (~0x0004);
const int kTcpNoDelay = 0x0001;
const int kIpMulticastLoop = 11, kIpAddMembership = 12, kIpDropMembership = 13;
const int kIpv6MulticastLoop = 11, kIpv6JoinGroup = 12, kIpv6LeaveGroup = 13, kIpv6V6Only = 27;
const int kAiPassive = 0x1, kAiNumericHost = 0x4;
const long kFionbio = (long) 0x8004667EUL;
const unsigned long kSioUdpConnReset = 0x9800000CUL;
const int kShutdownBoth = 2;
const int kWsaEWouldBlock = 10035, kWsaEInProgress = 10036, kWsaEMsgSize = 10040, kWsaEConnReset = 10054;

struct WsSockAddr {
  uint16_t family;
  char data[14];
};
struct WsSockAddrIn {
  uint16_t family;
  uint16_t port; // network order
  uint8_t addr[4];
  char zero[8];
};
struct WsSockAddrIn6 {
  uint16_t family;
  uint16_t port;
  uint32_t flowInfo;
  uint8_t addr[16];
  uint32_t scopeId;
};
struct WsAddrInfo {
  int flags;
  int family;
  int sockType;
  int protocol;
  size_t addrLength;
  char* canonName;
  WsSockAddr* addr;
  WsAddrInfo* next;
};
struct WsTimeval {
  long seconds;
  long microseconds;
};
struct WsFdSet {
  unsigned int count;
  SocketHandle sockets[64];
};
struct WsIpMreq {
  uint8_t multiaddr[4];
  uint8_t iface[4];
};
struct WsIpv6Mreq {
  uint8_t multiaddr[16];
  unsigned long iface;
};
struct WsSocketAddress {
  WsSockAddr* sockaddr;
  int length;
};
// IP_ADAPTER_UNICAST_ADDRESS_LH, whole, and the leading part of
// IP_ADAPTER_ADDRESSES_LH that is read here. The system allocates the full
// structures in the caller's buffer; reading a prefix of one is safe.
struct WsUnicastAddress {
  uint64_t alignment; // Length, Flags
  WsUnicastAddress* next;
  WsSocketAddress address;
  int prefixOrigin;
  int suffixOrigin;
  int dadState;
  unsigned long validLifetime;
  unsigned long preferredLifetime;
  unsigned long leaseLifetime;
  uint8_t onLinkPrefixLength;
};
struct WsAdapterAddresses {
  uint64_t alignment; // Length, IfIndex
  WsAdapterAddresses* next;
  char* adapterName;
  WsUnicastAddress* firstUnicastAddress;
  void* firstAnycastAddress;
  void* firstMulticastAddress;
  void* firstDnsServerAddress;
  wchar_t* dnsSuffix;
  wchar_t* description;
  wchar_t* friendlyName;
  uint8_t physicalAddress[8];
  unsigned long physicalAddressLength;
  unsigned long flags;
  unsigned long mtu;
  unsigned long ifType;
  int operStatus;
};
const unsigned long kIfTypeSoftwareLoopback = 24, kIfTypeTunnel = 131;
const int kIfOperStatusUp = 1;

/** Winsock's entry points, resolved once from ws2_32.dll -- which every
 *  Windows process can load and nearly every host already has. Kept loaded
 *  for the life of the process: the pointers are cached. */
struct WinsockApi {
  bool ok = false;
  int(__stdcall* startup)(uint16_t, void*) = nullptr;
  int(__stdcall* cleanup)() = nullptr;
  int(__stdcall* lastError)() = nullptr;
  SocketHandle(__stdcall* socket)(int, int, int) = nullptr;
  int(__stdcall* closesocket)(SocketHandle) = nullptr;
  int(__stdcall* bind)(SocketHandle, const WsSockAddr*, int) = nullptr;
  int(__stdcall* listen)(SocketHandle, int) = nullptr;
  SocketHandle(__stdcall* accept)(SocketHandle, WsSockAddr*, int*) = nullptr;
  int(__stdcall* connect)(SocketHandle, const WsSockAddr*, int) = nullptr;
  int(__stdcall* send)(SocketHandle, const char*, int, int) = nullptr;
  int(__stdcall* recv)(SocketHandle, char*, int, int) = nullptr;
  int(__stdcall* sendto)(SocketHandle, const char*, int, int, const WsSockAddr*, int) = nullptr;
  int(__stdcall* recvfrom)(SocketHandle, char*, int, int, WsSockAddr*, int*) = nullptr;
  int(__stdcall* shutdown)(SocketHandle, int) = nullptr;
  int(__stdcall* setsockopt)(SocketHandle, int, int, const char*, int) = nullptr;
  int(__stdcall* getsockopt)(SocketHandle, int, int, char*, int*) = nullptr;
  int(__stdcall* getsockname)(SocketHandle, WsSockAddr*, int*) = nullptr;
  int(__stdcall* getpeername)(SocketHandle, WsSockAddr*, int*) = nullptr;
  int(__stdcall* ioctlsocket)(SocketHandle, long, unsigned long*) = nullptr;
  int(__stdcall* select)(int, WsFdSet*, WsFdSet*, WsFdSet*, const WsTimeval*) = nullptr;
  int(__stdcall* getaddrinfo)(const char*, const char*, const WsAddrInfo*, WsAddrInfo**) = nullptr;
  void(__stdcall* freeaddrinfo)(WsAddrInfo*) = nullptr;
  int(__stdcall* ioctl)(SocketHandle, unsigned long, void*, unsigned long, void*, unsigned long, unsigned long*,
                        void*, void*) = nullptr; // WSAIoctl
};

template <typename Fn>
inline void resolveProc(HMODULE module, const char* name, Fn& out) {
  out = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(module, name)));
}

inline const WinsockApi& winsock() {
  static const WinsockApi api = [] {
    WinsockApi a;
    HMODULE m = LoadLibraryExW(L"ws2_32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) return a;
    resolveProc(m, "WSAStartup", a.startup);
    resolveProc(m, "WSACleanup", a.cleanup);
    resolveProc(m, "WSAGetLastError", a.lastError);
    resolveProc(m, "socket", a.socket);
    resolveProc(m, "closesocket", a.closesocket);
    resolveProc(m, "bind", a.bind);
    resolveProc(m, "listen", a.listen);
    resolveProc(m, "accept", a.accept);
    resolveProc(m, "connect", a.connect);
    resolveProc(m, "send", a.send);
    resolveProc(m, "recv", a.recv);
    resolveProc(m, "sendto", a.sendto);
    resolveProc(m, "recvfrom", a.recvfrom);
    resolveProc(m, "shutdown", a.shutdown);
    resolveProc(m, "setsockopt", a.setsockopt);
    resolveProc(m, "getsockopt", a.getsockopt);
    resolveProc(m, "getsockname", a.getsockname);
    resolveProc(m, "getpeername", a.getpeername);
    resolveProc(m, "ioctlsocket", a.ioctlsocket);
    resolveProc(m, "select", a.select);
    resolveProc(m, "getaddrinfo", a.getaddrinfo);
    resolveProc(m, "freeaddrinfo", a.freeaddrinfo);
    resolveProc(m, "WSAIoctl", a.ioctl);
    a.ok = a.startup && a.cleanup && a.lastError && a.socket && a.closesocket && a.bind && a.listen && a.accept &&
           a.connect && a.send && a.recv && a.sendto && a.recvfrom && a.shutdown && a.setsockopt && a.getsockopt &&
           a.getsockname && a.getpeername && a.ioctlsocket && a.select && a.getaddrinfo && a.freeaddrinfo;
    return a;
  }();
  return api;
}

/** One WSAStartup per object that uses Winsock, and its WSACleanup when the
 *  object goes: Winsock counts them, so it is torn down when the plugin's
 *  last socket is -- not left initialised in the host after the plugin has
 *  gone, which is what osc.h's sockets were fixed for. */
class WinsockUser {
public:
  WinsockUser() noexcept {
    const WinsockApi& api = winsock();
    struct alignas(8) WsaData {
      unsigned char bytes[512]; // WSADATA is 408 bytes on x64
    } data;
    started_ = api.ok && api.startup(0x0202, &data) == 0;
  }
  ~WinsockUser() {
    if (started_) winsock().cleanup();
  }
  WinsockUser(const WinsockUser&) = delete;
  WinsockUser& operator=(const WinsockUser&) = delete;
  WinsockUser(WinsockUser&& other) noexcept : started_(other.started_) { other.started_ = false; }
  WinsockUser& operator=(WinsockUser&& other) noexcept {
    std::swap(started_, other.started_);
    return *this;
  }
  bool ok() const noexcept { return started_; }

private:
  bool started_ = false;
};

inline int lastSocketError() { return winsock().lastError ? winsock().lastError() : -1; }
inline bool isWouldBlock(int e) { return e == kWsaEWouldBlock || e == kWsaEInProgress; }

#else // POSIX

using SocketHandle = int;
const SocketHandle kInvalidSocket = -1;
const int kAfUnspec = AF_UNSPEC, kAfInet = AF_INET, kAfInet6 = AF_INET6;
const int kSockStream = SOCK_STREAM, kSockDgram = SOCK_DGRAM;
const int kIpprotoIp = IPPROTO_IP, kIpprotoTcp = IPPROTO_TCP, kIpprotoUdp = IPPROTO_UDP,
          kIpprotoIpv6 = IPPROTO_IPV6;
const int kSolSocket = SOL_SOCKET, kSoReuseAddr = SO_REUSEADDR, kSoBroadcast = SO_BROADCAST, kSoError = SO_ERROR;
const int kTcpNoDelay = TCP_NODELAY;
const int kIpMulticastLoop = IP_MULTICAST_LOOP, kIpAddMembership = IP_ADD_MEMBERSHIP,
          kIpDropMembership = IP_DROP_MEMBERSHIP;
const int kIpv6MulticastLoop = IPV6_MULTICAST_LOOP, kIpv6JoinGroup = IPV6_JOIN_GROUP,
          kIpv6LeaveGroup = IPV6_LEAVE_GROUP, kIpv6V6Only = IPV6_V6ONLY;
const int kAiPassive = AI_PASSIVE, kAiNumericHost = AI_NUMERICHOST;
const int kShutdownBoth = SHUT_RDWR;

class WinsockUser {
public:
  WinsockUser() noexcept {} // user-provided: an RAII local, not an unused one
  ~WinsockUser() {}
  WinsockUser(const WinsockUser&) = delete;
  WinsockUser& operator=(const WinsockUser&) = delete;
  WinsockUser(WinsockUser&&) noexcept {}
  WinsockUser& operator=(WinsockUser&&) noexcept { return *this; }
  bool ok() const noexcept { return true; }
};

inline int lastSocketError() { return errno; }
inline bool isWouldBlock(int e) { return e == EAGAIN || e == EWOULDBLOCK || e == EINPROGRESS; }
#endif

/** A socket address big enough for either family, plus its length. */
struct SockAddr {
  alignas(8) unsigned char bytes[128] = {};
  int length = 0;
};

inline uint16_t toNetwork16(int port) {
  const uint16_t p = (uint16_t) port;
  const uint8_t b[2] = {(uint8_t) (p >> 8), (uint8_t) (p & 0xff)};
  uint16_t out;
  std::memcpy(&out, b, 2);
  return out;
}
inline int fromNetwork16(uint16_t v) {
  uint8_t b[2];
  std::memcpy(b, &v, 2);
  return (b[0] << 8) | b[1];
}

inline SockAddr makeSockAddr(const IPAddress& ip, int port) {
  SockAddr out;
#if defined(_WIN32)
  if (ip.isIPv6) {
    WsSockAddrIn6 a{};
    a.family = (uint16_t) kAfInet6;
    a.port = toNetwork16(port);
    std::memcpy(a.addr, ip.address, 16);
    std::memcpy(out.bytes, &a, sizeof(a));
    out.length = (int) sizeof(a);
  } else {
    WsSockAddrIn a{};
    a.family = (uint16_t) kAfInet;
    a.port = toNetwork16(port);
    std::memcpy(a.addr, ip.address, 4);
    std::memcpy(out.bytes, &a, sizeof(a));
    out.length = (int) sizeof(a);
  }
#else
  if (ip.isIPv6) {
    sockaddr_in6 a;
    std::memset(&a, 0, sizeof(a));
#if defined(__APPLE__)
    a.sin6_len = (uint8_t) sizeof(a);
#endif
    a.sin6_family = AF_INET6;
    a.sin6_port = toNetwork16(port);
    std::memcpy(&a.sin6_addr, ip.address, 16);
    std::memcpy(out.bytes, &a, sizeof(a));
    out.length = (int) sizeof(a);
  } else {
    sockaddr_in a;
    std::memset(&a, 0, sizeof(a));
#if defined(__APPLE__)
    a.sin_len = (uint8_t) sizeof(a);
#endif
    a.sin_family = AF_INET;
    a.sin_port = toNetwork16(port);
    std::memcpy(&a.sin_addr, ip.address, 4);
    std::memcpy(out.bytes, &a, sizeof(a));
    out.length = (int) sizeof(a);
  }
#endif
  return out;
}

/** The address and port in a socket address; false for another family. */
inline bool readSockAddr(const void* sockaddrBytes, IPAddress& ip, int& port) {
  if (!sockaddrBytes) return false;
#if defined(_WIN32)
  uint16_t family;
  std::memcpy(&family, sockaddrBytes, 2);
  if (family == kAfInet) {
    WsSockAddrIn a;
    std::memcpy(&a, sockaddrBytes, sizeof(a));
    ip = IPAddress(a.addr, false);
    port = fromNetwork16(a.port);
    return true;
  }
  if (family == kAfInet6) {
    WsSockAddrIn6 a;
    std::memcpy(&a, sockaddrBytes, sizeof(a));
    ip = IPAddress(a.addr, true);
    port = fromNetwork16(a.port);
    return true;
  }
#else
  sockaddr sa;
  std::memcpy(&sa, sockaddrBytes, sizeof(sa));
  if (sa.sa_family == AF_INET) {
    sockaddr_in a;
    std::memcpy(&a, sockaddrBytes, sizeof(a));
    ip = IPAddress((const uint8_t*) &a.sin_addr, false);
    port = fromNetwork16(a.sin_port);
    return true;
  }
  if (sa.sa_family == AF_INET6) {
    sockaddr_in6 a;
    std::memcpy(&a, sockaddrBytes, sizeof(a));
    ip = IPAddress((const uint8_t*) &a.sin6_addr, true);
    port = fromNetwork16(a.sin6_port);
    return true;
  }
#endif
  return false;
}

#if defined(_WIN32)
/** Winsock's sockets are INHERITABLE handles, unlike POSIX descriptors opened
 *  with SOCK_CLOEXEC below. Left so, any process the host (or another plugin,
 *  or the CRT's system()) starts with bInheritHandles inherits every socket
 *  this SDK has open -- and a listener closed here stays listening in that
 *  child, a connection closed here never ends for its peer. */
inline void makeNotInheritable(SocketHandle s) {
  if (s != kInvalidSocket) SetHandleInformation((HANDLE) s, HANDLE_FLAG_INHERIT, 0);
}
#endif

inline SocketHandle openSocket(int family, int type, int protocol) {
#if defined(_WIN32)
  if (!winsock().ok) return kInvalidSocket;
  const SocketHandle s = winsock().socket(family, type, protocol);
  makeNotInheritable(s);
  return s;
#else
#if defined(SOCK_CLOEXEC)
  SocketHandle s = ::socket(family, type | SOCK_CLOEXEC, protocol);
#else
  SocketHandle s = ::socket(family, type, protocol);
  if (s >= 0) ::fcntl(s, F_SETFD, FD_CLOEXEC);
#endif
#if defined(SO_NOSIGPIPE)
  // A write to a peer that has gone must not raise SIGPIPE: its default
  // action ends the process, and the process is the user's DAW.
  if (s >= 0) {
    const int one = 1;
    ::setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
  }
#endif
  return s;
#endif
}

inline void closeSocket(SocketHandle s) {
  if (s == kInvalidSocket) return;
#if defined(_WIN32)
  winsock().closesocket(s);
#else
  ::close(s);
#endif
}

inline void shutdownSocket(SocketHandle s) {
  if (s == kInvalidSocket) return;
#if defined(_WIN32)
  winsock().shutdown(s, kShutdownBoth);
#else
  ::shutdown(s, kShutdownBoth);
#endif
}

/** Shuts a socket AND makes a send or receive blocked in it on another thread
 *  return, without closing it (a closed number can be reused under that
 *  thread). POSIX shutdown does both; Windows' does not wake a blocking
 *  send whose peer has stopped reading, and its pending I/O is cancelled. */
inline void shutdownAndWake(SocketHandle s) {
  if (s == kInvalidSocket) return;
  shutdownSocket(s);
#if defined(_WIN32)
  CancelIoEx((HANDLE) s, nullptr);
#endif
}

inline bool setIntOption(SocketHandle s, int level, int option, int value) {
#if defined(_WIN32)
  return winsock().setsockopt(s, level, option, (const char*) &value, (int) sizeof(value)) == 0;
#else
  return ::setsockopt(s, level, option, &value, sizeof(value)) == 0;
#endif
}

inline bool setBlocking(SocketHandle s, bool blocking) {
#if defined(_WIN32)
  unsigned long mode = blocking ? 0 : 1;
  return winsock().ioctlsocket(s, kFionbio, &mode) == 0;
#else
  const int flags = ::fcntl(s, F_GETFL, 0);
  if (flags < 0) return false;
  return ::fcntl(s, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK)) == 0;
#endif
}

/** 1 when ready, 0 on timeout, -1 on error. A negative timeout waits forever. */
inline int waitForSocket(SocketHandle s, bool forReading, int timeoutMs) {
  if (s == kInvalidSocket) return -1;
#if defined(_WIN32)
  WsFdSet set{};
  set.count = 1;
  set.sockets[0] = s;
  WsFdSet errors{};
  errors.count = 1;
  errors.sockets[0] = s;
  WsTimeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
  // The exception set only when waiting to WRITE, which is how a connect is
  // waited for. Waiting to read, it also reports out-of-band data: a single
  // urgent byte from the peer, which recv without MSG_OOB never returns,
  // made this say "ready" at once, every time, while the read that followed
  // blocked until ordinary data came -- InterprocessConnection's reader
  // among them, out of reach of its stop flag.
  const int r = winsock().select(0, forReading ? &set : nullptr, forReading ? nullptr : &set,
                                 forReading ? nullptr : &errors, timeoutMs < 0 ? nullptr : &tv);
  if (r < 0) return -1;
  if (r == 0) return 0;
  // A failed connect is reported in the exception set on Windows, not as
  // writability: ready, and the caller's SO_ERROR says why.
  return 1;
#else
  pollfd p;
  p.fd = s;
  p.events = (short) (forReading ? POLLIN : POLLOUT);
  p.revents = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs < 0 ? 0 : timeoutMs);
  for (;;) {
    int wait = timeoutMs;
    if (timeoutMs > 0) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
      wait = left > 0 ? (int) left : 0;
    }
    const int r = ::poll(&p, 1, wait);
    if (r < 0 && errno == EINTR) continue;
    if (r < 0) return -1;
    if (r == 0) return 0;
    // HUP and ERR count as ready: the read or write that follows reports it.
    return 1;
  }
#endif
}

inline int sendBytes(SocketHandle s, const void* data, int n) {
#if defined(_WIN32)
  return winsock().send(s, (const char*) data, n, 0);
#else
#if defined(MSG_NOSIGNAL)
  const int flags = MSG_NOSIGNAL; // see openSocket: never SIGPIPE
#else
  const int flags = 0;
#endif
  for (;;) {
    const ssize_t r = ::send(s, data, (size_t) n, flags);
    if (r < 0 && errno == EINTR) continue;
    return (int) r;
  }
#endif
}

inline int recvBytes(SocketHandle s, void* data, int n) {
#if defined(_WIN32)
  return winsock().recv(s, (char*) data, n, 0);
#else
  for (;;) {
    const ssize_t r = ::recv(s, data, (size_t) n, 0);
    if (r < 0 && errno == EINTR) continue;
    return (int) r;
  }
#endif
}

/** The local address a socket is bound to. */
inline bool localAddressOf(SocketHandle s, IPAddress& ip, int& port) {
  SockAddr a;
#if defined(_WIN32)
  int len = (int) sizeof(a.bytes);
  if (winsock().getsockname(s, (WsSockAddr*) a.bytes, &len) != 0) return false;
#else
  socklen_t len = (socklen_t) sizeof(a.bytes);
  if (::getsockname(s, (sockaddr*) a.bytes, &len) != 0) return false;
#endif
  return readSockAddr(a.bytes, ip, port);
}

/**
 * Addresses for a host name, as getaddrinfo gives them (its order is RFC
 * 6724's preference, so trying them in turn is the right thing). A numeric
 * address is answered without a lookup. Blocks for as long as the system
 * resolver takes, which no timeout here can shorten.
 */
inline std::vector<std::pair<IPAddress, int>> resolveHost(const std::string& host, int port, bool stream,
                                                          bool passive = false) {
  std::vector<std::pair<IPAddress, int>> out;
  IPAddress numeric;
  std::string name = host;
  if (name.size() > 2 && name.front() == '[' && name.back() == ']') name = name.substr(1, name.size() - 2);
  if (IPAddress::parse(name, numeric)) {
    out.emplace_back(numeric, port);
    return out;
  }
  if (name.empty()) return out;
  // "localhost" is this SDK's listeners' address -- the IPv4 loopback --
  // first. The resolver's order puts ::1 first, and on Windows a refused
  // connection is retried for two seconds before it fails.
  if (name == "localhost") {
    out.emplace_back(IPAddress::local(false), port);
    out.emplace_back(IPAddress::local(true), port);
    return out;
  }
  char service[16];
  std::snprintf(service, sizeof(service), "%d", port);
#if defined(_WIN32)
  if (!winsock().ok) return out;
  WsAddrInfo hints{};
  hints.family = kAfUnspec;
  hints.sockType = stream ? kSockStream : kSockDgram;
  hints.flags = passive ? kAiPassive : 0;
  WsAddrInfo* list = nullptr;
  if (winsock().getaddrinfo(name.c_str(), service, &hints, &list) != 0 || !list) return out;
  for (WsAddrInfo* p = list; p; p = p->next) {
    IPAddress ip;
    int prt = 0;
    if (p->addr && readSockAddr(p->addr, ip, prt)) out.emplace_back(ip, prt);
  }
  winsock().freeaddrinfo(list);
#else
  addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = stream ? SOCK_STREAM : SOCK_DGRAM;
  hints.ai_flags = passive ? AI_PASSIVE : 0;
  addrinfo* list = nullptr;
  if (::getaddrinfo(name.c_str(), service, &hints, &list) != 0 || !list) return out;
  for (addrinfo* p = list; p; p = p->ai_next) {
    IPAddress ip;
    int prt = 0;
    if (p->ai_addr && readSockAddr(p->ai_addr, ip, prt)) out.emplace_back(ip, prt);
  }
  ::freeaddrinfo(list);
#endif
  // The same address can come back once per protocol; once is enough.
  std::vector<std::pair<IPAddress, int>> unique;
  for (const auto& a : out) {
    bool seen = false;
    for (const auto& u : unique) seen = seen || u.first == a.first;
    if (!seen) unique.push_back(a);
  }
  return unique;
}

/** The address a listener binds when it is given none, or "localhost": the
 *  loopback. See the header. */
inline bool listenAddress(const std::string& host, IPAddress& out) {
  if (host.empty() || host == "localhost") {
    out = IPAddress::local();
    return true;
  }
  if (IPAddress::parse(host, out)) return true;
  const auto found = resolveHost(host, 0, true, true);
  if (found.empty()) return false;
  out = found.front().first;
  return true;
}

/** One adapter as the enumerations below see it. */
struct Interface {
  std::string name;
  std::vector<IPAddress> addresses;
  std::vector<IPAddress> broadcasts; // per IPv4 address, null where none
  MACAddress mac;
  bool loopback = false;
};

inline std::vector<Interface> listInterfaces() {
  std::vector<Interface> out;
#if defined(_WIN32)
  using GetAdaptersFn = unsigned long(__stdcall*)(unsigned long, unsigned long, void*, WsAdapterAddresses*,
                                                  unsigned long*);
  static GetAdaptersFn getAdapters = [] {
    GetAdaptersFn fn = nullptr;
    if (HMODULE m = LoadLibraryExW(L"iphlpapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) resolveProc(m, "GetAdaptersAddresses", fn);
    return fn;
  }();
  if (!getAdapters) return out;
  // Skip anycast, multicast, DNS servers and friendly names: none is read.
  const unsigned long flags = 0x2 | 0x4 | 0x8 | 0x20;
  unsigned long size = 16384;
  std::vector<uint64_t> buffer;
  unsigned long result = 111; // ERROR_BUFFER_OVERFLOW
  for (int attempt = 0; attempt < 4 && result == 111; ++attempt) {
    buffer.assign(size / sizeof(uint64_t) + 1, 0);
    result = getAdapters(0 /* AF_UNSPEC */, flags, nullptr, (WsAdapterAddresses*) buffer.data(), &size);
  }
  if (result != 0) return out;
  for (WsAdapterAddresses* a = (WsAdapterAddresses*) buffer.data(); a; a = a->next) {
    if (a->operStatus != kIfOperStatusUp) continue;
    Interface i;
    i.name = a->adapterName ? a->adapterName : "";
    i.loopback = a->ifType == kIfTypeSoftwareLoopback;
    if (a->physicalAddressLength == 6 && !i.loopback && a->ifType != kIfTypeTunnel) i.mac = MACAddress(a->physicalAddress);
    for (WsUnicastAddress* u = a->firstUnicastAddress; u; u = u->next) {
      IPAddress ip;
      int port = 0;
      if (!u->address.sockaddr || !readSockAddr(u->address.sockaddr, ip, port)) continue;
      i.addresses.push_back(ip);
      IPAddress bcast;
      if (!ip.isIPv6 && u->onLinkPrefixLength > 0 && u->onLinkPrefixLength < 32) {
        const uint32_t mask = 0xFFFFFFFFu << (32 - u->onLinkPrefixLength);
        bcast = IPAddress((ip.toIPv4() & mask) | ~mask);
      }
      i.broadcasts.push_back(bcast);
    }
    out.push_back(std::move(i));
  }
#else
  ifaddrs* list = nullptr;
  if (::getifaddrs(&list) != 0 || !list) return out;
  auto find = [&out](const char* name) -> Interface& {
    for (Interface& i : out)
      if (i.name == name) return i;
    out.emplace_back();
    out.back().name = name ? name : "";
    return out.back();
  };
  for (ifaddrs* p = list; p; p = p->ifa_next) {
    if (!p->ifa_addr || !(p->ifa_flags & IFF_UP)) continue;
    Interface& i = find(p->ifa_name);
    if (p->ifa_flags & IFF_LOOPBACK) i.loopback = true;
    const int family = p->ifa_addr->sa_family;
    if (family == AF_INET || family == AF_INET6) {
      IPAddress ip;
      int port = 0;
      if (!readSockAddr(p->ifa_addr, ip, port)) continue;
      i.addresses.push_back(ip);
      IPAddress bcast;
      int bport = 0;
      if (family == AF_INET && (p->ifa_flags & IFF_BROADCAST) && p->ifa_broadaddr)
        readSockAddr(p->ifa_broadaddr, bcast, bport);
      i.broadcasts.push_back(bcast);
    }
#if defined(__APPLE__)
    else if (family == AF_LINK) {
      const sockaddr_dl* dl = (const sockaddr_dl*) p->ifa_addr;
      if (dl->sdl_alen == 6) i.mac = MACAddress((const uint8_t*) LLADDR(dl));
    }
#elif defined(__linux__)
    else if (family == AF_PACKET) {
      const sockaddr_ll* ll = (const sockaddr_ll*) p->ifa_addr;
      if (ll->sll_halen == 6) i.mac = MACAddress(ll->sll_addr);
    }
#endif
  }
  ::freeifaddrs(list);
  // getifaddrs lists an interface once per address; the loopback flag is per
  // interface, so a MAC on one is not reported.
  for (Interface& i : out)
    if (i.loopback) i.mac = MACAddress();
#endif
  return out;
}

} // namespace netdetail

inline std::vector<IPAddress> IPAddress::getAllAddresses(bool includeIPv6) {
  netdetail::WinsockUser winsock;
  std::vector<IPAddress> v4, v6;
  for (const netdetail::Interface& i : netdetail::listInterfaces())
    for (const IPAddress& a : i.addresses) {
      std::vector<IPAddress>& list = a.isIPv6 ? v6 : v4;
      if (a.isIPv6 && !includeIPv6) continue;
      if (std::find(list.begin(), list.end(), a) == list.end()) list.push_back(a);
    }
  v4.insert(v4.end(), v6.begin(), v6.end());
  return v4;
}

inline IPAddress IPAddress::getLocalAddress(bool includeIPv6) {
  for (const IPAddress& a : getAllAddresses(includeIPv6))
    if (!a.isLoopback()) return a;
  return local();
}

inline IPAddress IPAddress::getInterfaceBroadcastAddress(const IPAddress& interfaceAddress) {
  netdetail::WinsockUser winsock;
  for (const netdetail::Interface& i : netdetail::listInterfaces())
    for (size_t k = 0; k < i.addresses.size(); ++k)
      if (i.addresses[k] == interfaceAddress && k < i.broadcasts.size()) return i.broadcasts[k];
  return IPAddress();
}

inline std::vector<MACAddress> MACAddress::getAllAddresses() {
  netdetail::WinsockUser winsock;
  std::vector<MACAddress> out;
  for (const netdetail::Interface& i : netdetail::listInterfaces())
    if (!i.mac.isNull() && std::find(out.begin(), out.end(), i.mac) == out.end()) out.push_back(i.mac);
  return out;
}

// ═══════════════════════════════════════════════════════════════════════════
// TCP
// ═══════════════════════════════════════════════════════════════════════════

/**
 * A TCP connection, or a listener that accepts them.
 *
 * Blocking, like the reference's: read() and write() wait. Use
 * waitUntilReady() with a timeout to wait for data without committing to a
 * read, which is also how a reader thread stays stoppable. One thread may
 * read while another writes. close() from another thread wakes a thread
 * blocked in read(), which then returns 0 -- it must not read again.
 *
 * Connected sockets have Nagle's algorithm OFF (TCP_NODELAY): every user of
 * this in a plugin is exchanging small messages with a peer that answers,
 * where Nagle plus delayed acknowledgement is a 40-200 ms stall per round
 * trip.
 */
class StreamingSocket {
public:
  /** Closed: connect() or createListener() opens it. */
  StreamingSocket() = default;
  /** close()s it. */
  ~StreamingSocket() { close(); }
  ///@{
  /** Not copyable: the socket has one owner. */
  StreamingSocket(const StreamingSocket&) = delete;
  StreamingSocket& operator=(const StreamingSocket&) = delete;
  ///@}
  /** Takes over `other`'s socket; `other` is left closed. */
  StreamingSocket(StreamingSocket&& other) noexcept { take(other); }
  /** Closes this one's socket, then takes over `other`'s. */
  StreamingSocket& operator=(StreamingSocket&& other) noexcept {
    if (this != &other) {
      close();
      take(other);
    }
    return *this;
  }

  /**
   * Connects to host:port, trying each address the name resolves to until one
   * answers within what is left of timeOutMillisecs. The host may be a name, a
   * dotted quad, or an IPv6 address with or without brackets.
   */
  bool connect(const std::string& remoteHostname, int remotePortNumber, int timeOutMillisecs = 3000) {
    close();
    if (!winsock_.ok() || remotePortNumber <= 0 || remotePortNumber > 65535) return false;
    const auto start = std::chrono::steady_clock::now();
    for (const auto& target : netdetail::resolveHost(remoteHostname, remotePortNumber, true)) {
      int left = timeOutMillisecs;
      if (timeOutMillisecs >= 0) {
        const auto spent =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        left = (int) (timeOutMillisecs - spent);
        if (left <= 0) break;
      }
      if (connectTo(target.first, target.second, left)) {
        hostName_ = remoteHostname;
        port_ = remotePortNumber;
        return true;
      }
    }
    return false;
  }

  /** True from a successful connect(), or for an accepted socket, until
   *  close() or a read() that finds the peer gone. A listener is never
   *  connected. */
  bool isConnected() const noexcept { return connected_; }

  /** Shuts the connection (waking a blocked reader) and releases it. */
  void close() {
    if (handle_ != netdetail::kInvalidSocket) {
      if (connected_) netdetail::shutdownSocket(handle_);
      netdetail::closeSocket(handle_);
    }
    handle_ = netdetail::kInvalidSocket;
    connected_ = listener_ = false;
    hostName_.clear();
    port_ = 0;
  }

  /** The name it was connected to, or the peer's address for an accepted one. */
  const std::string& getHostName() const noexcept { return hostName_; }
  /** The remote port, or for a listener the port it listens on. */
  int getPort() const noexcept { return port_; }
  /** The address of the other end; only meaningful while connected. */
  IPAddress getPeerAddress() const noexcept { return peer_; }
  /** The local port, which is how a listener opened on port 0 says which one
   *  the system gave it. -1 when not open. */
  int getBoundPort() const {
    IPAddress ip;
    int port = -1;
    if (handle_ == netdetail::kInvalidSocket || !netdetail::localAddressOf(handle_, ip, port)) return -1;
    return port;
  }
  /** Whether the peer is this machine. */
  bool isLocal() const noexcept { return connected_ && peer_.isLoopback(); }

  /** 1 when a read (or write) would not block, 0 on timeout, -1 on error. */
  int waitUntilReady(bool readyForReading, int timeoutMsecs) {
    return netdetail::waitForSocket(handle_, readyForReading, timeoutMsecs);
  }

  /**
   * Reads up to maxBytesToRead. Without blockUntilSpecifiedAmountHasArrived
   * it returns as soon as anything arrives; with it, it keeps reading until
   * the buffer is full or the connection ends.
   *
   * Returns the count read; 0 when the peer closed the connection (and
   * isConnected() is now false); -1 on an error with nothing read.
   */
  int read(void* destBuffer, int maxBytesToRead, bool blockUntilSpecifiedAmountHasArrived) {
    if (!connected_ || !destBuffer || maxBytesToRead <= 0) return connected_ ? 0 : -1;
    char* out = static_cast<char*>(destBuffer);
    int done = 0;
    while (done < maxBytesToRead) {
      const int got = netdetail::recvBytes(handle_, out + done, maxBytesToRead - done);
      if (got == 0) {
        connected_ = false;
        break;
      }
      if (got < 0) {
        if (done == 0) return -1;
        break;
      }
      done += got;
      if (!blockUntilSpecifiedAmountHasArrived) break;
    }
    return done;
  }

  /** Writes all of it: numBytesToWrite, or -1. */
  int write(const void* sourceBuffer, int numBytesToWrite) {
    if (!connected_ || numBytesToWrite < 0 || (!sourceBuffer && numBytesToWrite > 0)) return -1;
    const char* in = static_cast<const char*>(sourceBuffer);
    int done = 0;
    while (done < numBytesToWrite) {
      const int sent = netdetail::sendBytes(handle_, in + done, numBytesToWrite - done);
      if (sent <= 0) return -1;
      done += sent;
    }
    return done;
  }

  /**
   * Listens on portNumber (0: let the system choose; getBoundPort() says which)
   * at localHostName -- the LOOPBACK when empty, see the header. "0.0.0.0"
   * listens on every IPv4 interface, "::" on every interface of both families.
   */
  bool createListener(int portNumber, const std::string& localHostName = std::string()) {
    close();
    IPAddress ip;
    if (!winsock_.ok() || portNumber < 0 || portNumber > 65535 || !netdetail::listenAddress(localHostName, ip))
      return false;
    handle_ = netdetail::openSocket(ip.isIPv6 ? netdetail::kAfInet6 : netdetail::kAfInet, netdetail::kSockStream,
                                    netdetail::kIpprotoTcp);
    if (handle_ == netdetail::kInvalidSocket) return false;
#if defined(_WIN32)
    // Not SO_REUSEADDR: on Windows that lets ANOTHER program bind the same
    // port and steal the connections. Exclusive use is the safe default.
    netdetail::setIntOption(handle_, netdetail::kSolSocket, netdetail::kSoExclusiveAddrUse, 1);
#else
    // A listener restarted within TIME_WAIT would otherwise fail to bind.
    netdetail::setIntOption(handle_, netdetail::kSolSocket, netdetail::kSoReuseAddr, 1);
#endif
    if (ip.isIPv6) netdetail::setIntOption(handle_, netdetail::kIpprotoIpv6, netdetail::kIpv6V6Only, 0);
    const netdetail::SockAddr addr = netdetail::makeSockAddr(ip, portNumber);
#if defined(_WIN32)
    const bool bound = netdetail::winsock().bind(handle_, (const netdetail::WsSockAddr*) addr.bytes, addr.length) == 0 &&
                       netdetail::winsock().listen(handle_, 0x7fffffff) == 0;
#else
    const bool bound = ::bind(handle_, (const sockaddr*) addr.bytes, (socklen_t) addr.length) == 0 &&
                       ::listen(handle_, SOMAXCONN) == 0;
#endif
    if (!bound) {
      close();
      return false;
    }
    listener_ = true;
    port_ = getBoundPort();
    hostName_ = ip.toString();
    return true;
  }

  /** The next incoming connection, or null on timeout (negative: wait
   *  forever) or when this is not a listener. */
  std::unique_ptr<StreamingSocket> waitForNextConnection(int timeoutMs = -1) const {
    if (!listener_ || handle_ == netdetail::kInvalidSocket) return nullptr;
    if (netdetail::waitForSocket(handle_, true, timeoutMs) != 1) return nullptr;
    netdetail::SockAddr peer;
#if defined(_WIN32)
    int len = (int) sizeof(peer.bytes);
    const netdetail::SocketHandle s = netdetail::winsock().accept(handle_, (netdetail::WsSockAddr*) peer.bytes, &len);
    netdetail::makeNotInheritable(s);
#else
    socklen_t len = (socklen_t) sizeof(peer.bytes);
#if defined(__linux__)
    const netdetail::SocketHandle s = ::accept4(handle_, (sockaddr*) peer.bytes, &len, SOCK_CLOEXEC);
#else
    const netdetail::SocketHandle s = ::accept(handle_, (sockaddr*) peer.bytes, &len);
    if (s >= 0) ::fcntl(s, F_SETFD, FD_CLOEXEC);
#endif
#if defined(SO_NOSIGPIPE)
    if (s >= 0) netdetail::setIntOption(s, SOL_SOCKET, SO_NOSIGPIPE, 1);
#endif
#endif
    if (s == netdetail::kInvalidSocket) return nullptr;
    auto out = std::make_unique<StreamingSocket>();
    out->handle_ = s;
    out->connected_ = true;
    int peerPort = 0;
    netdetail::readSockAddr(peer.bytes, out->peer_, peerPort);
    out->port_ = peerPort;
    out->hostName_ = out->peer_.toString();
    // Accepted sockets inherit the listener's non-blocking flag on some
    // systems; this one is a blocking socket like any other.
    netdetail::setBlocking(s, true);
    netdetail::setIntOption(s, netdetail::kIpprotoTcp, netdetail::kTcpNoDelay, 1);
    return out;
  }

  /** True after a successful createListener(), until close(). */
  bool isListener() const noexcept { return listener_; }
  /** The platform's socket, for an option this class does not cover. -1 when closed. */
  intptr_t getRawSocketHandle() const noexcept {
    return handle_ == netdetail::kInvalidSocket ? (intptr_t) -1 : (intptr_t) handle_;
  }

private:
  bool connectTo(const IPAddress& ip, int port, int timeoutMs) {
    netdetail::SocketHandle s =
        netdetail::openSocket(ip.isIPv6 ? netdetail::kAfInet6 : netdetail::kAfInet, netdetail::kSockStream,
                              netdetail::kIpprotoTcp);
    if (s == netdetail::kInvalidSocket) return false;
    const netdetail::SockAddr addr = netdetail::makeSockAddr(ip, port);
    // Non-blocking for the connect alone, so it can be given up on.
    netdetail::setBlocking(s, false);
#if defined(_WIN32)
    int r = netdetail::winsock().connect(s, (const netdetail::WsSockAddr*) addr.bytes, addr.length);
#else
    int r = ::connect(s, (const sockaddr*) addr.bytes, (socklen_t) addr.length);
    while (r < 0 && errno == EINTR) r = ::connect(s, (const sockaddr*) addr.bytes, (socklen_t) addr.length);
#endif
    bool ok = r == 0;
    if (!ok && netdetail::isWouldBlock(netdetail::lastSocketError()) &&
        netdetail::waitForSocket(s, false, timeoutMs) == 1) {
      int error = 0;
#if defined(_WIN32)
      int len = (int) sizeof(error);
      netdetail::winsock().getsockopt(s, netdetail::kSolSocket, netdetail::kSoError, (char*) &error, &len);
#else
      socklen_t len = sizeof(error);
      ::getsockopt(s, SOL_SOCKET, SO_ERROR, &error, &len);
#endif
      ok = error == 0;
    }
    if (!ok) {
      netdetail::closeSocket(s);
      return false;
    }
    netdetail::setBlocking(s, true);
    netdetail::setIntOption(s, netdetail::kIpprotoTcp, netdetail::kTcpNoDelay, 1);
    handle_ = s;
    connected_ = true;
    peer_ = ip;
    return true;
  }

  void take(StreamingSocket& other) noexcept {
    winsock_ = std::move(other.winsock_);
    handle_ = other.handle_;
    other.handle_ = netdetail::kInvalidSocket;
    hostName_ = std::move(other.hostName_);
    port_ = other.port_;
    peer_ = other.peer_;
    connected_ = other.connected_;
    listener_ = other.listener_;
    other.connected_ = other.listener_ = false;
    other.port_ = 0;
  }

  netdetail::WinsockUser winsock_;
  netdetail::SocketHandle handle_ = netdetail::kInvalidSocket;
  std::string hostName_;
  int port_ = 0;
  IPAddress peer_;
  bool connected_ = false;
  bool listener_ = false;
};

// ═══════════════════════════════════════════════════════════════════════════
// UDP
// ═══════════════════════════════════════════════════════════════════════════

/**
 * A UDP socket: datagrams to and from anywhere, broadcast, multicast.
 *
 * The socket is created when it is first needed -- by bindToPort, write or
 * joinMulticast -- in the family that call implies, so the same object
 * serves IPv4 and IPv6. bindToPort(port) binds the LOOPBACK (see the
 * header); "0.0.0.0" or "::" hears the network, and is what broadcast and
 * multicast reception need.
 *
 * A datagram larger than the buffer handed to read() is cut to fit and the
 * rest is lost, which is what UDP does; 65507 bytes always fits one.
 */
class DatagramSocket {
public:
  /** Nothing is opened yet. `enableBroadcasting` sets SO_BROADCAST on the
   *  socket when it is created, which sending to a broadcast address
   *  needs. */
  explicit DatagramSocket(bool enableBroadcasting = false) : broadcast_(enableBroadcasting) {}
  /** shutdown()s it. */
  ~DatagramSocket() { shutdown(); }
  ///@{
  /** Not copyable: the socket has one owner. */
  DatagramSocket(const DatagramSocket&) = delete;
  DatagramSocket& operator=(const DatagramSocket&) = delete;
  ///@}
  /** Takes over `other`'s socket and settings; `other` is left closed. */
  DatagramSocket(DatagramSocket&& other) noexcept { take(other); }
  /** Closes this one's socket, then takes over `other`'s. */
  DatagramSocket& operator=(DatagramSocket&& other) noexcept {
    if (this != &other) {
      shutdown();
      take(other);
    }
    return *this;
  }

  /** Binds the loopback, port 0 meaning "any free one" (getBoundPort says). */
  bool bindToPort(int localPortNumber) { return bindToPort(localPortNumber, std::string()); }
  /** Binds `localAddress` -- "" or "localhost" for the loopback, "0.0.0.0"
   *  or "::" for every interface, or an address or name -- at
   *  `localPortNumber` (0: any free port). A socket already open is closed
   *  first. False when the address does not resolve or the port cannot be
   *  had. */
  bool bindToPort(int localPortNumber, const std::string& localAddress) {
    IPAddress ip;
    if (localPortNumber < 0 || localPortNumber > 65535 || !netdetail::listenAddress(localAddress, ip)) return false;
    if (handle_ != netdetail::kInvalidSocket) shutdown();
    if (!ensure(ip.isIPv6)) return false;
    const netdetail::SockAddr addr = netdetail::makeSockAddr(ip, localPortNumber);
#if defined(_WIN32)
    const bool ok = netdetail::winsock().bind(handle_, (const netdetail::WsSockAddr*) addr.bytes, addr.length) == 0;
#else
    const bool ok = ::bind(handle_, (const sockaddr*) addr.bytes, (socklen_t) addr.length) == 0;
#endif
    if (!ok) {
      shutdown();
      return false;
    }
    bound_ = true;
    return true;
  }

  /** The local port after bindToPort(), the system's choice for port 0;
   *  -1 when not bound. */
  int getBoundPort() const {
    IPAddress ip;
    int port = -1;
    if (!bound_ || !netdetail::localAddressOf(handle_, ip, port)) return -1;
    return port;
  }

  /** 1 when a datagram is waiting (or a write would not block), 0 on timeout, -1 on error. */
  int waitUntilReady(bool readyForReading, int timeoutMsecs) {
    return netdetail::waitForSocket(handle_, readyForReading, timeoutMsecs);
  }

  /** One datagram. Blocks for one unless `block` is false, when it returns 0
   *  at once if none is waiting. -1 on error. */
  int read(void* destBuffer, int maxBytesToRead, bool blockUntilSpecifiedAmountHasArrived) {
    IPAddress ip;
    int port = 0;
    return read(destBuffer, maxBytesToRead, blockUntilSpecifiedAmountHasArrived, ip, port);
  }
  /** The same, also saying who sent it: the sender's address as text (an
   *  IPv4 sender on an IPv6 socket written as plain IPv4) and its port. */
  int read(void* destBuffer, int maxBytesToRead, bool blockUntilSpecifiedAmountHasArrived,
           std::string& senderIPAddress, int& senderPortNumber) {
    IPAddress ip;
    const int got = read(destBuffer, maxBytesToRead, blockUntilSpecifiedAmountHasArrived, ip, senderPortNumber);
    if (got >= 0) senderIPAddress = IPAddress::convertIPv4MappedAddressToIPv4(ip).toString();
    return got;
  }
  /** The same, with the sender as the socket reported it: an IPv4 sender
   *  on an IPv6 socket is IPv4-mapped. */
  int read(void* destBuffer, int maxBytesToRead, bool blockUntilSpecifiedAmountHasArrived, IPAddress& sender,
           int& senderPortNumber) {
    if (handle_ == netdetail::kInvalidSocket || !destBuffer || maxBytesToRead < 0) return -1;
    for (;;) {
      if (!blockUntilSpecifiedAmountHasArrived && netdetail::waitForSocket(handle_, true, 0) != 1) return 0;
      netdetail::SockAddr from;
#if defined(_WIN32)
      int len = (int) sizeof(from.bytes);
      int got = netdetail::winsock().recvfrom(handle_, (char*) destBuffer, maxBytesToRead, 0,
                                              (netdetail::WsSockAddr*) from.bytes, &len);
      if (got < 0) {
        const int e = netdetail::lastSocketError();
        // Truncated: the buffer is full and that is the datagram we can give.
        if (e == netdetail::kWsaEMsgSize) got = maxBytesToRead;
        // An ICMP "port unreachable" from an earlier send, reported on this
        // read. Not a datagram and not this socket's failure.
        else if (e == netdetail::kWsaEConnReset) continue;
        else return -1;
      }
#else
      socklen_t len = (socklen_t) sizeof(from.bytes);
      const int flags = blockUntilSpecifiedAmountHasArrived ? 0 : MSG_DONTWAIT;
      ssize_t got = ::recvfrom(handle_, destBuffer, (size_t) maxBytesToRead, flags, (sockaddr*) from.bytes, &len);
      if (got < 0) {
        if (errno == EINTR) continue;
        if (!blockUntilSpecifiedAmountHasArrived && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        if (errno == ECONNREFUSED) continue;
        return -1;
      }
#endif
      netdetail::readSockAddr(from.bytes, sender, senderPortNumber);
      return (int) got;
    }
  }

  /** Sends one datagram to host:port; the bytes sent, or -1. */
  int write(const std::string& remoteHostname, int remotePortNumber, const void* sourceBuffer, int numBytesToWrite) {
    IPAddress ip;
    if (!IPAddress::parse(remoteHostname, ip)) {
      // Resolved once and remembered: a sender that names its peer sends
      // to it many times, and a lookup per datagram is a resolver per packet.
      if (remoteHostname != lastHost_) {
        const auto found = netdetail::resolveHost(remoteHostname, remotePortNumber, false);
        if (found.empty()) return -1;
        // The socket's own family when it has one; otherwise the resolver's
        // first preference.
        IPAddress chosen = found.front().first;
        if (handle_ != netdetail::kInvalidSocket)
          for (const auto& f : found)
            if (f.first.isIPv6 == v6_) {
              chosen = f.first;
              break;
            }
        lastHost_ = remoteHostname;
        lastAddress_ = chosen;
      }
      ip = lastAddress_;
    }
    return write(ip, remotePortNumber, sourceBuffer, numBytesToWrite);
  }
  /** Sends one datagram to `remote`, opening the socket in that address's
   *  family if none is open. An IPv4 target from an IPv6 socket is sent as
   *  IPv4-mapped; an IPv6 target from an IPv4 socket fails unless it is
   *  IPv4-mapped. The bytes sent, or -1. */
  int write(const IPAddress& remote, int remotePortNumber, const void* sourceBuffer, int numBytesToWrite) {
    if (remotePortNumber <= 0 || remotePortNumber > 65535 || numBytesToWrite < 0) return -1;
    IPAddress target = remote;
    if (handle_ == netdetail::kInvalidSocket && !ensure(target.isIPv6)) return -1;
    // An IPv4 target from an IPv6 socket goes as a mapped address.
    if (v6_ && !target.isIPv6) target = IPAddress::convertIPv4AddressToIPv4Mapped(target);
    if (!v6_ && target.isIPv6) {
      if (!target.isIPv4MappedAddress()) return -1;
      target = IPAddress::convertIPv4MappedAddressToIPv4(target);
    }
    const netdetail::SockAddr addr = netdetail::makeSockAddr(target, remotePortNumber);
#if defined(_WIN32)
    const int sent = netdetail::winsock().sendto(handle_, (const char*) sourceBuffer, numBytesToWrite, 0,
                                                 (const netdetail::WsSockAddr*) addr.bytes, addr.length);
#else
    ssize_t sent;
    do {
      sent = ::sendto(handle_, sourceBuffer, (size_t) numBytesToWrite, 0, (const sockaddr*) addr.bytes,
                      (socklen_t) addr.length);
    } while (sent < 0 && errno == EINTR);
#endif
    return sent < 0 ? -1 : (int) sent;
  }

  /** Closes the socket. Another thread blocked in read() returns. */
  void shutdown() {
    if (handle_ != netdetail::kInvalidSocket) {
      netdetail::shutdownSocket(handle_);
      netdetail::closeSocket(handle_);
    }
    handle_ = netdetail::kInvalidSocket;
    bound_ = false;
  }

  /** Joins a multicast group on the default interface. */
  bool joinMulticast(const std::string& multicastIPAddress) { return membership(multicastIPAddress, true); }
  /** Leaves a group joined with joinMulticast(). False for text that is not
   *  a multicast address. */
  bool leaveMulticast(const std::string& multicastIPAddress) { return membership(multicastIPAddress, false); }

  /** Whether this machine hears its own multicasts (the default is the
   *  system's: on, everywhere this SDK runs). */
  bool setMulticastLoopbackEnabled(bool enableLoopback) {
    if (handle_ == netdetail::kInvalidSocket && !ensure(false)) return false;
    return v6_ ? netdetail::setIntOption(handle_, netdetail::kIpprotoIpv6, netdetail::kIpv6MulticastLoop,
                                         enableLoopback ? 1 : 0)
               : netdetail::setIntOption(handle_, netdetail::kIpprotoIp, netdetail::kIpMulticastLoop,
                                         enableLoopback ? 1 : 0);
  }

  /** Lets several sockets bind one port -- several listeners for one
   *  broadcast. Takes effect at the next bindToPort. */
  bool setEnablePortReuse(bool enabled) {
    reuse_ = enabled;
    return true;
  }

  /** The platform's socket, for an option this class does not cover; -1
   *  when there is none yet. Still owned by this object. */
  intptr_t getRawSocketHandle() const noexcept {
    return handle_ == netdetail::kInvalidSocket ? (intptr_t) -1 : (intptr_t) handle_;
  }

private:
  bool ensure(bool ipv6) {
    if (handle_ != netdetail::kInvalidSocket) return v6_ == ipv6 || v6_;
    if (!winsock_.ok()) return false;
    handle_ = netdetail::openSocket(ipv6 ? netdetail::kAfInet6 : netdetail::kAfInet, netdetail::kSockDgram,
                                    netdetail::kIpprotoUdp);
    if (handle_ == netdetail::kInvalidSocket) return false;
    v6_ = ipv6;
    if (ipv6) netdetail::setIntOption(handle_, netdetail::kIpprotoIpv6, netdetail::kIpv6V6Only, 0);
    if (broadcast_) netdetail::setIntOption(handle_, netdetail::kSolSocket, netdetail::kSoBroadcast, 1);
    if (reuse_) {
      netdetail::setIntOption(handle_, netdetail::kSolSocket, netdetail::kSoReuseAddr, 1);
#if defined(SO_REUSEPORT) && !defined(_WIN32)
      // BSD (and macOS) wants this as well before two sockets may share a
      // port; Linux gives SO_REUSEADDR the same meaning for UDP already.
      netdetail::setIntOption(handle_, SOL_SOCKET, SO_REUSEPORT, 1);
#endif
    }
#if defined(_WIN32)
    // Without this, an ICMP "port unreachable" for one datagram fails the
    // NEXT receive on Windows (WSAECONNRESET). read() also steps over it.
    if (netdetail::winsock().ioctl) {
      unsigned long off = 0, returned = 0;
      netdetail::winsock().ioctl(handle_, netdetail::kSioUdpConnReset, &off, sizeof(off), nullptr, 0, &returned,
                                 nullptr, nullptr);
    }
#endif
    return true;
  }

  bool membership(const std::string& group, bool join) {
    IPAddress ip;
    if (!IPAddress::parse(group, ip) || !ip.isMulticast()) return false;
    if (handle_ == netdetail::kInvalidSocket && !ensure(ip.isIPv6)) return false;
    if (ip.isIPv6) {
#if defined(_WIN32)
      netdetail::WsIpv6Mreq m{};
      std::memcpy(m.multiaddr, ip.address, 16);
      return netdetail::winsock().setsockopt(handle_, netdetail::kIpprotoIpv6,
                                             join ? netdetail::kIpv6JoinGroup : netdetail::kIpv6LeaveGroup,
                                             (const char*) &m, (int) sizeof(m)) == 0;
#else
      ipv6_mreq m;
      std::memset(&m, 0, sizeof(m));
      std::memcpy(&m.ipv6mr_multiaddr, ip.address, 16);
      return ::setsockopt(handle_, IPPROTO_IPV6, join ? IPV6_JOIN_GROUP : IPV6_LEAVE_GROUP, &m, sizeof(m)) == 0;
#endif
    }
    if (v6_) return false; // an IPv4 group on an IPv6 socket: bind IPv4 for it
#if defined(_WIN32)
    netdetail::WsIpMreq m{};
    std::memcpy(m.multiaddr, ip.address, 4);
    return netdetail::winsock().setsockopt(handle_, netdetail::kIpprotoIp,
                                           join ? netdetail::kIpAddMembership : netdetail::kIpDropMembership,
                                           (const char*) &m, (int) sizeof(m)) == 0;
#else
    ip_mreq m;
    std::memset(&m, 0, sizeof(m));
    std::memcpy(&m.imr_multiaddr, ip.address, 4);
    return ::setsockopt(handle_, IPPROTO_IP, join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP, &m, sizeof(m)) == 0;
#endif
  }

  void take(DatagramSocket& other) noexcept {
    winsock_ = std::move(other.winsock_);
    handle_ = other.handle_;
    other.handle_ = netdetail::kInvalidSocket;
    v6_ = other.v6_;
    bound_ = other.bound_;
    other.bound_ = false;
    broadcast_ = other.broadcast_;
    reuse_ = other.reuse_;
    lastHost_ = std::move(other.lastHost_);
    lastAddress_ = other.lastAddress_;
  }

  netdetail::WinsockUser winsock_;
  netdetail::SocketHandle handle_ = netdetail::kInvalidSocket;
  bool v6_ = false;
  bool bound_ = false;
  bool broadcast_ = false;
  bool reuse_ = false;
  std::string lastHost_;
  IPAddress lastAddress_;
};

// ═══════════════════════════════════════════════════════════════════════════
// URL
// ═══════════════════════════════════════════════════════════════════════════

class WebInputStream;

/**
 * A URL, held as its RFC 3986 components.
 *
 * Parsing splits, it does not judge: any string becomes a URL whose
 * components recompose to it (scheme and IPv6 host case aside), and
 * isWellFormed() says whether it is a valid absolute URI. Components are
 * kept as written -- still percent-encoded -- and the accessors that return
 * DECODED text say so (getFileName, getParameterValues).
 *
 * resolve() is RFC 3986 section 5.2 exactly, dot segments included, which is
 * what a redirect's Location and a link in a downloaded page need.
 */
class URL {
public:
  /** How createInputStream and WebInputStream make a request. Set fields
   *  directly, or chain the with...() calls as the reference's options do. */
  struct InputStreamOptions {
    /** GET when empty -- POST when hasPostData is set, as withPostData()
     *  sets it. */
    std::string httpRequestCmd;
    /** Request headers, name and value. One HTTP cannot carry (a CR or LF in
     *  it, a name that is not a token) fails the request. */
    std::vector<std::pair<std::string, std::string>> extraHeaders;
    /** The request body. */
    std::vector<uint8_t> postData;
    /** Makes an empty httpRequestCmd mean POST. withPostData() sets it;
     *  filling postData directly does not. */
    bool hasPostData = false;
    /** Connecting, and then the longest silence while reading, in ms. */
    int connectionTimeoutMs = 30000;
    /** How many redirects to follow; 0 hands a 3xx back as the response. */
    int numRedirectsToFollow = 5;
    /** Called on the reading thread after each read with the bytes so far and
     *  the total (-1 if unknown). Returning false cancels. */
    std::function<bool(int64_t bytesDownloaded, int64_t totalLength)> progressCallback;

    /** A copy with this method -- "PUT", "HEAD", "DELETE". */
    InputStreamOptions withHttpRequestCmd(const std::string& command) const {
      InputStreamOptions o = *this;
      o.httpRequestCmd = command;
      return o;
    }
    /** A copy with one more request header. */
    InputStreamOptions withExtraHeader(const std::string& name, const std::string& value) const {
      InputStreamOptions o = *this;
      o.extraHeaders.emplace_back(name, value);
      return o;
    }
    /** Header lines "Name: value", separated by CR/LF, as the reference takes them. */
    InputStreamOptions withExtraHeaders(const std::string& headerLines) const {
      InputStreamOptions o = *this;
      size_t start = 0;
      while (start <= headerLines.size()) {
        size_t end = headerLines.find_first_of("\r\n", start);
        if (end == std::string::npos) end = headerLines.size();
        const std::string line = headerLines.substr(start, end - start);
        const size_t colon = line.find(':');
        if (colon != std::string::npos && colon > 0) {
          size_t v = colon + 1;
          while (v < line.size() && (line[v] == ' ' || line[v] == '\t')) ++v;
          o.extraHeaders.emplace_back(line.substr(0, colon), line.substr(v));
        }
        start = end + 1;
      }
      return o;
    }
    /** A copy that sends `body`, as a POST unless httpRequestCmd names
     *  another method. An empty body is still a POST. */
    InputStreamOptions withPostData(const std::vector<uint8_t>& body) const {
      InputStreamOptions o = *this;
      o.postData = body;
      o.hasPostData = true;
      return o;
    }
    /** The same with the string's bytes, sent as they are. */
    InputStreamOptions withPostData(const std::string& body) const {
      return withPostData(std::vector<uint8_t>(body.begin(), body.end()));
    }
    /** A copy with connectionTimeoutMs set. */
    InputStreamOptions withConnectionTimeoutMs(int ms) const {
      InputStreamOptions o = *this;
      o.connectionTimeoutMs = ms;
      return o;
    }
    /** A copy with numRedirectsToFollow set. */
    InputStreamOptions withNumRedirectsToFollow(int count) const {
      InputStreamOptions o = *this;
      o.numRedirectsToFollow = count;
      return o;
    }
    /** A copy with progressCallback set. */
    InputStreamOptions withProgressCallback(std::function<bool(int64_t, int64_t)> callback) const {
      InputStreamOptions o = *this;
      o.progressCallback = std::move(callback);
      return o;
    }
  };

  /** What downloadToFile did. */
  struct DownloadResult {
    bool succeeded = false;  ///< the whole body arrived with a 2xx status and replaced the target
    int statusCode = 0;      ///< the response's status; 0 when no response arrived
    int64_t bytesWritten = 0; ///< body bytes written to the temporary file, even when it then failed
    std::string error;       ///< what went wrong, for a person; empty on success
  };

  /** The empty URL. */
  URL() = default;
  /** Parses `text`. Every string is accepted (see the class comment);
   *  isWellFormed() says whether it is a valid absolute URI. */
  explicit URL(const std::string& text) { parse(text); }

  // ── Components, as written (percent-encoded) ──

  /** True for the URL of an empty string. */
  bool isEmpty() const noexcept { return text_.empty(); }
  /** Lowercase, without the ':'. Empty for a relative reference. */
  const std::string& getScheme() const noexcept { return scheme_; }
  /** True when there is a "//" authority, even an empty one
   *  ("file:///tmp/x"). */
  bool hasAuthority() const noexcept { return hasAuthority_; }
  /** What comes before '@' in the authority ("user:pass"), still encoded;
   *  empty when there is none. */
  const std::string& getUserInfo() const noexcept { return userInfo_; }
  /** The host, without the brackets an IPv6 literal is written in. The
   *  reference calls this getDomain(). */
  const std::string& getHost() const noexcept { return host_; }
  /** getHost(), by the reference's name. */
  const std::string& getDomain() const noexcept { return host_; }
  /** The explicit port, or 0 when none is written. */
  int getPort() const noexcept { return portNumber(); }
  /** The explicit port, else the scheme's (80, 443, 21), else 0. */
  int getPortOrDefault() const noexcept {
    const int p = portNumber();
    if (p > 0) return p;
    if (scheme_ == "http" || scheme_ == "ws") return 80;
    if (scheme_ == "https" || scheme_ == "wss") return 443;
    if (scheme_ == "ftp") return 21;
    return 0;
  }
  /** The path with its leading '/'. */
  const std::string& getPath() const noexcept { return path_; }
  /** The path without its leading '/', and the query if asked -- the
   *  reference's getSubPath. */
  std::string getSubPath(bool includeGetParameters = false) const {
    std::string p = (!path_.empty() && path_[0] == '/') ? path_.substr(1) : path_;
    if (includeGetParameters && hasQuery_) p += "?" + query_;
    return p;
  }
  /** True when there is a '?', even with nothing after it. */
  bool hasQuery() const noexcept { return hasQuery_; }
  /** Without the '?'. */
  const std::string& getQuery() const noexcept { return query_; }
  /** True when there is a '#', even with nothing after it. */
  bool hasFragment() const noexcept { return hasFragment_; }
  /** Without the '#'. */
  const std::string& getFragment() const noexcept { return fragment_; }

  /** The last path segment, decoded: "a%20b.wav" for ".../a%20b.wav" gives
   *  "a b.wav". */
  std::string getFileName() const {
    const size_t slash = path_.rfind('/');
    return removeEscapeChars(slash == std::string::npos ? path_ : path_.substr(slash + 1));
  }

  /** "scheme://host:port" as written -- the port only when the URL has one,
   *  an IPv6 host in its brackets. What a request is sent to. */
  std::string getOrigin() const {
    std::string out = scheme_ + "://";
    out += ipLiteral_ ? "[" + host_ + "]" : host_;
    if (hasPort_ && !port_.empty()) out += ":" + port_;
    return out;
  }

  /** The whole URL. Without the query when includeGetParameters is false. */
  std::string toString(bool includeGetParameters = true) const {
    std::string out;
    if (!scheme_.empty()) out += scheme_ + ":";
    if (hasAuthority_) {
      out += "//";
      if (hasUserInfo_) out += userInfo_ + "@";
      out += ipLiteral_ ? "[" + host_ + "]" : host_;
      if (hasPort_) out += ":" + port_;
    } else if (path_.size() >= 2 && path_[0] == '/' && path_[1] == '/') {
      // A path that begins "//" with no authority would read back AS an
      // authority. RFC 3986 5.2 can produce one (a base with no authority,
      // a reference of "..//x"); "/." keeps it a path.
      out += "/.";
    }
    out += path_;
    if (includeGetParameters && hasQuery_) out += "?" + query_;
    if (hasFragment_) out += "#" + fragment_;
    return out;
  }

  /** A valid absolute URI: a scheme, only characters RFC 3986 allows (and
   *  percent signs only before two hex digits), a port that is a port, an
   *  IPv6 host that is an address -- and for http and https, a host. */
  bool isWellFormed() const {
    if (!validScheme(scheme_)) return false;
    if ((scheme_ == "http" || scheme_ == "https") && (!hasAuthority_ || host_.empty())) return false;
    if (hasAuthority_) {
      if (!allowedChars(userInfo_, ":")) return false;
      if (ipLiteral_) {
        IPAddress ip;
        if (!IPAddress::parse(host_, ip) || !ip.isIPv6) return false;
      } else if (!allowedChars(host_, "")) {
        return false;
      }
      if (hasPort_) {
        if (port_.size() > 5) return false;
        for (char c : port_)
          if (c < '0' || c > '9') return false;
        if (portNumber() > 65535) return false;
      }
    }
    return allowedChars(path_, ":@/") && allowedChars(query_, ":@/?") && allowedChars(fragment_, ":@/?");
  }

  // ── Building new URLs ──

  /**
   * This URL with a path segment (or several, "a/b") added: exactly one '/'
   * between, and characters a path cannot hold escaped ("my file.wav" becomes
   * "my%20file.wav"). Escapes already present ("%20") are kept, so a part that
   * was encoded once is not encoded twice.
   */
  URL getChildURL(const std::string& subPath) const {
    URL u = *this;
    std::string part = encodeLoosely(subPath, "/:@");
    while (!part.empty() && part[0] == '/') part.erase(0, 1);
    std::string p = u.path_;
    if (p.empty() || p.back() != '/') p += '/';
    u.path_ = p + part;
    u.rebuild();
    return u;
  }
  /** This URL with its path replaced (escaped as getChildURL escapes). */
  URL withNewSubPath(const std::string& newPath) const {
    URL u = *this;
    std::string p = encodeLoosely(newPath, "/:@");
    if (!p.empty() && p[0] != '/' && u.hasAuthority_) p = "/" + p;
    u.path_ = p;
    u.rebuild();
    return u;
  }
  /** One level up: ".../a/b" gives ".../a", the query and fragment dropped. */
  URL getParentURL() const {
    URL u = *this;
    std::string p = u.path_;
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    const size_t slash = p.rfind('/');
    u.path_ = slash == std::string::npos ? std::string() : p.substr(0, slash == 0 ? 1 : slash);
    u.hasQuery_ = u.hasFragment_ = false;
    u.query_.clear();
    u.fragment_.clear();
    u.rebuild();
    return u;
  }
  /** This URL with name=value appended to the query, both encoded. */
  URL withParameter(const std::string& name, const std::string& value) const {
    URL u = *this;
    if (u.hasQuery_ && !u.query_.empty()) u.query_ += "&";
    u.query_ += addEscapeChars(name, true) + "=" + addEscapeChars(value, true);
    u.hasQuery_ = true;
    u.rebuild();
    return u;
  }
  /** withParameter() for each pair, in order. */
  URL withParameters(const std::vector<std::pair<std::string, std::string>>& parameters) const {
    URL u = *this;
    for (const auto& p : parameters) u = u.withParameter(p.first, p.second);
    return u;
  }
  /** This URL with no query at all, its '?' included; the fragment stays. */
  URL withoutParameters() const {
    URL u = *this;
    u.query_.clear();
    u.hasQuery_ = false;
    u.rebuild();
    return u;
  }
  /** This URL with its fragment replaced; characters a fragment cannot hold
   *  are escaped, escapes already there are kept. */
  URL withFragment(const std::string& fragment) const {
    URL u = *this;
    u.fragment_ = encodeLoosely(fragment, ":@/?");
    u.hasFragment_ = true;
    u.rebuild();
    return u;
  }

  /** The query's parameters, decoded ('+' is a space, as forms send it). */
  std::vector<std::pair<std::string, std::string>> getParameters() const {
    std::vector<std::pair<std::string, std::string>> out;
    if (!hasQuery_ || query_.empty()) return out;
    size_t start = 0;
    while (start <= query_.size()) {
      size_t end = query_.find('&', start);
      if (end == std::string::npos) end = query_.size();
      const std::string pair = query_.substr(start, end - start);
      if (!pair.empty()) {
        const size_t eq = pair.find('=');
        out.emplace_back(decodeForm(pair.substr(0, eq)),
                         eq == std::string::npos ? std::string() : decodeForm(pair.substr(eq + 1)));
      }
      start = end + 1;
    }
    return out;
  }
  /** The names from getParameters(), decoded and in order; a name given
   *  twice is listed twice. */
  std::vector<std::string> getParameterNames() const {
    std::vector<std::string> out;
    for (const auto& p : getParameters()) out.push_back(p.first);
    return out;
  }
  /** The values from getParameters(), decoded, in the same order as
   *  getParameterNames(). */
  std::vector<std::string> getParameterValues() const {
    std::vector<std::string> out;
    for (const auto& p : getParameters()) out.push_back(p.second);
    return out;
  }
  /** The first value of `name`, or empty. */
  std::string getParameterValue(const std::string& name) const {
    for (const auto& p : getParameters())
      if (p.first == name) return p.second;
    return std::string();
  }

  /** `reference` resolved against this URL (RFC 3986 5.2.2, strict). */
  URL resolve(const std::string& reference) const { return resolve(URL(reference)); }
  /** The same, for a reference already parsed. */
  URL resolve(const URL& r) const {
    URL t;
    if (!r.scheme_.empty()) {
      t = r;
      t.path_ = removeDotSegments(r.path_);
    } else {
      if (r.hasAuthority_) {
        t.copyAuthority(r);
        t.path_ = removeDotSegments(r.path_);
        t.setQuery(r);
      } else {
        if (r.path_.empty()) {
          t.path_ = path_;
          if (r.hasQuery_) t.setQuery(r);
          else t.setQuery(*this);
        } else {
          if (r.path_[0] == '/') {
            t.path_ = removeDotSegments(r.path_);
          } else {
            t.path_ = removeDotSegments(merge(r.path_));
          }
          t.setQuery(r);
        }
        t.copyAuthority(*this);
      }
      t.scheme_ = scheme_;
    }
    t.hasFragment_ = r.hasFragment_;
    t.fragment_ = r.fragment_;
    t.rebuild();
    return t;
  }

  // ── Percent-encoding ──

  /**
   * Escapes everything but letters, digits and "-._~" as %XX (uppercase hex,
   * of each UTF-8 byte) -- the reference's rules: as a PARAMETER (isParameter)
   * nothing else is kept; as a path segment, also "!$&'*+,;=:@". Round
   * brackets are kept unless roundBracketsAreLegal is false (some servers
   * choke on them). A '/' is always escaped: this encodes ONE segment or one
   * value.
   */
  static std::string addEscapeChars(const std::string& text, bool isParameter, bool roundBracketsAreLegal = true) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
      const unsigned char c = (unsigned char) ch;
      bool keep = isUnreserved(c);
      if (!keep && !isParameter) keep = std::strchr("!$&'*+,;=:@", c) != nullptr && c != 0;
      if (!keep && roundBracketsAreLegal) keep = c == '(' || c == ')';
      if (keep) {
        out.push_back((char) c);
      } else {
        out.push_back('%');
        out.push_back(hex[c >> 4]);
        out.push_back(hex[c & 15]);
      }
    }
    return out;
  }

  /** Decodes every valid %XX. A '%' not followed by two hex digits stays as
   *  it is. '+' stays a '+' -- it is a space only in a form-encoded query,
   *  which getParameters() handles. */
  static std::string removeEscapeChars(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
      if (text[i] == '%' && i + 2 < text.size() && hexDigit(text[i + 1]) >= 0 && hexDigit(text[i + 2]) >= 0) {
        out.push_back((char) ((hexDigit(text[i + 1]) << 4) | hexDigit(text[i + 2])));
        i += 2;
      } else {
        out.push_back(text[i]);
      }
    }
    return out;
  }

  // ── Fetching ──

  /** A connected stream for this URL (http and https), or null when no
   *  response could be had -- a 404 IS a response, and comes back as a
   *  stream whose getStatusCode() says so. Blocks; never on the audio thread. */
  std::unique_ptr<WebInputStream> createInputStream(const InputStreamOptions& options) const;
  /** The same with the default options: a GET, 30 s timeout, up to five
   *  redirects. */
  std::unique_ptr<WebInputStream> createInputStream() const { return createInputStream(InputStreamOptions()); }

  /** The body as text (as bytes; no decoding), or empty on failure. */
  std::string readEntireTextStream(bool usePostCommand = false) const;
  /** The body as bytes: of whatever response arrived -- a 404's error page
   *  included -- or empty when none did. `usePostCommand` sends an empty
   *  POST instead of a GET. Blocks. */
  std::vector<uint8_t> readEntireBinaryStream(bool usePostCommand = false) const;

  /**
   * Downloads to a file, all or nothing: the body is written to a temporary
   * file beside the target and moved over it only when the whole response has
   * arrived with a 2xx status. A failed or cancelled download leaves the
   * target as it was. Progress and cancellation through
   * options.progressCallback.
   */
  DownloadResult downloadToFile(const std::string& targetFile, const InputStreamOptions& options) const;
  /** The same with the default options. */
  DownloadResult downloadToFile(const std::string& targetFile) const {
    return downloadToFile(targetFile, InputStreamOptions());
  }

  /** The same toString() text. Nothing is normalised beyond the scheme's
   *  case: "http://a" and "http://a/" are different URLs here. */
  bool operator==(const URL& other) const { return toString() == other.toString(); }
  /** Not operator==. */
  bool operator!=(const URL& other) const { return !(*this == other); }

private:
  friend class WebInputStream;

  static bool isAlpha(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
  static bool isDigit(unsigned char c) { return c >= '0' && c <= '9'; }
  static bool isUnreserved(unsigned char c) {
    return isAlpha(c) || isDigit(c) || c == '-' || c == '.' || c == '_' || c == '~';
  }
  static bool isSubDelim(unsigned char c) { return c != 0 && std::strchr("!$&'()*+,;=", c) != nullptr; }
  static int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  }
  static bool validScheme(const std::string& s) {
    if (s.empty() || !isAlpha((unsigned char) s[0])) return false;
    for (char ch : s) {
      const unsigned char c = (unsigned char) ch;
      if (!isAlpha(c) && !isDigit(c) && c != '+' && c != '-' && c != '.') return false;
    }
    return true;
  }
  /** Every character unreserved, a sub-delimiter, in `extra`, or a valid %XX. */
  static bool allowedChars(const std::string& s, const char* extra) {
    for (size_t i = 0; i < s.size(); ++i) {
      const unsigned char c = (unsigned char) s[i];
      if (c == '%') {
        if (i + 2 >= s.size() || hexDigit(s[i + 1]) < 0 || hexDigit(s[i + 2]) < 0) return false;
        i += 2;
        continue;
      }
      if (isUnreserved(c) || isSubDelim(c) || (c != 0 && std::strchr(extra, c) != nullptr)) continue;
      return false;
    }
    return true;
  }
  /** Escapes what may not appear (spaces, non-ASCII, controls, the backtick and
   *  any of `<>"{}|\^`, and gen-delims not in `keep`) while leaving valid %XX alone. */
  static std::string encodeLoosely(const std::string& s, const char* keep) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
      const unsigned char c = (unsigned char) s[i];
      const bool validEscape = c == '%' && i + 2 < s.size() && hexDigit(s[i + 1]) >= 0 && hexDigit(s[i + 2]) >= 0;
      if (validEscape || isUnreserved(c) || isSubDelim(c) || (c != 0 && std::strchr(keep, c) != nullptr)) {
        out.push_back((char) c);
      } else {
        out.push_back('%');
        out.push_back(hex[c >> 4]);
        out.push_back(hex[c & 15]);
      }
    }
    return out;
  }
  static std::string decodeForm(const std::string& s) {
    std::string spaced = s;
    for (char& c : spaced)
      if (c == '+') c = ' ';
    return removeEscapeChars(spaced);
  }

  /** RFC 3986 5.2.4. */
  static std::string removeDotSegments(const std::string& input) {
    std::string in = input, out;
    while (!in.empty()) {
      if (in.compare(0, 3, "../") == 0) in.erase(0, 3);
      else if (in.compare(0, 2, "./") == 0) in.erase(0, 2);
      else if (in.compare(0, 3, "/./") == 0) in.erase(0, 2);
      else if (in == "/.") in = "/";
      else if (in.compare(0, 4, "/../") == 0 || in == "/..") {
        in = in.size() == 3 ? std::string("/") : in.substr(3);
        const size_t slash = out.rfind('/');
        out.erase(slash == std::string::npos ? 0 : slash);
      } else if (in == "." || in == "..") {
        in.clear();
      } else {
        // Move the first segment, with its leading '/' if any, to the output.
        size_t next = in.find('/', in[0] == '/' ? 1 : 0);
        if (next == std::string::npos) next = in.size();
        out += in.substr(0, next);
        in.erase(0, next);
      }
    }
    return out;
  }
  /** RFC 3986 5.2.3. */
  std::string merge(const std::string& relativePath) const {
    if (hasAuthority_ && path_.empty()) return "/" + relativePath;
    const size_t slash = path_.rfind('/');
    if (slash == std::string::npos) return relativePath;
    return path_.substr(0, slash + 1) + relativePath;
  }

  void copyAuthority(const URL& from) {
    hasAuthority_ = from.hasAuthority_;
    hasUserInfo_ = from.hasUserInfo_;
    userInfo_ = from.userInfo_;
    host_ = from.host_;
    ipLiteral_ = from.ipLiteral_;
    hasPort_ = from.hasPort_;
    port_ = from.port_;
  }
  void setQuery(const URL& from) {
    hasQuery_ = from.hasQuery_;
    query_ = from.query_;
  }

  int portNumber() const noexcept {
    if (!hasPort_ || port_.empty() || port_.size() > 6) return 0;
    int v = 0;
    for (char c : port_) {
      if (c < '0' || c > '9') return 0;
      v = v * 10 + (c - '0');
    }
    return v;
  }

  /** RFC 3986 Appendix B's split, with the authority taken apart. */
  void parse(const std::string& s) {
    size_t i = 0;
    const size_t colon = s.find(':');
    const size_t delimiter = s.find_first_of("/?#");
    if (colon != std::string::npos && colon > 0 && (delimiter == std::string::npos || colon < delimiter) &&
        validScheme(s.substr(0, colon))) {
      scheme_ = s.substr(0, colon);
      for (char& c : scheme_) c = asciiLowerChar(c);
      i = colon + 1;
    }
    if (s.compare(i, 2, "//") == 0) {
      hasAuthority_ = true;
      i += 2;
      size_t end = s.find_first_of("/?#", i);
      if (end == std::string::npos) end = s.size();
      parseAuthority(s.substr(i, end - i));
      i = end;
    }
    size_t end = s.find_first_of("?#", i);
    if (end == std::string::npos) end = s.size();
    path_ = s.substr(i, end - i);
    i = end;
    if (i < s.size() && s[i] == '?') {
      hasQuery_ = true;
      size_t hash = s.find('#', i + 1);
      if (hash == std::string::npos) hash = s.size();
      query_ = s.substr(i + 1, hash - i - 1);
      i = hash;
    }
    if (i < s.size() && s[i] == '#') {
      hasFragment_ = true;
      fragment_ = s.substr(i + 1);
    }
    rebuild();
  }

  void parseAuthority(const std::string& authority) {
    std::string hostPort = authority;
    const size_t at = authority.rfind('@');
    if (at != std::string::npos) {
      hasUserInfo_ = true;
      userInfo_ = authority.substr(0, at);
      hostPort = authority.substr(at + 1);
    }
    if (!hostPort.empty() && hostPort[0] == '[') {
      const size_t close = hostPort.find(']');
      if (close != std::string::npos) {
        ipLiteral_ = true;
        host_ = hostPort.substr(1, close - 1);
        for (char& c : host_) c = asciiLowerChar(c);
        const std::string rest = hostPort.substr(close + 1);
        if (!rest.empty() && rest[0] == ':') {
          hasPort_ = true;
          port_ = rest.substr(1);
        } else if (!rest.empty()) {
          // Junk after the bracket: kept in the host so nothing is lost, and
          // the URL is not well formed.
          ipLiteral_ = false;
          host_ = hostPort;
        }
        return;
      }
    }
    const size_t portColon = hostPort.rfind(':');
    if (portColon != std::string::npos) {
      hasPort_ = true;
      port_ = hostPort.substr(portColon + 1);
      host_ = hostPort.substr(0, portColon);
    } else {
      host_ = hostPort;
    }
  }

  void rebuild() { text_ = toString(); }

  std::string text_;
  std::string scheme_;
  std::string userInfo_;
  std::string host_;
  std::string port_;
  std::string path_;
  std::string query_;
  std::string fragment_;
  bool hasAuthority_ = false;
  bool hasUserInfo_ = false;
  bool hasPort_ = false;
  bool ipLiteral_ = false;
  bool hasQuery_ = false;
  bool hasFragment_ = false;
};

// ═══════════════════════════════════════════════════════════════════════════
// HTTP
// ═══════════════════════════════════════════════════════════════════════════

namespace netdetail {

using HeaderList = std::vector<std::pair<std::string, std::string>>;

inline bool sameHeaderName(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (asciiLowerChar(a[i]) != asciiLowerChar(b[i])) return false;
  return true;
}

inline std::string findHeader(const HeaderList& headers, const std::string& name) {
  for (const auto& h : headers)
    if (sameHeaderName(h.first, name)) return h.second;
  return std::string();
}

inline void removeHeader(HeaderList& headers, const std::string& name) {
  headers.erase(std::remove_if(headers.begin(), headers.end(),
                               [&](const std::pair<std::string, std::string>& h) { return sameHeaderName(h.first, name); }),
                headers.end());
}

/** "HTTP/1.1 404 Not Found" -> 404; 0 when it is not a status line. */
inline int parseStatusLine(const std::string& line) {
  if (line.compare(0, 5, "HTTP/") != 0) return 0;
  size_t i = line.find(' ');
  if (i == std::string::npos) return 0;
  while (i < line.size() && line[i] == ' ') ++i;
  int code = 0, digits = 0;
  while (i < line.size() && line[i] >= '0' && line[i] <= '9' && digits < 3) {
    code = code * 10 + (line[i++] - '0');
    ++digits;
  }
  if (digits != 3 || (i < line.size() && line[i] != ' ')) return 0;
  return code;
}

/** "Name: value" into its parts, whitespace trimmed; false for anything else
 *  (a status line, an empty line, a name with spaces in it). */
inline bool parseHeaderLine(const std::string& line, std::string& name, std::string& value) {
  const size_t colon = line.find(':');
  if (colon == std::string::npos || colon == 0) return false;
  for (size_t i = 0; i < colon; ++i) {
    const unsigned char c = (unsigned char) line[i];
    if (c <= ' ' || c >= 127) return false;
  }
  size_t a = colon + 1, b = line.size();
  while (a < b && (line[a] == ' ' || line[a] == '\t')) ++a;
  while (b > a && (line[b - 1] == ' ' || line[b - 1] == '\t' || line[b - 1] == '\r' || line[b - 1] == '\n')) --b;
  name = line.substr(0, colon);
  value = line.substr(a, b - a);
  return true;
}

/** A block of header lines (CRLF or LF), the status line first: the status,
 *  and the headers into `out`. */
inline int parseHeaderBlock(const std::string& block, HeaderList& out) {
  int status = 0;
  size_t start = 0;
  while (start < block.size()) {
    size_t end = block.find('\n', start);
    if (end == std::string::npos) end = block.size();
    std::string line = block.substr(start, end - start);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::string name, value;
    const int s = parseStatusLine(line);
    if (s) {
      status = s;
      out.clear(); // a later status line (after a 100 Continue) starts again
    } else if (parseHeaderLine(line, name, value)) {
      out.emplace_back(name, value);
    }
    start = end + 1;
  }
  return status;
}

/** RFC 7230's token: what a method and a header name are made of. */
inline bool isHttpToken(const std::string& s) {
  if (s.empty()) return false;
  for (char ch : s) {
    const unsigned char c = (unsigned char) ch;
    const bool alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    if (!alnum && (c == 0 || std::strchr("!#$%&'*+-.^_`|~", c) == nullptr)) return false;
  }
  return true;
}

/** A header value that stays one header: no CR, LF or NUL. Every backend
 *  passes a value through as it is, and WinHTTP takes the headers as one
 *  CRLF-separated block -- a line break inside a value WAS a second header
 *  on the wire, with whatever name the value's author chose. */
inline bool isHttpFieldValue(const std::string& s) {
  for (char c : s)
    if (c == '\r' || c == '\n' || c == '\0') return false;
  return true;
}

/** A URL's path and query as a request target: "/" for an empty path,
 *  characters a request line cannot carry escaped, existing escapes kept. */
inline std::string requestTarget(const URL& url) {
  static const char hex[] = "0123456789ABCDEF";
  std::string raw = url.getPath().empty() ? std::string("/") : url.getPath();
  if (url.hasQuery()) raw += "?" + url.getQuery();
  std::string out;
  for (char ch : raw) {
    const unsigned char c = (unsigned char) ch;
    if (c <= ' ' || c >= 127 || std::strchr("\"<>\\^`{|}", c) != nullptr) {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 15]);
    } else {
      out.push_back((char) c);
    }
  }
  return out;
}

inline std::string base64(const std::string& in) {
  static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  size_t i = 0;
  while (i + 2 < in.size()) {
    const uint32_t v = ((uint32_t) (uint8_t) in[i] << 16) | ((uint32_t) (uint8_t) in[i + 1] << 8) | (uint8_t) in[i + 2];
    out += table[(v >> 18) & 63];
    out += table[(v >> 12) & 63];
    out += table[(v >> 6) & 63];
    out += table[v & 63];
    i += 3;
  }
  if (i + 1 == in.size()) {
    const uint32_t v = (uint32_t) (uint8_t) in[i] << 16;
    out += table[(v >> 18) & 63];
    out += table[(v >> 12) & 63];
    out += "==";
  } else if (i + 2 == in.size()) {
    const uint32_t v = ((uint32_t) (uint8_t) in[i] << 16) | ((uint32_t) (uint8_t) in[i + 1] << 8);
    out += table[(v >> 18) & 63];
    out += table[(v >> 12) & 63];
    out += table[(v >> 6) & 63];
    out += '=';
  }
  return out;
}

/** One request, as every backend is handed it. */
struct HttpRequest {
  URL url;            // http or https, no user info
  std::string target; // path?query, escaped
  std::string method;
  HeaderList headers;
  const std::vector<uint8_t>* body = nullptr;
  int timeoutMs = 30000;
};

struct HttpResponseHead {
  int status = 0;
  HeaderList headers;
};

/** One platform's HTTP, for one request. open() blocks until the response's
 *  headers (or a failure); read() until data, the end (0) or a failure (-1);
 *  cancel() may be called from any thread at any time, and makes whatever is
 *  blocked return. */
class HttpBackend {
public:
  virtual ~HttpBackend() = default;
  virtual bool open(const HttpRequest& request, HttpResponseHead& head, std::string& error) = 0;
  virtual int read(void* dest, int maxBytes, std::string& error) = 0;
  virtual void cancel() = 0;
};

/**
 * A deadline on another thread: arm(ms), and unless disarm() comes first,
 * the callback runs when it passes. How WebInputStream's timeout is kept --
 * the same on every platform, instead of three libraries' own timeouts,
 * which measured here do not agree (WinHTTP's waited for a response head
 * anywhere from one to three seconds past a 400 ms limit, depending on
 * options that have nothing to do with time).
 */
class Watchdog {
public:
  explicit Watchdog(std::function<void()> onExpire) : onExpire_(std::move(onExpire)) {}
  ~Watchdog() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      quit_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
  }
  Watchdog(const Watchdog&) = delete;
  Watchdog& operator=(const Watchdog&) = delete;

  void arm(int ms) {
    if (ms <= 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    armed_ = true;
    if (!thread_.joinable()) thread_ = std::thread([this] { run(); });
    cv_.notify_all();
  }
  void disarm() {
    std::lock_guard<std::mutex> lock(mutex_);
    armed_ = false;
    cv_.notify_all();
  }

private:
  void run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!quit_) {
      if (!armed_) {
        cv_.wait(lock);
        continue;
      }
      const auto deadline = deadline_;
      cv_.wait_until(lock, deadline);
      if (!quit_ && armed_ && deadline_ == deadline && std::chrono::steady_clock::now() >= deadline) {
        armed_ = false;
        lock.unlock();
        onExpire_();
        lock.lock();
      }
    }
  }

  std::function<void()> onExpire_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::thread thread_;
  std::chrono::steady_clock::time_point deadline_;
  bool armed_ = false;
  bool quit_ = false;
};

/**
 * The hand-over between a platform that PUSHES a response (libcurl's
 * callbacks, NSURLSession's delegate) and a reader that pulls it: the head
 * once, then bytes. Bounded -- a megabyte -- so a fast server and a slow
 * reader do not buffer a whole download in memory: the producer waits for
 * room, and cancel() releases it.
 */
class HttpPipe {
public:
  static constexpr size_t kCapacity = 1u << 20;

  void setHead(const HttpResponseHead& head) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (headReady_) return;
    head_ = head;
    headReady_ = true;
    cv_.notify_all();
  }
  /** False when cancelled: the producer should abort its transfer. */
  bool push(const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    std::unique_lock<std::mutex> lock(mutex_);
    while (n > 0) {
      cv_.wait(lock, [this] { return cancelled_ || buffered() < kCapacity; });
      if (cancelled_) return false;
      const size_t room = kCapacity - buffered();
      const size_t take = n < room ? n : room;
      if (readPos_ > 0 && readPos_ == data_.size()) {
        data_.clear();
        readPos_ = 0;
      }
      data_.insert(data_.end(), p, p + take);
      p += take;
      n -= take;
      cv_.notify_all();
    }
    return true;
  }
  void finish(bool ok, const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    finished_ = true;
    failed_ = !ok;
    if (!ok && error_.empty()) error_ = error;
    cv_.notify_all();
  }
  void cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    cancelled_ = true;
    cv_.notify_all();
  }
  bool isCancelled() {
    std::lock_guard<std::mutex> lock(mutex_);
    return cancelled_;
  }
  /** Waits for the head; false (with the error) if the transfer ended or was
   *  cancelled first. */
  bool waitForHead(HttpResponseHead& head, std::string& error) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return headReady_ || finished_ || cancelled_; });
    if (cancelled_) {
      error = "cancelled";
      return false;
    }
    if (!headReady_) {
      error = error_.empty() ? std::string("the connection closed before a response") : error_;
      return false;
    }
    head = head_;
    return true;
  }
  int pop(void* dest, int maxBytes, std::string& error) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return cancelled_ || finished_ || buffered() > 0; });
    if (cancelled_) {
      error = "cancelled";
      return -1;
    }
    if (buffered() > 0) {
      const size_t n = (size_t) maxBytes < buffered() ? (size_t) maxBytes : buffered();
      std::memcpy(dest, data_.data() + readPos_, n);
      readPos_ += n;
      if (readPos_ == data_.size()) {
        data_.clear();
        readPos_ = 0;
      }
      cv_.notify_all();
      return (int) n;
    }
    if (failed_) {
      error = error_;
      return -1;
    }
    return 0;
  }

private:
  size_t buffered() const { return data_.size() - readPos_; }

  std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<uint8_t> data_;
  size_t readPos_ = 0;
  HttpResponseHead head_;
  std::string error_;
  bool headReady_ = false, finished_ = false, failed_ = false, cancelled_ = false;
};

#if defined(_WIN32)
// ── Windows: WinHTTP, loaded at run time ────────────────────────────────────

using HInternet = void*;
const unsigned long kWinHttpAccessDefaultProxy = 0, kWinHttpAccessAutomaticProxy = 4;
const unsigned long kWinHttpFlagSecure = 0x00800000, kWinHttpFlagEscapeDisable = 0x00000040;
const unsigned long kWinHttpOptionDisableFeature = 63, kWinHttpDisableRedirects = 0x00000002;
const unsigned long kWinHttpOptionReceiveResponseTimeout = 7;
const unsigned long kWinHttpOptionDecompression = 118, kWinHttpDecompressionAll = 0x3;
const unsigned long kWinHttpAddReqAdd = 0x20000000, kWinHttpAddReqReplace = 0x80000000;
const unsigned long kWinHttpQueryStatusCode = 19, kWinHttpQueryRawHeadersCrlf = 22,
                    kWinHttpQueryFlagNumber = 0x20000000;
const unsigned long kErrorWinHttpTimeout = 12002, kErrorWinHttpNameNotResolved = 12007,
                    kErrorWinHttpCancelled = 12017, kErrorWinHttpCannotConnect = 12029,
                    kErrorWinHttpConnectionError = 12030, kErrorWinHttpSecureFailure = 12175;

struct WinHttpApi {
  bool ok = false;
  HInternet(__stdcall* open)(const wchar_t*, unsigned long, const wchar_t*, const wchar_t*, unsigned long) = nullptr;
  HInternet(__stdcall* connect)(HInternet, const wchar_t*, uint16_t, unsigned long) = nullptr;
  HInternet(__stdcall* openRequest)(HInternet, const wchar_t*, const wchar_t*, const wchar_t*, const wchar_t*,
                                    const wchar_t**, unsigned long) = nullptr;
  int(__stdcall* setTimeouts)(HInternet, int, int, int, int) = nullptr;
  int(__stdcall* setOption)(HInternet, unsigned long, void*, unsigned long) = nullptr;
  int(__stdcall* addRequestHeaders)(HInternet, const wchar_t*, unsigned long, unsigned long) = nullptr;
  int(__stdcall* sendRequest)(HInternet, const wchar_t*, unsigned long, void*, unsigned long, unsigned long,
                              uintptr_t) = nullptr;
  int(__stdcall* receiveResponse)(HInternet, void*) = nullptr;
  int(__stdcall* queryHeaders)(HInternet, unsigned long, const wchar_t*, void*, unsigned long*, unsigned long*) = nullptr;
  int(__stdcall* readData)(HInternet, void*, unsigned long, unsigned long*) = nullptr;
  int(__stdcall* closeHandle)(HInternet) = nullptr;
};

inline const WinHttpApi& winhttp() {
  static const WinHttpApi api = [] {
    WinHttpApi a;
    HMODULE m = LoadLibraryExW(L"winhttp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) return a;
    resolveProc(m, "WinHttpOpen", a.open);
    resolveProc(m, "WinHttpConnect", a.connect);
    resolveProc(m, "WinHttpOpenRequest", a.openRequest);
    resolveProc(m, "WinHttpSetTimeouts", a.setTimeouts);
    resolveProc(m, "WinHttpSetOption", a.setOption);
    resolveProc(m, "WinHttpAddRequestHeaders", a.addRequestHeaders);
    resolveProc(m, "WinHttpSendRequest", a.sendRequest);
    resolveProc(m, "WinHttpReceiveResponse", a.receiveResponse);
    resolveProc(m, "WinHttpQueryHeaders", a.queryHeaders);
    resolveProc(m, "WinHttpReadData", a.readData);
    resolveProc(m, "WinHttpCloseHandle", a.closeHandle);
    a.ok = a.open && a.connect && a.openRequest && a.setTimeouts && a.setOption && a.addRequestHeaders &&
           a.sendRequest && a.receiveResponse && a.queryHeaders && a.readData && a.closeHandle;
    return a;
  }();
  return api;
}

/**
 * Synchronous WinHTTP. Cancellation closes the request handle from the
 * cancelling thread, which is WinHTTP's documented way to abandon a
 * synchronous call in progress on another: the blocked call returns
 * ERROR_WINHTTP_OPERATION_CANCELLED.
 */
class WinHttpBackend final : public HttpBackend {
public:
  ~WinHttpBackend() override {
    const WinHttpApi& api = winhttp();
    if (!api.ok) return;
    if (request_) api.closeHandle(request_);
    if (connect_) api.closeHandle(connect_);
    if (session_) api.closeHandle(session_);
  }

  bool open(const HttpRequest& r, HttpResponseHead& head, std::string& error) override {
    const WinHttpApi& api = winhttp();
    if (!api.ok) {
      error = "winhttp.dll could not be loaded";
      return false;
    }
    bool created;
    unsigned long createError = 0;
    {
      // Under the lock so a cancel() arriving now either comes first (and
      // nothing is created) or finds the request handle to close.
      std::lock_guard<std::mutex> lock(mutex_);
      if (cancelled_) return cancelledError(error);
      created = createHandles(api, r);
      if (!created) createError = GetLastError();
    }
    if (!created) return describe(createError, error);
    HInternet request = current();
    if (!request) return cancelledError(error);
    void* body = (r.body && !r.body->empty()) ? (void*) r.body->data() : nullptr;
    const unsigned long bodySize = body ? (unsigned long) r.body->size() : 0;
    if (!api.sendRequest(request, nullptr, 0, body, bodySize, bodySize, 0) || !api.receiveResponse(request, nullptr))
      return fail(error);
    unsigned long status = 0, size = sizeof(status);
    if (!api.queryHeaders(request, kWinHttpQueryStatusCode | kWinHttpQueryFlagNumber, nullptr, &status, &size,
                          nullptr))
      return fail(error);
    size = 0;
    api.queryHeaders(request, kWinHttpQueryRawHeadersCrlf, nullptr, nullptr, &size, nullptr);
    std::wstring raw(size / sizeof(wchar_t) + 1, L'\0');
    if (size && api.queryHeaders(request, kWinHttpQueryRawHeadersCrlf, nullptr, &raw[0], &size, nullptr)) {
      raw.resize(size / sizeof(wchar_t));
      parseHeaderBlock(pathio::narrow(raw.c_str()), head.headers);
    }
    head.status = (int) status;
    return true;
  }

  int read(void* dest, int maxBytes, std::string& error) override {
    HInternet request = current();
    if (!request) {
      cancelledError(error);
      return -1;
    }
    unsigned long got = 0;
    if (!winhttp().readData(request, dest, (unsigned long) maxBytes, &got)) {
      fail(error);
      return -1;
    }
    return (int) got;
  }

  void cancel() override {
    std::lock_guard<std::mutex> lock(mutex_);
    cancelled_ = true;
    if (request_) {
      winhttp().closeHandle(request_);
      request_ = nullptr;
    }
  }

private:
  /** Measured: each time the process's LAST WinHTTP session closes, WinHTTP
   *  keeps one handle it never gives back -- a session per request, this
   *  backend's way, leaked one per request (a thousand after a thousand
   *  downloads, still held after the process sat idle), and WinHTTP called
   *  directly the same way leaks the same. One session held open for the
   *  life of the process, never used for a request, keeps the count flat. */
  static void holdOneSessionOpen(const WinHttpApi& api) {
    static const HInternet anchor = [&api] {
      HInternet s = api.open(L"Sonore-SDK", kWinHttpAccessAutomaticProxy, nullptr, nullptr, 0);
      return s ? s : api.open(L"Sonore-SDK", kWinHttpAccessDefaultProxy, nullptr, nullptr, 0);
    }();
    (void) anchor;
  }

  /** The session, connection and request handles; the caller holds mutex_. */
  bool createHandles(const WinHttpApi& api, const HttpRequest& r) {
    holdOneSessionOpen(api);
    session_ = api.open(L"Sonore-SDK", kWinHttpAccessAutomaticProxy, nullptr, nullptr, 0);
    // Before Windows 8.1 the automatic proxy mode does not exist.
    if (!session_) session_ = api.open(L"Sonore-SDK", kWinHttpAccessDefaultProxy, nullptr, nullptr, 0);
    if (!session_) return false;
    const int t = r.timeoutMs > 0 ? r.timeoutMs : 30000;
    api.setTimeouts(session_, t, t, t, t);
    unsigned long decompress = kWinHttpDecompressionAll; // Windows 8.1+; refused before, harmlessly
    api.setOption(session_, kWinHttpOptionDecompression, &decompress, sizeof(decompress));
    // An IPv6 literal goes to WinHTTP in its brackets, as in a URL.
    const std::string& name = r.url.getHost();
    const std::wstring host =
        pathio::widen((name.find(':') != std::string::npos ? "[" + name + "]" : name).c_str());
    connect_ = api.connect(session_, host.c_str(), (uint16_t) r.url.getPortOrDefault(), 0);
    if (!connect_) return false;
    const std::wstring method = pathio::widen(r.method.c_str());
    const std::wstring target = pathio::widen(r.target.c_str());
    // ESCAPE_DISABLE: the target is escaped already (requestTarget), and
    // WinHTTP escaping it again would turn %20 into %2520.
    const unsigned long flags = (r.url.getScheme() == "https" ? kWinHttpFlagSecure : 0) | kWinHttpFlagEscapeDisable;
    request_ = api.openRequest(connect_, method.c_str(), target.c_str(), nullptr, nullptr, nullptr, flags);
    if (!request_) return false;
    unsigned long noRedirects = kWinHttpDisableRedirects;
    api.setOption(request_, kWinHttpOptionDisableFeature, &noRedirects, sizeof(noRedirects));
    // WinHttpReceiveResponse waits by its OWN timeout, ninety seconds unless
    // set, not the receive timeout above: a server that accepted the request
    // and then said nothing held the caller for a minute and a half.
    unsigned long responseTimeout = (unsigned long) t;
    api.setOption(request_, kWinHttpOptionReceiveResponseTimeout, &responseTimeout, sizeof(responseTimeout));
    std::string lines;
    for (const auto& h : r.headers) lines += h.first + ": " + h.second + "\r\n";
    if (!lines.empty()) {
      const std::wstring w = pathio::widen(lines.c_str());
      api.addRequestHeaders(request_, w.c_str(), (unsigned long) -1, kWinHttpAddReqAdd | kWinHttpAddReqReplace);
    }
    return true;
  }

  HInternet current() {
    std::lock_guard<std::mutex> lock(mutex_);
    return cancelled_ ? nullptr : request_;
  }
  bool cancelledError(std::string& error) {
    error = "cancelled";
    return false;
  }
  bool fail(std::string& error) { return describe(GetLastError(), error); }
  bool describe(unsigned long code, std::string& error) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (cancelled_ || code == kErrorWinHttpCancelled) {
        error = "cancelled";
        return false;
      }
    }
    char buffer[96];
    switch (code) {
    case kErrorWinHttpTimeout: error = "timed out"; break;
    case kErrorWinHttpNameNotResolved: error = "the host name could not be resolved"; break;
    case kErrorWinHttpCannotConnect: error = "could not connect to the server"; break;
    case kErrorWinHttpConnectionError: error = "the connection was reset"; break;
    case kErrorWinHttpSecureFailure: error = "the secure connection failed (certificate or TLS)"; break;
    default:
      std::snprintf(buffer, sizeof(buffer), "WinHTTP error %lu", code);
      error = buffer;
    }
    return false;
  }

  std::mutex mutex_;
  bool cancelled_ = false;
  HInternet session_ = nullptr;
  HInternet connect_ = nullptr;
  HInternet request_ = nullptr;
};

inline std::unique_ptr<HttpBackend> makeHttpBackend() { return std::make_unique<WinHttpBackend>(); }

#elif defined(__APPLE__)
// ── macOS: NSURLSession through the Objective-C runtime ─────────────────────

template <typename Ret, typename... Args>
inline Ret nsMsg(id target, SEL selector, Args... args) {
  using Fn = Ret (*)(id, SEL, Args...);
  return reinterpret_cast<Fn>(objc_msgSend)(target, selector, args...);
}
inline id nsClass(const char* name) { return (id) objc_getClass(name); }
inline SEL nsSel(const char* name) { return sel_registerName(name); }
inline id nsStr(const std::string& s) {
  return nsMsg<id>(nsClass("NSString"), nsSel("stringWithUTF8String:"), s.c_str());
}
inline std::string fromNs(id str) {
  if (!str) return std::string();
  const char* utf8 = nsMsg<const char*>(str, nsSel("UTF8String"));
  return utf8 ? std::string(utf8) : std::string();
}

/** What the delegate writes into: owned by the backend, reached from the
 *  delegate through an instance variable, and outliving every delegate call
 *  because the backend waits for the session to be invalidated first. */
struct NsUrlState {
  HttpPipe pipe;
  std::mutex mutex;
  std::condition_variable cv;
  bool invalidated = false;
};

inline NsUrlState* nsState(id self) {
  void* p = nullptr;
  object_getInstanceVariable(self, "sonoreState", &p);
  return static_cast<NsUrlState*>(p);
}

/** A block's invoke pointer follows isa, flags and reserved in every block. */
template <typename Arg>
inline void callBlock(id block, Arg arg) {
  struct BlockHeader {
    void* isa;
    int flags;
    int reserved;
    void (*invoke)(void*, Arg);
  };
  auto* b = reinterpret_cast<BlockHeader*>(block);
  if (b && b->invoke) b->invoke(b, arg);
}

inline void nsDidReceiveResponse(id self, SEL, id, id, id response, id completion) {
  if (NsUrlState* s = nsState(self)) {
    HttpResponseHead head;
    if (response && nsMsg<BOOL>(response, nsSel("respondsToSelector:"), nsSel("statusCode")))
      head.status = (int) nsMsg<long>(response, nsSel("statusCode"));
    id fields = response && nsMsg<BOOL>(response, nsSel("respondsToSelector:"), nsSel("allHeaderFields"))
                    ? nsMsg<id>(response, nsSel("allHeaderFields"))
                    : nullptr;
    if (fields) {
      id keys = nsMsg<id>(fields, nsSel("allKeys"));
      const unsigned long count = keys ? nsMsg<unsigned long>(keys, nsSel("count")) : 0;
      for (unsigned long i = 0; i < count; ++i) {
        id key = nsMsg<id>(keys, nsSel("objectAtIndex:"), i);
        id value = nsMsg<id>(fields, nsSel("objectForKey:"), key);
        head.headers.emplace_back(fromNs(key), fromNs(value));
      }
    }
    s->pipe.setHead(head);
  }
  callBlock<long>(completion, 1L); // NSURLSessionResponseAllow
}

inline void nsDidReceiveData(id self, SEL, id, id task, id data) {
  NsUrlState* s = nsState(self);
  if (!s || !data) return;
  const void* bytes = nsMsg<const void*>(data, nsSel("bytes"));
  const unsigned long length = nsMsg<unsigned long>(data, nsSel("length"));
  // Blocks this session's own delegate queue while the reader catches up.
  if (bytes && length && !s->pipe.push(bytes, length)) nsMsg<void>(task, nsSel("cancel"));
}

inline void nsDidComplete(id self, SEL, id, id, id error) {
  NsUrlState* s = nsState(self);
  if (!s) return;
  if (!error) {
    s->pipe.finish(true, std::string());
    return;
  }
  const long code = nsMsg<long>(error, nsSel("code"));
  std::string text = fromNs(nsMsg<id>(error, nsSel("localizedDescription")));
  if (code == -1001) text = "timed out"; // NSURLErrorTimedOut
  if (code == -999) text = "cancelled";  // NSURLErrorCancelled
  s->pipe.finish(false, text);
}

/** Redirects are refused here and followed by WebInputStream: completing
 *  with nil delivers the 3xx itself. */
inline void nsWillRedirect(id, SEL, id, id, id, id, id completion) { callBlock<id>(completion, (id) nullptr); }

inline void nsDidBecomeInvalid(id self, SEL, id, id) {
  if (NsUrlState* s = nsState(self)) {
    std::lock_guard<std::mutex> lock(s->mutex);
    s->invalidated = true;
    s->cv.notify_all();
  }
}

inline Class nsDelegateClass() {
  static Class klass = [] {
    // Unique to this binary: two plugins built from this SDK share one
    // Objective-C runtime, and a shared name would hand one the other's
    // methods (see webview_cocoa.h's uniqueClassName).
    char name[96];
    std::snprintf(name, sizeof(name), "SonoreURLSessionDelegate_%llx",
                  (unsigned long long) (uintptr_t) (void*) &nsDidReceiveData);
    if (Class existing = objc_getClass(name)) return existing;
    Class c = objc_allocateClassPair((Class) nsClass("NSObject"), name, 0);
    if (!c) return (Class) objc_getClass(name);
    class_addIvar(c, "sonoreState", sizeof(void*), (uint8_t) (sizeof(void*) == 8 ? 3 : 2), "^v");
    class_addMethod(c, nsSel("URLSession:dataTask:didReceiveResponse:completionHandler:"),
                    (IMP) nsDidReceiveResponse, "v@:@@@@?");
    class_addMethod(c, nsSel("URLSession:dataTask:didReceiveData:"), (IMP) nsDidReceiveData, "v@:@@@");
    class_addMethod(c, nsSel("URLSession:task:didCompleteWithError:"), (IMP) nsDidComplete, "v@:@@@");
    class_addMethod(c, nsSel("URLSession:task:willPerformHTTPRedirection:newRequest:completionHandler:"),
                    (IMP) nsWillRedirect, "v@:@@@@@?");
    class_addMethod(c, nsSel("URLSession:didBecomeInvalidWithError:"), (IMP) nsDidBecomeInvalid, "v@:@@");
    objc_registerClassPair(c);
    return c;
  }();
  return klass;
}

class NsUrlBackend final : public HttpBackend {
public:
  ~NsUrlBackend() override {
    state_.pipe.cancel(); // releases a delegate call waiting for room
    if (session_) {
      nsMsg<void>(session_, nsSel("invalidateAndCancel"));
      std::unique_lock<std::mutex> lock(state_.mutex);
      state_.cv.wait_for(lock, std::chrono::seconds(10), [this] { return state_.invalidated; });
    }
    if (delegate_) object_setInstanceVariable(delegate_, "sonoreState", nullptr);
    if (task_) nsMsg<void>(task_, nsSel("release"));
    if (session_) nsMsg<void>(session_, nsSel("release"));
    if (delegate_) nsMsg<void>(delegate_, nsSel("release"));
  }

  bool open(const HttpRequest& r, HttpResponseHead& head, std::string& error) override {
    id pool = nsMsg<id>(nsMsg<id>(nsClass("NSAutoreleasePool"), nsSel("alloc")), nsSel("init"));
    const double seconds = (r.timeoutMs > 0 ? r.timeoutMs : 30000) / 1000.0;
    id config = nsMsg<id>(nsClass("NSURLSessionConfiguration"), nsSel("ephemeralSessionConfiguration"));
    nsMsg<void>(config, nsSel("setTimeoutIntervalForRequest:"), seconds);
    delegate_ = nsMsg<id>(nsMsg<id>((id) nsDelegateClass(), nsSel("alloc")), nsSel("init"));
    object_setInstanceVariable(delegate_, "sonoreState", &state_);
    session_ = nsMsg<id>(nsClass("NSURLSession"), nsSel("sessionWithConfiguration:delegate:delegateQueue:"), config,
                         delegate_, (id) nullptr);
    if (session_) nsMsg<id>(session_, nsSel("retain"));
    // The origin and the escaped target: what goes on the wire.
    const std::string text = r.url.getOrigin() + r.target;
    id nsurl = nsMsg<id>(nsClass("NSURL"), nsSel("URLWithString:"), nsStr(text));
    if (!session_ || !nsurl) {
      nsMsg<void>(pool, nsSel("drain"));
      error = nsurl ? "NSURLSession could not be created" : "the URL was refused by NSURL";
      return false;
    }
    id request = nsMsg<id>(nsClass("NSMutableURLRequest"), nsSel("requestWithURL:"), nsurl);
    nsMsg<void>(request, nsSel("setHTTPMethod:"), nsStr(r.method));
    nsMsg<void>(request, nsSel("setTimeoutInterval:"), seconds);
    for (const auto& h : r.headers)
      nsMsg<void>(request, nsSel("setValue:forHTTPHeaderField:"), nsStr(h.second), nsStr(h.first));
    if (r.body && !r.body->empty()) {
      id data = nsMsg<id>(nsClass("NSData"), nsSel("dataWithBytes:length:"), (const void*) r.body->data(),
                          (unsigned long) r.body->size());
      nsMsg<void>(request, nsSel("setHTTPBody:"), data);
    }
    task_ = nsMsg<id>(session_, nsSel("dataTaskWithRequest:"), request);
    if (task_) {
      nsMsg<id>(task_, nsSel("retain"));
      nsMsg<void>(task_, nsSel("resume"));
    }
    nsMsg<void>(pool, nsSel("drain"));
    if (!task_) {
      error = "NSURLSession refused the request";
      return false;
    }
    return state_.pipe.waitForHead(head, error);
  }

  int read(void* dest, int maxBytes, std::string& error) override { return state_.pipe.pop(dest, maxBytes, error); }

  void cancel() override {
    state_.pipe.cancel();
    if (task_) nsMsg<void>(task_, nsSel("cancel"));
  }

private:
  NsUrlState state_;
  id delegate_ = nullptr;
  id session_ = nullptr;
  id task_ = nullptr;
};

inline std::unique_ptr<HttpBackend> makeHttpBackend() { return std::make_unique<NsUrlBackend>(); }

#elif defined(__linux__)
// ── Linux: libcurl, dlopened ────────────────────────────────────────────────

struct CurlApi {
  bool ok = false;
  int (*globalInit)(long) = nullptr;
  void* (*easyInit)() = nullptr;
  int (*easySetopt)(void*, int, ...) = nullptr;
  int (*easyPerform)(void*) = nullptr;
  void (*easyCleanup)(void*) = nullptr;
  const char* (*easyStrerror)(int) = nullptr;
  void* (*slistAppend)(void*, const char*) = nullptr;
  void (*slistFreeAll)(void*) = nullptr;
};

// curl.h's values (CURLOPTTYPE_LONG 0, OBJECTPOINT 10000, FUNCTIONPOINT
// 20000, OFF_T 30000), fixed in libcurl's ABI since 7.x.
const int kCurlOptWriteData = 10001, kCurlOptUrl = 10002, kCurlOptErrorBuffer = 10010,
          kCurlOptWriteFunction = 20011, kCurlOptPostFields = 10015, kCurlOptUserAgent = 10018,
          kCurlOptLowSpeedLimit = 19, kCurlOptLowSpeedTime = 20, kCurlOptHttpHeader = 10023,
          kCurlOptHeaderData = 10029, kCurlOptCustomRequest = 10036, kCurlOptNoProgress = 43,
          kCurlOptNoBody = 44, kCurlOptPost = 47, kCurlOptFollowLocation = 52, kCurlOptXferInfoData = 10057,
          kCurlOptHeaderFunction = 20079, kCurlOptHttpGet = 80, kCurlOptNoSignal = 99,
          kCurlOptAcceptEncoding = 10102, kCurlOptPostFieldSizeLarge = 30120, kCurlOptConnectTimeoutMs = 156,
          kCurlOptProtocols = 181, kCurlOptRedirProtocols = 182, kCurlOptXferInfoFunction = 20219,
          kCurlOptSuppressConnectHeaders = 265;
const long kCurlProtoHttp = 1, kCurlProtoHttps = 2;
const int kCurleOk = 0, kCurleOperationTimedOut = 28, kCurleAbortedByCallback = 42;

inline const CurlApi& curl() {
  static const CurlApi api = [] {
    CurlApi a;
    void* lib = dlopen("libcurl.so.4", RTLD_NOW | RTLD_LOCAL);
    if (!lib) lib = dlopen("libcurl-gnutls.so.4", RTLD_NOW | RTLD_LOCAL);
    if (!lib) lib = dlopen("libcurl.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return a;
    a.globalInit = reinterpret_cast<int (*)(long)>(dlsym(lib, "curl_global_init"));
    a.easyInit = reinterpret_cast<void* (*)()>(dlsym(lib, "curl_easy_init"));
    a.easySetopt = reinterpret_cast<int (*)(void*, int, ...)>(dlsym(lib, "curl_easy_setopt"));
    a.easyPerform = reinterpret_cast<int (*)(void*)>(dlsym(lib, "curl_easy_perform"));
    a.easyCleanup = reinterpret_cast<void (*)(void*)>(dlsym(lib, "curl_easy_cleanup"));
    a.easyStrerror = reinterpret_cast<const char* (*)(int)>(dlsym(lib, "curl_easy_strerror"));
    a.slistAppend = reinterpret_cast<void* (*)(void*, const char*)>(dlsym(lib, "curl_slist_append"));
    a.slistFreeAll = reinterpret_cast<void (*)(void*)>(dlsym(lib, "curl_slist_free_all"));
    a.ok = a.globalInit && a.easyInit && a.easySetopt && a.easyPerform && a.easyCleanup && a.easyStrerror &&
           a.slistAppend && a.slistFreeAll;
    // Once per process, before any other call, and here while this function
    // is the only thread able to reach libcurl through us.
    if (a.ok) a.globalInit(3 /* CURL_GLOBAL_DEFAULT */);
    return a;
  }();
  return api;
}

/** curl_easy_perform on a thread of its own, pushing into an HttpPipe. */
class CurlBackend final : public HttpBackend {
public:
  ~CurlBackend() override {
    cancel();
    if (worker_.joinable()) worker_.join();
    const CurlApi& api = curl();
    if (easy_) api.easyCleanup(easy_);
    if (headerList_) api.slistFreeAll(headerList_);
  }

  bool open(const HttpRequest& r, HttpResponseHead& head, std::string& error) override {
    const CurlApi& api = curl();
    if (!api.ok) {
      error = "libcurl is not installed (libcurl.so.4 could not be loaded)";
      return false;
    }
    easy_ = api.easyInit();
    if (!easy_) {
      error = "curl_easy_init failed";
      return false;
    }
    method_ = r.method;
    // A copy: the transfer may still be sending the body after the response
    // head has arrived and the caller's request has gone.
    if (r.body) body_ = *r.body;
    url_ = r.url.getOrigin() + r.target;
    api.easySetopt(easy_, kCurlOptUrl, url_.c_str());
    api.easySetopt(easy_, kCurlOptNoSignal, 1L); // threads: never SIGALRM for timeouts
    api.easySetopt(easy_, kCurlOptProtocols, kCurlProtoHttp | kCurlProtoHttps);
    api.easySetopt(easy_, kCurlOptRedirProtocols, kCurlProtoHttp | kCurlProtoHttps);
    api.easySetopt(easy_, kCurlOptFollowLocation, 0L);
    api.easySetopt(easy_, kCurlOptSuppressConnectHeaders, 1L);
    api.easySetopt(easy_, kCurlOptAcceptEncoding, "");
    api.easySetopt(easy_, kCurlOptUserAgent, "Sonore-SDK");
    api.easySetopt(easy_, kCurlOptErrorBuffer, errorBuffer_);
    const long timeout = r.timeoutMs > 0 ? r.timeoutMs : 30000;
    api.easySetopt(easy_, kCurlOptConnectTimeoutMs, timeout);
    api.easySetopt(easy_, kCurlOptLowSpeedLimit, 1L);
    api.easySetopt(easy_, kCurlOptLowSpeedTime, (timeout + 999) / 1000);
    api.easySetopt(easy_, kCurlOptWriteFunction, &CurlBackend::onWrite);
    api.easySetopt(easy_, kCurlOptWriteData, this);
    api.easySetopt(easy_, kCurlOptHeaderFunction, &CurlBackend::onHeader);
    api.easySetopt(easy_, kCurlOptHeaderData, this);
    api.easySetopt(easy_, kCurlOptNoProgress, 0L);
    api.easySetopt(easy_, kCurlOptXferInfoFunction, &CurlBackend::onProgress);
    api.easySetopt(easy_, kCurlOptXferInfoData, this);
    const bool hasBody = !body_.empty();
    if (r.method == "GET" && !hasBody) {
      api.easySetopt(easy_, kCurlOptHttpGet, 1L);
    } else if (r.method == "HEAD") {
      api.easySetopt(easy_, kCurlOptNoBody, 1L);
    } else {
      if (r.method == "POST" || hasBody) {
        api.easySetopt(easy_, kCurlOptPost, 1L);
        api.easySetopt(easy_, kCurlOptPostFields, hasBody ? (const char*) body_.data() : "");
        api.easySetopt(easy_, kCurlOptPostFieldSizeLarge, (long long) body_.size());
      }
      if (r.method != "POST") api.easySetopt(easy_, kCurlOptCustomRequest, method_.c_str());
    }
    for (const auto& h : r.headers) headerList_ = api.slistAppend(headerList_, (h.first + ": " + h.second).c_str());
    // libcurl waits a second for "100 Continue" before a large body; servers
    // that never send one cost every upload that second.
    headerList_ = api.slistAppend(headerList_, "Expect:");
    api.easySetopt(easy_, kCurlOptHttpHeader, headerList_);
    worker_ = std::thread([this] {
      const int result = curl().easyPerform(easy_);
      std::string text;
      if (result == kCurleAbortedByCallback) text = "cancelled";
      else if (result == kCurleOperationTimedOut) text = "timed out";
      else if (result != kCurleOk) text = errorBuffer_[0] ? errorBuffer_ : curl().easyStrerror(result);
      // A response with no body (a 204, a HEAD) has its head here at the end.
      if (result == kCurleOk) completeHead();
      pipe_.finish(result == kCurleOk, text);
    });
    return pipe_.waitForHead(head, error);
  }

  int read(void* dest, int maxBytes, std::string& error) override { return pipe_.pop(dest, maxBytes, error); }
  void cancel() override {
    cancelled_ = true;
    pipe_.cancel();
  }

private:
  void completeHead() {
    if (status_ >= 200) {
      HttpResponseHead h;
      h.status = status_;
      h.headers = headers_;
      pipe_.setHead(h);
    }
  }

  static size_t onHeader(char* data, size_t size, size_t count, void* user) {
    auto* self = static_cast<CurlBackend*>(user);
    const size_t n = size * count;
    std::string line(data, n);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    std::string name, value;
    if (const int status = parseStatusLine(line)) {
      self->status_ = status;
      self->headers_.clear();
    } else if (line.empty()) {
      self->completeHead(); // the end of a final (not 1xx) response's head
    } else if (parseHeaderLine(line, name, value)) {
      self->headers_.emplace_back(name, value);
    }
    return n;
  }
  static size_t onWrite(char* data, size_t size, size_t count, void* user) {
    auto* self = static_cast<CurlBackend*>(user);
    const size_t n = size * count;
    self->completeHead();
    return self->pipe_.push(data, n) ? n : 0; // 0 aborts the transfer
  }
  static int onProgress(void* user, long long, long long, long long, long long) {
    return static_cast<CurlBackend*>(user)->cancelled_.load() ? 1 : 0;
  }

  HttpPipe pipe_;
  std::string method_;
  std::vector<uint8_t> body_;
  std::string url_;
  std::thread worker_;
  std::atomic<bool> cancelled_{false};
  void* easy_ = nullptr;
  void* headerList_ = nullptr;
  int status_ = 0;
  HeaderList headers_;
  char errorBuffer_[256] = {0};
};

inline std::unique_ptr<HttpBackend> makeHttpBackend() { return std::make_unique<CurlBackend>(); }

#else
inline std::unique_ptr<HttpBackend> makeHttpBackend() { return nullptr; }
#endif

} // namespace netdetail

/**
 * An HTTP or HTTPS request whose response is read as an InputStream.
 *
 * connect() sends the request and waits for the response's head, following
 * redirects (see the header); read() then streams the body. Both are called
 * by the first read if nobody called connect(). A response with an error
 * status is still a response: connect() is true, getStatusCode() says 404,
 * and the body is the server's explanation.
 *
 * cancel() may be called from any thread, and brings a blocked connect() or
 * read() back at once. A cancelled stream stays cancelled. The timeout
 * (InputStreamOptions::connectionTimeoutMs) is kept the same way, by a
 * deadline on a thread of the stream's own: the time to the response's head,
 * and then the longest silence while reading -- whatever the platform library
 * underneath would have done (see netdetail::Watchdog).
 *
 * Blocking, so never on the audio thread.
 */
class WebInputStream : public InputStream {
public:
  /** A request for `url`. Nothing is sent until connect(), or the first
   *  call that needs the response. */
  explicit WebInputStream(const URL& url, const URL::InputStreamOptions& options = URL::InputStreamOptions())
      : url_(url), options_(options) {}
  /** Releases the connection, ending a request still in progress. */
  ~WebInputStream() override {
    std::lock_guard<std::mutex> lock(mutex_);
    backend_.reset();
  }
  ///@{
  /** Not copyable: the stream owns its connection. */
  WebInputStream(const WebInputStream&) = delete;
  WebInputStream& operator=(const WebInputStream&) = delete;
  ///@}

  /** Sends the request and waits for the response head. True when a response
   *  arrived, whatever its status. Called again, answers the first result. */
  bool connect() {
    if (attempted_) return connected_;
    attempted_ = true;
    connected_ = run();
    return connected_;
  }

  /** True once something failed: no response, a refused request, a read
   *  error, a timeout, a cancel. An HTTP error status is not one -- see
   *  getStatusCode(). */
  bool isError() const { return !error_.empty(); }
  /** What failed, for a person; empty when nothing did. */
  const std::string& getErrorMessage() const noexcept { return error_; }
  /** The response's status, 0 when there was none. */
  int getStatusCode() const noexcept { return status_; }
  /** The final response's headers as name and value pairs; empty before
   *  connect(). */
  const netdetail::HeaderList& getResponseHeaders() const noexcept { return headers_; }
  /** A response header's value by name (case ignored), or empty. */
  std::string getResponseHeader(const std::string& name) const { return netdetail::findHeader(headers_, name); }
  /** Where the response came from, after redirects. */
  const URL& getFinalURL() const noexcept { return finalUrl_; }
  /** How many redirects were followed to reach getFinalURL(). */
  int getNumRedirectsFollowed() const noexcept { return redirects_; }

  /** Stops the request from any thread. */
  void cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    cancelled_ = true;
    if (backend_) backend_->cancel();
  }
  /** True after cancel(), including the one a progress callback causes by
   *  returning false. Any thread. */
  bool wasCancelled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cancelled_;
  }
  /** Whether the request ended because the server was silent too long. */
  bool hasTimedOut() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return timedOut_;
  }

  /** Content-Length, or -1: unknown for a chunked or compressed response
   *  (the platform decompresses, so the header counts different bytes). */
  int64_t getTotalLength() override {
    connect();
    return total_;
  }
  bool isExhausted() override {
    connect();
    return finished_ || !connected_;
  }
  /** Bytes of the body read so far. */
  int64_t getPosition() override { return position_; }

  /** Forward by reading. Backwards only for a GET or HEAD, by asking again
   *  from the start -- a POST is not repeated behind its caller's back. */
  bool setPosition(int64_t wanted) override {
    if (wanted < 0) wanted = 0;
    if (wanted < position_) {
      const std::string method = methodFor(options_);
      if (method != "GET" && method != "HEAD") return false;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cancelled_) return false;
        backend_.reset();
        timedOut_ = false;
      }
      attempted_ = connected_ = finished_ = false;
      position_ = 0;
      redirects_ = 0;
      error_.clear();
      if (!connect()) return false;
    }
    skipNextBytes(wanted - position_);
    return position_ == wanted;
  }

  int read(void* dest, int maxBytesToRead) override {
    if (!connect() || finished_ || !dest || maxBytesToRead <= 0) return 0;
    std::string error;
    watchdog_.arm(options_.connectionTimeoutMs);
    const int got = backend_->read(dest, maxBytesToRead, error);
    watchdog_.disarm();
    if (got < 0 && hasTimedOut()) error = "timed out";
    if (got <= 0) {
      finished_ = true;
      if (got < 0) error_ = error.empty() ? std::string("the connection failed") : error;
      return 0;
    }
    position_ += got;
    if (options_.progressCallback && !options_.progressCallback(position_, total_)) {
      cancel();
      error_ = "cancelled";
      finished_ = true;
    }
    return got;
  }

private:
  static std::string methodFor(const URL::InputStreamOptions& o) {
    if (!o.httpRequestCmd.empty()) return o.httpRequestCmd;
    return o.hasPostData ? "POST" : "GET";
  }
  static bool isRedirect(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
  }

  bool fail(const std::string& error) {
    error_ = error;
    return false;
  }

  /** The port as written is a port: digits, 1 to 65535 (or none written).
   *  WinHTTP takes it as 16 bits, so "host:65616" was a request to port 80
   *  while getPort() said 65616 -- somewhere other than the URL names. */
  static bool hasUsablePort(const URL& u) {
    if (!u.hasPort_ || u.port_.empty()) return true;
    if (u.port_.size() > 5) return false;
    for (char c : u.port_)
      if (c < '0' || c > '9') return false;
    const int p = u.portNumber();
    return p >= 1 && p <= 65535;
  }

  bool run() {
    URL current = url_;
    std::string method = methodFor(options_);
    std::vector<uint8_t> body = options_.postData;
    netdetail::HeaderList headers = options_.extraHeaders;
    // Refused rather than cleaned: a header that would have been split, or a
    // method that would have been a request line of its own, is a caller's
    // input that something upstream failed to check.
    if (!netdetail::isHttpToken(method)) return fail("the request method is not an HTTP token: \"" + method + "\"");
    for (const auto& h : headers)
      if (!netdetail::isHttpToken(h.first) || !netdetail::isHttpFieldValue(h.second))
        return fail("the request header \"" + h.first + "\" has a name or value HTTP cannot carry");
    for (int hop = 0;; ++hop) {
      if (current.getScheme() != "http" && current.getScheme() != "https")
        return fail("only http and https URLs can be fetched (\"" + current.getScheme() + "\" given)");
      if (current.getHost().empty()) return fail("the URL has no host");
      if (!hasUsablePort(current)) return fail("the URL's port is not a port: " + current.getOrigin());
      netdetail::HttpRequest request;
      request.method = method;
      request.headers = headers;
      // Credentials in the URL become a Basic header, and leave the URL: no
      // backend is handed a password to log.
      if (current.hasAuthority() && !current.getUserInfo().empty() &&
          netdetail::findHeader(request.headers, "Authorization").empty())
        request.headers.emplace_back("Authorization",
                                     "Basic " + netdetail::base64(URL::removeEscapeChars(current.getUserInfo())));
      URL bare = current;
      bare.hasUserInfo_ = false;
      bare.userInfo_.clear();
      bare.hasFragment_ = false;
      bare.fragment_.clear();
      bare.rebuild();
      request.url = bare;
      request.target = netdetail::requestTarget(bare);
      request.body = body.empty() ? nullptr : &body;
      request.timeoutMs = options_.connectionTimeoutMs;

      std::unique_ptr<netdetail::HttpBackend> backend = netdetail::makeHttpBackend();
      if (!backend) return fail("HTTP is not available on this platform");
      netdetail::HttpBackend* active = nullptr;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cancelled_) return fail("cancelled");
        backend_ = std::move(backend);
        active = backend_.get();
      }
      netdetail::HttpResponseHead head;
      std::string error;
      watchdog_.arm(options_.connectionTimeoutMs);
      const bool opened = active->open(request, head, error);
      watchdog_.disarm();
      if (!opened) return fail(hasTimedOut() ? std::string("timed out") : wasCancelled() ? std::string("cancelled") : error);
      status_ = head.status;
      headers_ = head.headers;
      finalUrl_ = bare;

      const std::string location = netdetail::findHeader(headers_, "Location");
      if (!isRedirect(status_) || location.empty() || hop >= options_.numRedirectsToFollow) break;

      const URL next = current.resolve(location);
      if (next.getScheme() != "http" && next.getScheme() != "https")
        return fail("redirected to an unsupported scheme: " + next.toString());
      if (status_ == 303 || ((status_ == 301 || status_ == 302) && method == "POST")) {
        if (method != "HEAD") method = "GET";
        body.clear();
        netdetail::removeHeader(headers, "Content-Type");
        netdetail::removeHeader(headers, "Content-Length");
      }
      if (next.getScheme() != current.getScheme() || next.getHost() != current.getHost() ||
          next.getPortOrDefault() != current.getPortOrDefault()) {
        // Another origin is not told this one's secrets.
        netdetail::removeHeader(headers, "Authorization");
        netdetail::removeHeader(headers, "Cookie");
        netdetail::removeHeader(headers, "Proxy-Authorization");
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        backend_.reset();
      }
      current = next;
      ++redirects_;
    }
    total_ = -1;
    const std::string length = netdetail::findHeader(headers_, "Content-Length");
    const std::string encoding = netdetail::findHeader(headers_, "Content-Encoding");
    if (!length.empty() && (encoding.empty() || netdetail::sameHeaderName(encoding, "identity"))) {
      int64_t v = 0;
      bool digits = true;
      for (char c : length) {
        if (c < '0' || c > '9' || v > (int64_t) 1e17) {
          digits = false;
          break;
        }
        v = v * 10 + (c - '0');
      }
      if (digits) total_ = v;
    }
    if (methodFor(options_) == "HEAD") finished_ = true;
    return true;
  }

  URL url_;
  URL::InputStreamOptions options_;
  mutable std::mutex mutex_;
  std::unique_ptr<netdetail::HttpBackend> backend_;
  bool cancelled_ = false;
  bool attempted_ = false;
  bool connected_ = false;
  bool finished_ = false;
  int status_ = 0;
  int redirects_ = 0;
  int64_t total_ = -1;
  int64_t position_ = 0;
  netdetail::HeaderList headers_;
  URL finalUrl_;
  std::string error_;
  bool timedOut_ = false;
  netdetail::Watchdog watchdog_{[this] {
    std::lock_guard<std::mutex> lock(mutex_);
    timedOut_ = true;
    if (backend_) backend_->cancel();
  }};
};

inline std::unique_ptr<WebInputStream> URL::createInputStream(const InputStreamOptions& options) const {
  auto stream = std::make_unique<WebInputStream>(*this, options);
  if (!stream->connect()) return nullptr;
  return stream;
}

inline std::vector<uint8_t> URL::readEntireBinaryStream(bool usePostCommand) const {
  std::vector<uint8_t> out;
  InputStreamOptions options;
  if (usePostCommand) options = options.withPostData(std::vector<uint8_t>()).withHttpRequestCmd("POST");
  if (auto stream = createInputStream(options)) stream->readIntoVector(out);
  return out;
}

inline std::string URL::readEntireTextStream(bool usePostCommand) const {
  const std::vector<uint8_t> bytes = readEntireBinaryStream(usePostCommand);
  return std::string(bytes.begin(), bytes.end());
}

inline URL::DownloadResult URL::downloadToFile(const std::string& targetFile, const InputStreamOptions& options) const {
  DownloadResult result;
  WebInputStream stream(*this, options);
  if (!stream.connect()) {
    result.error = stream.getErrorMessage();
    return result;
  }
  result.statusCode = stream.getStatusCode();
  if (result.statusCode < 200 || result.statusCode > 299) {
    char text[64];
    std::snprintf(text, sizeof(text), "the server answered %d", result.statusCode);
    result.error = text;
    return result;
  }
  TemporaryFile temp = TemporaryFile::forTarget(targetFile);
  {
    FileOutputStream out(temp.getFile());
    if (out.failedToOpen()) {
      result.error = "could not write " + temp.getFile();
      return result;
    }
    char buffer[65536];
    for (;;) {
      const int got = stream.read(buffer, (int) sizeof(buffer));
      if (got <= 0) break;
      if (!out.write(buffer, (size_t) got)) {
        result.error = "could not write " + temp.getFile() + " (is the disk full?)";
        return result;
      }
      result.bytesWritten += got;
    }
    out.flush();
  }
  if (stream.isError()) {
    result.error = stream.getErrorMessage();
    return result;
  }
  const int64_t expected = stream.getTotalLength();
  if (expected >= 0 && expected != result.bytesWritten) {
    result.error = "the download ended early";
    return result;
  }
  if (!temp.overwriteTargetFileWithTemporary()) {
    result.error = "could not replace " + targetFile;
    return result;
  }
  result.succeeded = true;
  return result;
}

} // namespace sonore
