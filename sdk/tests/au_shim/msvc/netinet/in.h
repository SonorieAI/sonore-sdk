// SPDX-License-Identifier: Apache-2.0
// netinet/in.h for MSVC ONLY, which has none: see posix_compat.h.
#pragma once
#include <stdint.h>
#include <sys/socket.h>
typedef uint16_t in_port_t;
struct in_addr {
  uint32_t s_addr;
};
struct in6_addr {
  uint8_t s6_addr[16];
};
struct sockaddr_in {
#if defined(__APPLE__)
  uint8_t sin_len;
#endif
  sa_family_t sin_family;
  in_port_t sin_port;
  struct in_addr sin_addr;
  char sin_zero[8];
};
struct sockaddr_in6 {
#if defined(__APPLE__)
  uint8_t sin6_len;
#endif
  sa_family_t sin6_family;
  in_port_t sin6_port;
  uint32_t sin6_flowinfo;
  struct in6_addr sin6_addr;
  uint32_t sin6_scope_id;
};
struct ip_mreq {
  struct in_addr imr_multiaddr;
  struct in_addr imr_interface;
};
struct ipv6_mreq {
  struct in6_addr ipv6mr_multiaddr;
  unsigned int ipv6mr_interface;
};
#define IPPROTO_IP 0
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17
#define IPPROTO_IPV6 41
#if defined(__APPLE__)
#define IP_MULTICAST_LOOP 11
#define IP_ADD_MEMBERSHIP 12
#define IP_DROP_MEMBERSHIP 13
#define IPV6_MULTICAST_LOOP 11
#define IPV6_JOIN_GROUP 12
#define IPV6_LEAVE_GROUP 13
#define IPV6_V6ONLY 27
#else
#define IP_MULTICAST_LOOP 34
#define IP_ADD_MEMBERSHIP 35
#define IP_DROP_MEMBERSHIP 36
#define IPV6_MULTICAST_LOOP 19
#define IPV6_JOIN_GROUP 20
#define IPV6_LEAVE_GROUP 21
#define IPV6_V6ONLY 26
#endif
