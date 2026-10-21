// SPDX-License-Identifier: Apache-2.0
// A shim of <mach-o/dyld.h>: the one call process.h makes. See
// ../objc/objc.h for what a shim here proves and what it does not.
#pragma once
#if !defined(SONORE_APPLE_SYNTAX_CHECK)
#error "au_shim is for SONORE_APPLE_SYNTAX_CHECK only -- never on a real include path"
#endif
#include <stdint.h>
extern "C" int _NSGetExecutablePath(char* buf, uint32_t* bufsize);
