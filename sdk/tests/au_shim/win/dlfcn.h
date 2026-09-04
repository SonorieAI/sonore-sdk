// SPDX-License-Identifier: Apache-2.0
// A shim of dlfcn.h, for WINDOWS ONLY.
//
// In its own directory, added to the include path only where there is no real
// dlfcn.h. The first version sat beside the CoreAudio shims and shadowed
// Linux's genuine header -- which is precisely the false confidence
// ../README.md warns about: a check that compiles against a fiction on a
// platform that had the truth available.
#pragma once
#define RTLD_LAZY 1
#define RTLD_NOW 2
#define RTLD_LOCAL 0
#define RTLD_GLOBAL 0x100
#define RTLD_DEFAULT ((void*) 0)
void* dlopen(const char*, int);
void* dlsym(void*, const char*);
int dlclose(void*);
const char* dlerror(void);
typedef struct {
  const char* dli_fname;
  void* dli_fbase;
  const char* dli_sname;
  void* dli_saddr;
} Dl_info;
int dladdr(const void*, Dl_info*);
