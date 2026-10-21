// SPDX-License-Identifier: Apache-2.0
// A shim of <crt_externs.h>: the environment, as code in a bundle must reach
// it (process.h). See objc/objc.h for what a shim here proves and does not.
#pragma once
#if !defined(SONORE_APPLE_SYNTAX_CHECK)
#error "au_shim is for SONORE_APPLE_SYNTAX_CHECK only -- never on a real include path"
#endif
extern "C" char*** _NSGetEnviron(void);
