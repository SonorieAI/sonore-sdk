// SPDX-License-Identifier: Apache-2.0
// sys/wait.h for MSVC ONLY, which has none: see posix_compat.h.
#pragma once
#include <sys/types.h>
#ifndef SIGKILL
#define SIGKILL 9 // MSVC's signal.h has no SIGKILL; kill() is in posix_compat.h
#endif
#define WNOHANG 1
#define WIFEXITED(s) (((s) & 0x7f) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define WIFSIGNALED(s) (((s) & 0x7f) != 0 && ((s) & 0x7f) != 0x7f)
#define WTERMSIG(s) ((s) & 0x7f)
extern "C" pid_t waitpid(pid_t, int*, int);
