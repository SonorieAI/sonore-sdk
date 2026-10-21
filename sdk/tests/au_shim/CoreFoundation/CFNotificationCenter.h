// SPDX-License-Identifier: Apache-2.0
// A shim of <CoreFoundation/CFNotificationCenter.h>: the distributed centre
// message_manager.h broadcasts through. See ../README.md: this proves
// internal consistency, not ABI correctness.
#pragma once

#include <CoreFoundation/CoreFoundation.h>

typedef struct __CFNotificationCenter* CFNotificationCenterRef;
typedef CFStringRef CFNotificationName;
typedef void (*CFNotificationCallback)(CFNotificationCenterRef center, void* observer,
                                       CFNotificationName name, const void* object,
                                       CFDictionaryRef userInfo);

// CF_ENUM(CFIndex, ...) on Apple: an enum type with CFIndex underneath.
enum CFNotificationSuspensionBehavior : CFIndex {
  CFNotificationSuspensionBehaviorDrop = 1,
  CFNotificationSuspensionBehaviorCoalesce = 2,
  CFNotificationSuspensionBehaviorHold = 3,
  CFNotificationSuspensionBehaviorDeliverImmediately = 4,
};

CFNotificationCenterRef CFNotificationCenterGetDistributedCenter(void);
void CFNotificationCenterAddObserver(CFNotificationCenterRef center, const void* observer,
                                     CFNotificationCallback callBack, CFStringRef name,
                                     const void* object,
                                     CFNotificationSuspensionBehavior suspensionBehavior);
void CFNotificationCenterRemoveEveryObserver(CFNotificationCenterRef center, const void* observer);
void CFNotificationCenterPostNotification(CFNotificationCenterRef center, CFNotificationName name,
                                          const void* object, CFDictionaryRef userInfo,
                                          Boolean deliverImmediately);
