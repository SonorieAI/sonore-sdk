// SPDX-License-Identifier: Apache-2.0
// poll.h for MSVC ONLY, which has none: see posix_compat.h.
#pragma once
struct pollfd {
  int fd;
  short events;
  short revents;
};
typedef unsigned int nfds_t;
#define POLLIN 0x0001
#define POLLOUT 0x0004
#define POLLERR 0x0008
#define POLLHUP 0x0010
#define POLLNVAL 0x0020
extern "C" int poll(struct pollfd*, nfds_t, int);
