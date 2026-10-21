// SPDX-License-Identifier: Apache-2.0
// sys/file.h for MSVC ONLY, which has none: see posix_compat.h.
#pragma once
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8
extern "C" int flock(int, int);
