// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: an Apple block, built by hand, for the APIs that take one.
//
// ── Why this exists ─────────────────────────────────────────────────────────
//
// A few of Apple's audio APIs take or hand out a BLOCK -- clang's closure
// extension, written `^(args) { ... }` -- where everything else in them takes
// a function pointer and a context:
//
//   AudioComponentInstantiate                 its completion handler (host_au.h)
//   kAudioUnitProperty_RequestViewController  an AUv3's editor (host_au.h)
//   kAudioUnitProperty_RenderContextObserver  the render thread's workgroup
//                                             (au_wrapper.h)
//
// This SDK is plain C++ with no .mm file anywhere, and the `^` syntax needs
// -fblocks. Apple clang turns that on by default, but the only compiler that
// reads the Mac code in this loop -- MSVC, against tests/au_shim -- cannot
// parse it, and code no compiler reads is how au_wrapper.h spent weeks
// unbuildable. So a block is built here the way the compiler builds one, from
// the published ABI, exactly as the Cocoa files send messages through
// objc_msgSend rather than in Objective-C.
//
// ── The ABI ─────────────────────────────────────────────────────────────────
//
// clang's "Block Implementation Specification" (Block-ABI-Apple), unchanged
// since Mac OS X 10.6. Every block begins
//
//   void* isa;                 the block's class
//   int   flags;
//   int   reserved;
//   R   (*invoke)(block*, args...);  called with the block itself first
//   descriptor*;               { reserved, size } at minimum
//
// and its captured variables follow. Calling a block is loading `invoke` and
// calling it with the block's own address as the first argument -- which is
// how the captures are reached.
//
// ── Why every block here is GLOBAL, captures and all ────────────────────────
//
// A block handed to an API is copied, retained or released under rules that
// are documented unevenly: AudioUnitGetProperty's "Copy rule", ARC's
// objc_retainBlock, a plain Block_copy. A block whose flags carry
// BLOCK_IS_GLOBAL is immune to every one of them: _Block_copy returns the same
// pointer, _Block_release does nothing, and -[__NSGlobalBlock__ retain],
// -release and -copy are all identities. So nothing anybody does to it can
// free it, or copy it somewhere its captures are not.
//
// The price is that the block lives where WE put it -- inside an object of
// ours -- and that object has to outlive every call the API can make. Each
// caller says how it guarantees that; host_au.h keeps a request alive until
// its completion has run, and au_wrapper.h keeps the observer inside the
// instance the OS stops rendering before it disposes.
//
// ── What it cannot do ───────────────────────────────────────────────────────
//
// arm64e. Pointer authentication signs a block's invoke pointer with the
// block's address, and a hand-built block has no signature. Third-party code
// on macOS is arm64 or x86_64 -- arm64e is reserved for Apple's own binaries --
// so this is refused at compile time rather than crashing at the first call.
//
// ── Compiled, never run ─────────────────────────────────────────────────────
//
// There is no Mac in this loop. tests/mac_host_selftest.cpp is the program a
// Mac runs to check this file: it hands one of these to _Block_copy and calls
// the result.
#pragma once

// Also read by the one-file syntax checks (SONORE_APPLE_SYNTAX_CHECK), which
// compile au_wrapper.h without __APPLE__: see tests/au_shim/README.md.
#if defined(__APPLE__) || defined(SONORE_APPLE_SYNTAX_CHECK)

#include <dlfcn.h>

#include <cstdint>

#if defined(__has_feature)
#if __has_feature(ptrauth_calls)
#error "apple_block.h builds blocks by hand, and arm64e signs a block's invoke pointer: build arm64 or x86_64"
#endif
#endif

namespace sonore {
namespace detail {

/** Block_descriptor_1 in libclosure: the two fields every block has. No
 *  copy/dispose helpers and no signature, because the flags below say there
 *  are none -- a global block is never copied, so it needs neither. */
struct BlockDescriptor {
  unsigned long reserved;
  unsigned long size;
};

/** BLOCK_IS_GLOBAL, libclosure's Block_private.h. */
constexpr int kBlockIsGlobal = 1 << 28;

/** A block whose captured state is one value, `Capture` -- in practice a
 *  pointer to the object that owns the block. `Invoke` is the function the
 *  block runs, whose first argument is the block itself:
 *  `void (*)(void* block, Args...)`. Reach the capture from there with
 *  blockCapture(). */
template <typename Capture, typename Invoke>
struct BlockLiteral {
  void* isa = nullptr;
  int flags = 0;
  int reserved = 0;
  Invoke invoke = nullptr;
  const BlockDescriptor* descriptor = nullptr;
  Capture capture{};
};

/** _NSConcreteGlobalBlock: the class every global block points at. Found with
 *  dlsym rather than declared, so nothing here depends on <Block.h> or on the
 *  compiler having blocks turned on; libSystem exports it in every process. */
inline void* blockGlobalClass() {
  static void* const isa = dlsym(RTLD_DEFAULT, "_NSConcreteGlobalBlock");
  return isa;
}

/** Fill `block` in place. False -- and the block unusable -- only if the
 *  runtime's global block class could not be found, which on macOS would mean
 *  a process without libSystem. */
template <typename Capture, typename Invoke>
inline bool makeGlobalBlock(BlockLiteral<Capture, Invoke>& block, Invoke invoke,
                            const Capture& capture) {
  static const BlockDescriptor descriptor = {0, sizeof(BlockLiteral<Capture, Invoke>)};
  void* isa = blockGlobalClass();
  if (!isa || !invoke) return false;
  block.isa = isa;
  block.flags = kBlockIsGlobal;
  block.reserved = 0;
  block.invoke = invoke;
  block.descriptor = &descriptor;
  block.capture = capture;
  return true;
}

/** The capture, from the `void* block` an invoke function is handed. */
template <typename Capture, typename Invoke>
inline Capture blockCapture(void* block) {
  return static_cast<BlockLiteral<Capture, Invoke>*>(block)->capture;
}

} // namespace detail
} // namespace sonore

#endif // __APPLE__ || SONORE_APPLE_SYNTAX_CHECK
