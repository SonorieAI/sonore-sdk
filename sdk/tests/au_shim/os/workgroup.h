// SPDX-License-Identifier: Apache-2.0
// A shim of <os/workgroup.h> (macOS 11): the types audio_workgroup.h checks its
// token storage against, and the calls it finds with dlsym. See ../README.md.
//
// os_workgroup_t is an OS object; in C and C++ it is a pointer to an opaque
// struct, which is all this needs.
#pragma once

#include <stdint.h>

extern "C" {
typedef struct os_workgroup_s* os_workgroup_t;
typedef struct os_workgroup_max_parallel_threads_attr_s* os_workgroup_mpt_attr_t;

#define OS_WORKGROUP_JOIN_TOKEN_SIZE 36
typedef struct os_workgroup_join_token_opaque_s {
  uint32_t sig;
  char opaque[OS_WORKGROUP_JOIN_TOKEN_SIZE];
} os_workgroup_join_token_s;
typedef struct os_workgroup_join_token_opaque_s* os_workgroup_join_token_t;

int os_workgroup_join(os_workgroup_t wg, os_workgroup_join_token_t token_out);
void os_workgroup_leave(os_workgroup_t wg, os_workgroup_join_token_t token);
int os_workgroup_max_parallel_threads(os_workgroup_t wg, os_workgroup_mpt_attr_t attr);
bool os_workgroup_testcancel(os_workgroup_t wg);
}
