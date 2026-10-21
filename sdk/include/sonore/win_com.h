// SPDX-License-Identifier: Apache-2.0
// Sonore SDK: COM on a thread somebody else owns.
//
// Media Foundation, the endpoint volume and the disc burner are all COM, and
// every one of them is called from a thread this SDK did not create: a host's
// UI thread, a sampler's loader, an application's worker. COM's apartment is
// a property of the THREAD, chosen by whoever initialises it first, and two
// mistakes are easy to make here:
//
//   * initialising a thread the host already initialised, in the other model.
//     CoInitializeEx answers RPC_E_CHANGED_MODE and changes nothing -- which
//     is correct, and COM is perfectly usable in the apartment that exists.
//     Treating that answer as failure refuses to work on every DAW's UI
//     thread, which is single-threaded by necessity;
//   * balancing a call that did not succeed. CoUninitialize after
//     RPC_E_CHANGED_MODE tears down the HOST's apartment, and the host finds
//     out at some later, unrelated call.
//
// So Apartment initialises only if needed, balances only what it initialised,
// and says whether COM can be used either way. S_FALSE ("already initialised,
// same model") is a success that must be balanced: it took a reference.
//
// The multithreaded model when the choice is ours, because nothing here pumps
// messages and none of these objects needs a window. The scope is always the
// one call: the apartment is gone when the call returns, so the next thing to
// initialise this thread chooses freely again.
#pragma once

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>

// CoInitializeEx and CoCreateInstance live in ole32. MSVC takes the hint from
// here; any other toolchain links ole32 itself, as it already does for the
// WASAPI backend.
#if defined(_MSC_VER)
#pragma comment(lib, "ole32.lib")
#endif

namespace sonore {
namespace comdetail {

class Apartment {
public:
  Apartment() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    owned_ = (hr == S_OK || hr == S_FALSE);
    usable_ = owned_ || hr == RPC_E_CHANGED_MODE;
  }
  ~Apartment() {
    if (owned_) CoUninitialize();
  }
  Apartment(const Apartment&) = delete;
  Apartment& operator=(const Apartment&) = delete;

  /** COM can be used on this thread for as long as this object lives. */
  bool usable() const { return usable_; }
  /** This object initialised the thread (and will uninitialise it). */
  bool owned() const { return owned_; }

private:
  bool owned_ = false;
  bool usable_ = false;
};

/** A COM pointer that releases itself. Deliberately tiny: no AddRef on copy,
 *  because it does not copy -- every interface here has exactly one owner and
 *  a scope, and a ref-counted smart pointer would hide the one bug worth
 *  seeing, a Release forgotten on an early return. */
template <class T>
class Ref {
public:
  Ref() = default;
  ~Ref() { reset(); }
  Ref(const Ref&) = delete;
  Ref& operator=(const Ref&) = delete;
  Ref(Ref&& other) noexcept : p_(other.p_) { other.p_ = nullptr; }
  Ref& operator=(Ref&& other) noexcept {
    if (this != &other) {
      reset();
      p_ = other.p_;
      other.p_ = nullptr;
    }
    return *this;
  }

  void reset() {
    if (p_) p_->Release();
    p_ = nullptr;
  }
  /** For an out-parameter: releases what was held first, so a pointer reused
   *  in a loop does not leak the previous iteration's object. */
  T** put() {
    reset();
    return &p_;
  }
  void** putVoid() { return reinterpret_cast<void**>(put()); }
  T* get() const { return p_; }
  T* operator->() const { return p_; }
  explicit operator bool() const { return p_ != nullptr; }

private:
  T* p_ = nullptr;
};

} // namespace comdetail
} // namespace sonore

#endif // _WIN32
