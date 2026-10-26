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
// As Apple's runtime.h has it outside Objective-C: an object type, handed
// around as Protocol*. This said `struct objc_protocol*`, which made
// class_addProtocol take a pointer to a pointer -- unnoticed until
// window_cocoa.h first called it (adopting NSTextInputClient).
typedef struct objc_object Protocol;

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
Protocol* objc_getProtocol(const char* name);
const char* class_getName(Class cls);

Class class_getSuperclass(Class cls);
BOOL class_respondsToSelector(Class cls, SEL sel);
// An override is registered with the SAME type encoding as the method it
// replaces, read from the superclass: key-value observing builds its setter
// interception from it (auv3_wrapper.h's addOverride).
Method class_getInstanceMethod(Class cls, SEL name);
const char* method_getTypeEncoding(Method m);
// A protocol the frameworks declare but may not have registered with the
// runtime yet: built and registered here (AUAudioUnitFactory).
Protocol* objc_allocateProtocol(const char* name);
void objc_registerProtocol(Protocol* proto);
void protocol_addMethodDescription(Protocol* proto, SEL name, const char* types,
                                   BOOL isRequiredMethod, BOOL isInstanceMethod);

// Weak references without ARC: a slot the runtime clears when its object
// starts deallocating, read back as nil from then on. objc_loadWeak returns
// the object retained and autoreleased.
id objc_storeWeak(id* location, id obj);
id objc_loadWeak(id* location);

Ivar object_getInstanceVariable(id obj, const char* name, void** outValue);
Ivar object_setInstanceVariable(id obj, const char* name, void* value);
Class object_getClass(id obj);
}
