// SPDX-License-Identifier: Apache-2.0
// A shim of <objc/message.h>. See objc.h in this directory.
//
// objc_msgSend is declared here exactly as Apple declares it -- as a function
// with an unprototyped C signature that callers cast to the right shape. That
// cast is the whole point: on arm64 a variadic call and a regular call pass
// arguments in different registers, so calling the trampoline through a wrongly
// typed pointer corrupts them. The SDK's msg<> template exists to make that
// cast in one place, and this declaration is what lets it compile.
#pragma once

#include <objc/objc.h>

extern "C" {
id objc_msgSend(id self, SEL op, ...);
// The struct-returning variant. Not used by this SDK today -- every Cocoa call
// it makes returns a pointer, a scalar or void -- but declared because a file
// that started using it and found nothing here would get a confusing error
// about an undeclared identifier rather than a clear one about the shim.
void objc_msgSend_stret(void* stretAddr, id self, SEL op, ...);
}
