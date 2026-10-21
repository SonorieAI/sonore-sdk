// SPDX-License-Identifier: Apache-2.0
// sys/un.h for MSVC ONLY, which has none: see posix_compat.h.
#pragma once
#include <sys/socket.h>
struct sockaddr_un {
#if defined(__APPLE__)
  unsigned char sun_len;
  sa_family_t sun_family;
  char sun_path[104];
#else
  sa_family_t sun_family;
  char sun_path[108];
#endif
};
