// SPDX-License-Identifier: Apache-2.0
// A shim of the CoreFoundation declarations au_wrapper.h uses.
// See ../README.md: this proves internal consistency, not ABI correctness.
#pragma once

#include <cstddef>
#include <cstdint>

typedef signed char SInt8;
typedef unsigned char UInt8;
typedef signed short SInt16;
typedef unsigned short UInt16;
typedef signed int SInt32;
typedef unsigned int UInt32;
typedef signed long long SInt64;
typedef unsigned long long UInt64;
typedef float Float32;
typedef double Float64;
typedef unsigned char Boolean;
typedef SInt32 OSStatus;
typedef UInt32 FourCharCode;
typedef FourCharCode OSType;
typedef long CFIndex;
typedef unsigned long CFTypeID;
typedef UInt32 CFStringEncoding;
typedef const void* CFTypeRef;
// A POINTER, as on Apple. The first shim had it as an integer and every
// `nullptr` allocator argument in au_wrapper.h failed to convert -- which is
// the shim being wrong, not the wrapper.
typedef const struct __CFAllocator* CFAllocatorRef;

enum { noErr = 0 };
enum { kCFStringEncodingUTF8 = 0x08000100 };
#define kCFAllocatorDefault ((CFAllocatorRef) nullptr)

typedef struct __CFString* CFStringRef;
typedef struct __CFString* CFMutableStringRef;
typedef struct __CFURL* CFURLRef;
typedef struct __CFBundle* CFBundleRef;
typedef struct __CFDictionary* CFDictionaryRef;
typedef struct __CFArray* CFArrayRef;
typedef struct __CFData* CFDataRef;
typedef struct __CFNumber* CFNumberRef;

CFStringRef CFStringCreateWithCString(CFAllocatorRef, const char*, CFStringEncoding);
Boolean CFStringGetCString(CFStringRef, char*, CFIndex, CFStringEncoding);
CFIndex CFStringGetLength(CFStringRef);
CFIndex CFStringGetMaximumSizeForEncoding(CFIndex, CFStringEncoding);
const char* CFStringGetCStringPtr(CFStringRef, CFStringEncoding);
void CFRelease(CFTypeRef);
CFTypeRef CFRetain(CFTypeRef);

CFBundleRef CFBundleGetBundleWithIdentifier(CFStringRef);
CFURLRef CFBundleCopyResourcesDirectoryURL(CFBundleRef);
CFURLRef CFBundleCopyBundleURL(CFBundleRef);
Boolean CFURLGetFileSystemRepresentation(CFURLRef, Boolean, UInt8*, CFIndex);

// CFSTR is a compiler builtin on Apple platforms. Here it only has to produce
// something of the right TYPE -- nothing in a syntax check dereferences it.
#define CFSTR(x) ((CFStringRef) (const void*) (x))

// ── Property lists, the shape a saved AU preset takes ───────────────────────
typedef CFTypeRef CFPropertyListRef;
typedef struct __CFDictionary* CFMutableDictionaryRef;
typedef struct __CFArray* CFMutableArrayRef;

struct CFRange {
  CFIndex location;
  CFIndex length;
};
CFRange CFRangeMake(CFIndex, CFIndex);

typedef UInt32 CFNumberType;
enum {
  kCFNumberSInt32Type = 3,
  kCFNumberSInt64Type = 4,
  kCFNumberFloat32Type = 5,
  kCFNumberFloat64Type = 6,
};

CFTypeID CFGetTypeID(CFTypeRef);
CFTypeID CFDictionaryGetTypeID(void);
CFTypeID CFDataGetTypeID(void);
CFTypeID CFNumberGetTypeID(void);
CFTypeID CFStringGetTypeID(void);
CFTypeID CFArrayGetTypeID(void);

const void* CFDictionaryGetValue(CFDictionaryRef, const void*);
void CFDictionarySetValue(CFMutableDictionaryRef, const void*, const void*);
CFMutableDictionaryRef CFDictionaryCreateMutable(CFAllocatorRef, CFIndex, const void*,
                                                 const void*);
extern const void* kCFTypeDictionaryKeyCallBacks;
extern const void* kCFTypeDictionaryValueCallBacks;

CFIndex CFDataGetLength(CFDataRef);
void CFDataGetBytes(CFDataRef, CFRange, UInt8*);
const UInt8* CFDataGetBytePtr(CFDataRef);
CFDataRef CFDataCreate(CFAllocatorRef, const UInt8*, CFIndex);

CFNumberRef CFNumberCreate(CFAllocatorRef, CFNumberType, const void*);
Boolean CFNumberGetValue(CFNumberRef, CFNumberType, void*);

CFIndex CFArrayGetCount(CFArrayRef);
const void* CFArrayGetValueAtIndex(CFArrayRef, CFIndex);
CFMutableArrayRef CFArrayCreateMutable(CFAllocatorRef, CFIndex, const void*);
void CFArrayAppendValue(CFMutableArrayRef, const void*);
extern const void* kCFTypeArrayCallBacks;

CFArrayRef CFArrayCreate(CFAllocatorRef, const void**, CFIndex, const void*);
CFURLRef CFURLCreateFromFileSystemRepresentation(CFAllocatorRef, const UInt8*, CFIndex, Boolean);
CFURLRef CFURLCreateCopyDeletingLastPathComponent(CFAllocatorRef, CFURLRef);
