// SPDX-License-Identifier: Apache-2.0
// A shim of <objc/runtime.h>. See objc.h in this directory.
//
// Only what the SDK's Cocoa files actually call. A shim that grows beyond its
// callers is a second implementation nobody asked for -- the same rule the AU
// shim's README sets out.
#pragma once

#include <objc/objc.h>
#include <stddef.h>
#include <stdint.h>

typedef struct objc_ivar* Ivar;
typedef struct objc_method* Method;
typedef struct objc_protocol* Protocol;

extern "C" {
Class objc_getClass(const char* name);
Class objc_allocateClassPair(Class superclass, const char* name, size_t extraBytes);
void objc_registerClassPair(Class cls);
void objc_disposeClassPair(Class cls);

SEL sel_registerName(const char* str);
const char* sel_getName(SEL sel);

BOOL class_addMethod(Class cls, SEL name, IMP imp, const char* types);
BOOL class_addIvar(Class cls, const char* name, size_t size, uint8_t alignment,
                   const char* types);
BOOL class_addProtocol(Class cls, Protocol* protocol);
const char* class_getName(Class cls);

Ivar object_getInstanceVariable(id obj, const char* name, void** outValue);
Ivar object_setInstanceVariable(id obj, const char* name, void* value);
Class object_getClass(id obj);
}
