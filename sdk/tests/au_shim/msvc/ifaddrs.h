// SPDX-License-Identifier: Apache-2.0
// ifaddrs.h for MSVC ONLY, which has none: see posix_compat.h. The BSD
// layout; Linux keeps the last address in a union reached by the same macro.
#pragma once
#include <sys/socket.h>
struct ifaddrs {
  struct ifaddrs* ifa_next;
  char* ifa_name;
  unsigned int ifa_flags;
  struct sockaddr* ifa_addr;
  struct sockaddr* ifa_netmask;
  struct sockaddr* ifa_dstaddr;
  void* ifa_data;
};
#define ifa_broadaddr ifa_dstaddr
extern "C" {
int getifaddrs(struct ifaddrs**);
void freeifaddrs(struct ifaddrs*);
}
