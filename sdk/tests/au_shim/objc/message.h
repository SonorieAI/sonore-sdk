// SPDX-License-Identifier: Apache-2.0
// A shim of <objc/message.h>. See objc.h in this directory.
//
// objc_msgSend is declared here exactly as Apple declares it for C and C++
// today (OBJC_OLD_DISPATCH_PROTOTYPES defaults to 0 outside Swift): as a
// function taking and returning nothing, which callers MUST cast to the right
// shape. That cast is the whole point: on arm64 a variadic call and a regular
// call pass arguments in different registers, so calling the trampoline
// through a wrongly typed pointer corrupts them. The SDK's msg<> template
// exists to make that cast in one place.
//
// It was declared with the OLD variadic prototype, which let an uncast call
// compile here that the real header refuses -- and had clang warn about every
// correct cast (-Wcast-function-type-mismatch), which a cast from Apple's
// void(void) does not draw.
#pragma once

#include <objc/objc.h>

extern "C" {
void objc_msgSend(void /* id self, SEL op, ... */);
// The struct-returning variant. Not used by this SDK today -- every Cocoa call
// it makes returns a pointer, a scalar or void -- but declared because a file
// that started using it and found nothing here would get a confusing error
// about an undeclared identifier rather than a clear one about the shim.
void objc_msgSend_stret(void /* void* stretAddr, id self, SEL op, ... */);
}
