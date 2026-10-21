// SPDX-License-Identifier: Apache-2.0
// dirent.h for MSVC ONLY, which has none: see posix_compat.h beside it.
// What host.h and plugin_cache.h use to walk a plugin folder on macOS and
// Linux -- the same three calls on both.
#pragma once
extern "C" {
struct dirent {
  unsigned long d_ino;
  unsigned char d_type;
  char d_name[256];
};
typedef struct __dirstream DIR;
DIR* opendir(const char*);
struct dirent* readdir(DIR*);
int closedir(DIR*);
}
