// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: which SDK this is, for code that has to know.
//
// Included by audio.h, so every plugin, tool and browser build sees it. The
// numbers are the CMake project's (project(SonoreSDK VERSION ...) in
// sdk/CMakeLists.txt), and sdk_tests fails when the two disagree.
// CHANGELOG.md says what changed between versions; README's "Versions and
// compatibility" says what a version number promises.
//
//   #if SONORE_SDK_VERSION >= 10100   // 1.1.0 or later
//   #endif
#pragma once

#define SONORE_SDK_VERSION_MAJOR 1
#define SONORE_SDK_VERSION_MINOR 0
#define SONORE_SDK_VERSION_PATCH 0
#define SONORE_SDK_VERSION_STRING "1.0.0"
/** One number to compare: major * 10000 + minor * 100 + patch. */
#define SONORE_SDK_VERSION \
  (SONORE_SDK_VERSION_MAJOR * 10000 + SONORE_SDK_VERSION_MINOR * 100 + SONORE_SDK_VERSION_PATCH)

/** Marks a part of the public API that is on its way out: still there, still
 *  working, and warned about at every use, for at least one minor release
 *  before the next major removes it. The message says what to use instead and
 *  in which version it was deprecated:
 *
 *    SONORE_DEPRECATED("1.3: use newName()") inline int oldName();
 *
 *  A build that cannot act on the warnings yet defines
 *  SONORE_NO_DEPRECATION_WARNINGS, and the mark becomes nothing. */
#if defined(SONORE_NO_DEPRECATION_WARNINGS)
#define SONORE_DEPRECATED(message)
#else
#define SONORE_DEPRECATED(message) [[deprecated(message)]]
#endif
