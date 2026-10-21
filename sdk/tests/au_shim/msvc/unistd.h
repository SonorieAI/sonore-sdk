// SPDX-License-Identifier: Apache-2.0
// unistd.h for MSVC ONLY, which has none: see posix_compat.h beside it.
#pragma once
// unlink is MSVC's own, declared by <stdio.h> with its DLL linkage: a second
// declaration here without it is C4273 under /MD.
#include <stdio.h>
typedef unsigned int uid_t;
extern "C" {
int close(int);
long read(int, void*, unsigned long);
long write(int, const void*, unsigned long);
int fsync(int);
uid_t getuid(void);
}
// The system layer's (process.h, ipc.h, file_types.h).
extern "C" {
int pipe(int[2]);
#if defined(__linux__)
int pipe2(int[2], int);
#endif
long readlink(const char*, char*, unsigned long);
int dup2(int, int);
long sysconf(int);
int ftruncate(int, long);
}
#define _SC_PAGESIZE 29
