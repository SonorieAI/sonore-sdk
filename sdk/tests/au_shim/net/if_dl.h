// SPDX-License-Identifier: Apache-2.0
// A shim of <net/if_dl.h>: the link-level address getifaddrs reports for an
// adapter (its MAC), as net.h reads it. See ../objc/objc.h for what a shim
// here proves and what it does not.
#pragma once
#if !defined(SONORE_APPLE_SYNTAX_CHECK)
#error "au_shim is for SONORE_APPLE_SYNTAX_CHECK only -- never on a real include path"
#endif
struct sockaddr_dl {
  unsigned char sdl_len;
  unsigned char sdl_family;
  unsigned short sdl_index;
  unsigned char sdl_type;
  unsigned char sdl_nlen;
  unsigned char sdl_alen;
  unsigned char sdl_slen;
  char sdl_data[12];
};
#define LLADDR(s) ((char*) ((s)->sdl_data + (s)->sdl_nlen))
