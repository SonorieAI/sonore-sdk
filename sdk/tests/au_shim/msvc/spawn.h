// SPDX-License-Identifier: Apache-2.0
// spawn.h for MSVC ONLY, which has none: see posix_compat.h.
#pragma once
#include <sys/types.h>
typedef struct {
  void* opaque[16];
} posix_spawn_file_actions_t;
typedef struct {
  void* opaque[64];
} posix_spawnattr_t;
#define POSIX_SPAWN_SETSIGDEF 0x04
#define POSIX_SPAWN_SETSIGMASK 0x08
#if defined(__APPLE__)
#define POSIX_SPAWN_CLOEXEC_DEFAULT 0x4000
#endif
extern "C" {
int posix_spawn_file_actions_init(posix_spawn_file_actions_t*);
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t*);
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t*, int, const char*, int, mode_t);
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t*, int, int);
int posix_spawnattr_init(posix_spawnattr_t*);
int posix_spawnattr_destroy(posix_spawnattr_t*);
int posix_spawnattr_setsigmask(posix_spawnattr_t*, const sigset_t*);
int posix_spawnattr_setsigdefault(posix_spawnattr_t*, const sigset_t*);
int posix_spawnattr_setflags(posix_spawnattr_t*, short);
int posix_spawn(pid_t*, const char*, const posix_spawn_file_actions_t*, const posix_spawnattr_t*, char* const[],
                char* const[]);
int posix_spawnp(pid_t*, const char*, const posix_spawn_file_actions_t*, const posix_spawnattr_t*, char* const[],
                 char* const[]);
}
