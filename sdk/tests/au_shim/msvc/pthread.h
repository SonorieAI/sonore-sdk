// SPDX-License-Identifier: Apache-2.0
// pthread.h for MSVC ONLY, which has none: see posix_compat.h. The calls the
// macOS code paths make, three of them Apple's own extensions:
// javascript.h asks a Mac thread for its stack bounds (pthread_get_stackaddr_np,
// pthread_get_stacksize_np) so a script's stack budget never exceeds what the
// thread has; audio_workgroup.h makes a thread real-time through its Mach port
// (pthread_mach_thread_np) and asks whether it is the main one
// (pthread_main_np). Declarations to compile against, as the rest of au_shim
// is -- not the ABI.
#pragma once
#include <stddef.h>

#include <mach/mach.h>

extern "C" {
typedef struct _opaque_pthread_t* pthread_t;
pthread_t pthread_self(void);
void* pthread_get_stackaddr_np(pthread_t);
size_t pthread_get_stacksize_np(pthread_t);
mach_port_t pthread_mach_thread_np(pthread_t thread);
int pthread_main_np(void);
}
