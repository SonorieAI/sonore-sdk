// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: which Apple platform this is -- the Mac, or one of the UIKit ones.
//
// `__APPLE__` says Apple and nothing more: it is defined for macOS, iOS,
// iPadOS, tvOS and visionOS alike. The files that grew up on the Mac test it
// alone and then reach for AppKit, the HAL and NSWindow, none of which exist
// on an iPhone. So the question is asked here, once, through Apple's own
// TargetConditionals.h, and answered as one macro:
//
//   SONORE_IOS   defined on the UIKit platforms (iOS, iPadOS, the iOS
//                simulator, Mac Catalyst is NOT one of them: it has
//                TARGET_OS_IPHONE but takes AppKit's place through UIKit
//                and is not a target this SDK builds).
//   SONORE_MACOS defined on macOS.
//
// The syntax checks (SONORE_APPLE_SYNTAX_CHECK, tests/au_shim) compile the
// Apple files on Windows, where TargetConditionals.h does not exist: they
// read the Mac halves unless SONORE_IOS_SYNTAX_CHECK asks for the iOS ones.
#pragma once

#if defined(__APPLE__) && !defined(SONORE_APPLE_SYNTAX_CHECK)
#include <TargetConditionals.h>
#endif

#if !defined(SONORE_IOS) && !defined(SONORE_MACOS)
#if defined(SONORE_APPLE_SYNTAX_CHECK)
#if defined(SONORE_IOS_SYNTAX_CHECK)
#define SONORE_IOS 1
#else
#define SONORE_MACOS 1
#endif
#elif defined(__APPLE__)
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE && !(defined(TARGET_OS_MACCATALYST) && TARGET_OS_MACCATALYST)
#define SONORE_IOS 1
#else
#define SONORE_MACOS 1
#endif
#endif
#endif
