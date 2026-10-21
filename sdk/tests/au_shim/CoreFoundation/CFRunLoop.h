// SPDX-License-Identifier: Apache-2.0
// A shim of <CoreFoundation/CFRunLoop.h>: the run loop and the sources that
// wake it, as message_manager.h uses them. See ../README.md: this proves
// internal consistency, not ABI correctness.
//
// Its own file, as on Apple, rather than more lines in CoreFoundation.h: the
// header that needs it includes it by this name, which is the name the real
// SDK gives it.
#pragma once

#include <CoreFoundation/CoreFoundation.h>

typedef double CFTimeInterval;
typedef struct __CFRunLoop* CFRunLoopRef;
typedef struct __CFRunLoopSource* CFRunLoopSourceRef;
typedef CFStringRef CFRunLoopMode;
typedef SInt32 CFRunLoopRunResult;

extern const CFRunLoopMode kCFRunLoopDefaultMode;
extern const CFRunLoopMode kCFRunLoopCommonModes;

/** Version 0: a source signalled by hand, whose `perform` runs on the loop. */
struct CFRunLoopSourceContext {
  CFIndex version;
  void* info;
  const void* (*retain)(const void* info);
  void (*release)(const void* info);
  CFStringRef (*copyDescription)(const void* info);
  Boolean (*equal)(const void* info1, const void* info2);
  CFIndex (*hash)(const void* info);
  void (*schedule)(void* info, CFRunLoopRef rl, CFRunLoopMode mode);
  void (*cancel)(void* info, CFRunLoopRef rl, CFRunLoopMode mode);
  void (*perform)(void* info);
};

CFRunLoopRef CFRunLoopGetCurrent(void);
CFRunLoopRef CFRunLoopGetMain(void);
void CFRunLoopRun(void);
CFRunLoopRunResult CFRunLoopRunInMode(CFRunLoopMode mode, CFTimeInterval seconds,
                                      Boolean returnAfterSourceHandled);
void CFRunLoopStop(CFRunLoopRef rl);
void CFRunLoopWakeUp(CFRunLoopRef rl);

CFRunLoopSourceRef CFRunLoopSourceCreate(CFAllocatorRef allocator, CFIndex order,
                                         CFRunLoopSourceContext* context);
void CFRunLoopAddSource(CFRunLoopRef rl, CFRunLoopSourceRef source, CFRunLoopMode mode);
void CFRunLoopRemoveSource(CFRunLoopRef rl, CFRunLoopSourceRef source, CFRunLoopMode mode);
void CFRunLoopSourceInvalidate(CFRunLoopSourceRef source);
void CFRunLoopSourceSignal(CFRunLoopSourceRef source);
