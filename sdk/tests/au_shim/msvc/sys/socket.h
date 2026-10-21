// SPDX-License-Identifier: Apache-2.0
// sys/socket.h for MSVC ONLY, which has none: see posix_compat.h. The names
// net.h and ipc.h use, with each platform's own values where they differ.
#pragma once
#include <stddef.h>
#include <sys/types.h>
typedef unsigned int socklen_t;
#if defined(__APPLE__)
typedef unsigned char sa_family_t;
struct sockaddr {
  unsigned char sa_len;
  sa_family_t sa_family;
  char sa_data[14];
};
#define AF_INET6 30
#define AF_LINK 18
#define SOL_SOCKET 0xffff
#define SO_REUSEADDR 0x0004
#define SO_REUSEPORT 0x0200
#define SO_BROADCAST 0x0020
#define SO_ERROR 0x1007
#define SO_NOSIGPIPE 0x1022
#define MSG_DONTWAIT 0x80
#else
typedef unsigned short sa_family_t;
struct sockaddr {
  sa_family_t sa_family;
  char sa_data[14];
};
#define AF_INET6 10
#define AF_PACKET 17
#define SOL_SOCKET 1
#define SO_REUSEADDR 2
#define SO_REUSEPORT 15
#define SO_BROADCAST 6
#define SO_ERROR 4
#define MSG_DONTWAIT 0x40
#define MSG_NOSIGNAL 0x4000
#define SOCK_CLOEXEC 02000000
#endif
#define AF_UNSPEC 0
#define AF_UNIX 1
#define AF_INET 2
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SHUT_RDWR 2
#define SOMAXCONN 128
extern "C" {
int socket(int, int, int);
int bind(int, const struct sockaddr*, socklen_t);
int listen(int, int);
int accept(int, struct sockaddr*, socklen_t*);
#if defined(__linux__)
int accept4(int, struct sockaddr*, socklen_t*, int);
#endif
int connect(int, const struct sockaddr*, socklen_t);
ssize_t send(int, const void*, size_t, int);
ssize_t recv(int, void*, size_t, int);
ssize_t sendto(int, const void*, size_t, int, const struct sockaddr*, socklen_t);
ssize_t recvfrom(int, void*, size_t, int, struct sockaddr*, socklen_t*);
int shutdown(int, int);
int setsockopt(int, int, int, const void*, socklen_t);
int getsockopt(int, int, int, void*, socklen_t*);
int getsockname(int, struct sockaddr*, socklen_t*);
}
