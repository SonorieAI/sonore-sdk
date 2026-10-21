// SPDX-License-Identifier: Apache-2.0
// The POSIX names the macOS code paths use, for MSVC ONLY -- force-included
// into sonore_mac_syntax, which compiles those paths on Windows with
// __APPLE__ defined and _WIN32 undefined.
//
// In its own directory for the reason win/dlfcn.h is in one: on a machine
// with a real unistd.h (MinGW, Linux) nothing here may shadow it. MSVC has
// none, and declares a few of these itself under their POSIX names.
#pragma once
// MSVC has no __attribute__; clang-cl does, and its own intrinsic headers
// (mmintrin.h, pulled in by <intrin.h>) are built on vector_size attributes,
// so defining it away there turned every vector type into a scalar and the
// Windows-clangcl build of these targets failed in clang's headers.
#if !defined(__clang__)
#define __attribute__(x)
#endif
#include <process.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#define S_ISDIR(m) (((m) & 0170000) == 0040000)
#define S_ISREG(m) (((m) & 0170000) == 0100000)
#include <fcntl.h>
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
extern "C" int open(const char*, int, ...);
typedef int pid_t;
typedef unsigned int useconds_t;
extern "C" int mkdir(const char*, unsigned);
extern "C" int kill(int, int);
extern "C" int nanosleep(const struct timespec*, struct timespec*);
extern "C" int usleep(useconds_t);

// What the system layer adds (process.h, ipc.h, net.h, file_types.h): the
// signal set, fcntl, 64-bit stdio seeks and realpath -- with each platform's
// values where they differ, since this file serves the Linux check as well.
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
typedef intptr_t ssize_t;
typedef unsigned int mode_t;
typedef struct {
  unsigned long bits[2];
} sigset_t;
extern "C" int sigemptyset(sigset_t*);
extern "C" int sigaddset(sigset_t*, int);
#define SIGHUP 1
#define SIGQUIT 3
#define SIGKILL 9
#define SIGPIPE 13
#define SIGALRM 14
#if defined(__APPLE__)
#define SIGCHLD 20
#define SIGUSR1 30
#define SIGUSR2 31
#define O_NONBLOCK 0x0004
#else
#define SIGCHLD 17
#define SIGUSR1 10
#define SIGUSR2 12
#define O_NONBLOCK 04000
#endif
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define FD_CLOEXEC 1
extern "C" int fcntl(int, int, ...);
extern "C" int fseeko(FILE*, off_t, int);
extern "C" off_t ftello(FILE*);
extern "C" char* realpath(const char*, char*);
extern "C" int chmod(const char*, int);
// <stdlib.h> on macOS and the BSDs; crypto.h's random bytes.
#include <stddef.h>
extern "C" void arc4random_buf(void*, size_t);
