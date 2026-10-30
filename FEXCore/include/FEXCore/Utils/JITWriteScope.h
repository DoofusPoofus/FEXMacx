// SPDX-License-Identifier: MIT
#pragma once

#ifdef __APPLE__
#include <pthread.h>
#endif

namespace FEXCore::Utils {
// macOS keeps MAP_JIT memory either writable or executable per thread.
// Hold one of these while writing code. Nested scopes are fine and only the outermost one flips the state back.
class ScopedJITWrite final {
public:
  ScopedJITWrite() {
#ifdef __APPLE__
    if (WriteDepth++ == 0) {
      pthread_jit_write_protect_np(0);
    }
#endif
  }

  ~ScopedJITWrite() {
#ifdef __APPLE__
    if (--WriteDepth == 0) {
      pthread_jit_write_protect_np(1);
    }
#endif
  }

  ScopedJITWrite(const ScopedJITWrite&) = delete;
  ScopedJITWrite& operator=(const ScopedJITWrite&) = delete;

private:
#ifdef __APPLE__
  static inline thread_local int WriteDepth = 0;
#endif
};
} // namespace FEXCore::Utils
