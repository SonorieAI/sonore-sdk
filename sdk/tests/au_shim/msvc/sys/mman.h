// SPDX-License-Identifier: Apache-2.0
// sys/mman.h for MSVC ONLY, which has none: see posix_compat.h.
#pragma once
#include <stddef.h>
#define PROT_READ 1
#define MAP_PRIVATE 2
#define MAP_FAILED ((void*) -1)
extern "C" {
void* mmap(void*, size_t, int, int, int, long);
int munmap(void*, size_t);
}
// file_types.h's MemoryMappedFile maps for writing, and shares.
#define PROT_WRITE 2
#define MAP_SHARED 1
