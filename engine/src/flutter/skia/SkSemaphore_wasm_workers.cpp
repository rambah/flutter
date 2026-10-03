// Copyright 2026 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "include/private/SkSemaphore.h"

#include <emscripten/wasm_worker.h>

#if !defined(__EMSCRIPTEN_WASM_WORKERS__)
#error "This SkSemaphore implementation requires Emscripten Wasm Workers."
#endif

// SkSemaphore's inline atomic fast path is unchanged. Only contended waits
// reach this implementation. POSIX sem_wait/sem_post cannot be used here:
// -sWASM_WORKERS without -pthread links their single-threaded no-op stubs.
struct SkSemaphore::OSSemaphore {
  emscripten_semaphore_t semaphore =
      EMSCRIPTEN_SEMAPHORE_T_STATIC_INITIALIZER(0);

  void signal(int count) {
    emscripten_semaphore_release(&semaphore, count);
  }

  void wait() {
    if (emscripten_current_thread_is_wasm_worker()) {
      emscripten_semaphore_waitinf_acquire(&semaphore, 1);
    } else {
      // A browser's main thread cannot use memory.atomic.wait. SkSemaphore's
      // synchronous contract requires spinning here, as it does for Emscripten
      // pthread mutexes. Lock holders must never depend on the main event loop
      // to release the lock. Keep critical sections short; contention can still
      // delay input and must be measured before enabling threaded rendering.
      while (emscripten_semaphore_try_acquire(&semaphore, 1) < 0) {
      }
    }
  }
};

SkSemaphore::~SkSemaphore() {
  delete fOSSemaphore;
}

void SkSemaphore::osSignal(int count) {
  fOSSemaphoreOnce([this] { fOSSemaphore = new OSSemaphore; });
  fOSSemaphore->signal(count);
}

void SkSemaphore::osWait() {
  fOSSemaphoreOnce([this] { fOSSemaphore = new OSSemaphore; });
  fOSSemaphore->wait();
}

bool SkSemaphore::try_wait() {
  int count = fCount.load(std::memory_order_relaxed);
  if (count > 0) {
    return fCount.compare_exchange_weak(count, count - 1,
                                       std::memory_order_acquire);
  }
  return false;
}
