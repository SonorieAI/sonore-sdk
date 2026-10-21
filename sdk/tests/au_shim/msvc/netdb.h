// SPDX-License-Identifier: Apache-2.0
// netdb.h for MSVC ONLY, which has none: see posix_compat.h. addrinfo's last
// members are in a different order on Linux and on the BSDs (macOS).
#pragma once
#include <sys/socket.h>
struct addrinfo {
  int ai_flags;
  int ai_family;
  int ai_socktype;
  int ai_protocol;
  socklen_t ai_addrlen;
#if defined(__APPLE__)
  char* ai_canonname;
  struct sockaddr* ai_addr;
#else
  struct sockaddr* ai_addr;
  char* ai_canonname;
#endif
  struct addrinfo* ai_next;
};
#define AI_PASSIVE 0x1
#define AI_NUMERICHOST 0x4
extern "C" {
int getaddrinfo(const char*, const char*, const struct addrinfo*, struct addrinfo**);
void freeaddrinfo(struct addrinfo*);
}
