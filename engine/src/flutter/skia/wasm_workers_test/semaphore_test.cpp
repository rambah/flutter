// Copyright 2026 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "include/private/SkMutex.h"
#include "include/private/SkSemaphore.h"

#include <emscripten.h>
#include <emscripten/wasm_worker.h>

#include <atomic>
#include <cstdio>

namespace {
constexpr int kWorkers = 3;
constexpr int kIterations = 50000;
emscripten_wasm_worker_t workers[kWorkers];
SkSemaphore gate;
SkSemaphore done;
SkSemaphore main_gate;
SkMutex mutex;
std::atomic<int> started{0};
std::atomic<int> entered{0};
std::atomic<int> active{0};
std::atomic<int> overlaps{0};
std::atomic<int> bad_payload{0};
std::atomic<bool> main_wait_ready{false};
int payload = 0;
int counter = 0;
int phase = 0;
int completed = 0;
double phase_start = 0;

void finish(bool passed, const char* reason) {
  std::printf("%s: %s\n", passed ? "PASS" : "FAIL", reason);
  EM_ASM({
    globalThis.semaphoreTestResult = ({
      passed: !!$0,
      reason: UTF8ToString($1),
      checks: $2,
      counter: $3,
      overlaps: $4
    });
  }, passed, reason, phase, counter, overlaps.load());
  emscripten_cancel_main_loop();
}

void waiter() {
  started.fetch_add(1);
  gate.wait();
  if (payload != 42) {
    bad_payload.fetch_add(1);
  }
  entered.fetch_add(1);
  done.signal();
}

void contend() {
  for (int i = 0; i < kIterations; ++i) {
    mutex.acquire();
    if (active.fetch_add(1) != 0) {
      overlaps.fetch_add(1);
    }
    ++counter;
    active.fetch_sub(1);
    mutex.release();
  }
}

void worker_contend() {
  contend();
  done.signal();
}

void release_main() {
  main_wait_ready.store(true);
  // Wake the main thread directly from a running worker, without depending on
  // the main thread's event loop. Exercise its non-blocking-wait restriction.
  emscripten_wasm_worker_sleep(20000000);
  main_gate.signal();
}

void reset_waiters() {
  started.store(0);
  entered.store(0);
  completed = 0;
  phase_start = emscripten_get_now();
}

bool all_done(int count) {
  while (done.try_wait()) {
    ++completed;
  }
  return completed == count;
}

void tick() {
  if (emscripten_get_now() - phase_start > 15000) {
    finish(false, "watchdog: a waiter never completed");
    return;
  }
  switch (phase) {
    case 0:
      if (started.load() != 1 || emscripten_get_now() - phase_start < 100) {
        return;
      }
      if (entered.load() != 0) {
        finish(false, "wait returned without a signal (POSIX stub)");
        return;
      }
      payload = 42;
      gate.signal();
      ++phase;
      break;
    case 1:
      if (!all_done(1)) {
        return;
      }
      if (bad_payload.load() || gate.try_wait()) {
        finish(false, "signal did not publish data or left an extra permit");
        return;
      }
      reset_waiters();
      gate.signal(kWorkers);
      for (auto worker : workers) {
        emscripten_wasm_worker_post_function_v(worker, waiter);
      }
      ++phase;
      break;
    case 2:
      if (!all_done(kWorkers)) {
        return;
      }
      if (entered.load() != kWorkers || gate.try_wait()) {
        finish(false, "signal-before-wait lost or duplicated permits");
        return;
      }
      reset_waiters();
      for (auto worker : workers) {
        emscripten_wasm_worker_post_function_v(worker, waiter);
      }
      ++phase;
      break;
    case 3:
      if (started.load() != kWorkers ||
          emscripten_get_now() - phase_start < 100) {
        return;
      }
      if (entered.load() != 0) {
        finish(false, "a contended waiter entered without a permit");
        return;
      }
      gate.signal(kWorkers);
      ++phase;
      break;
    case 4:
      if (!all_done(kWorkers)) {
        return;
      }
      if (entered.load() != kWorkers || gate.try_wait() || bad_payload.load()) {
        finish(false, "batched signal failed to wake exactly all waiters");
        return;
      }
      completed = 0;
      phase_start = emscripten_get_now();
      for (auto worker : workers) {
        emscripten_wasm_worker_post_function_v(worker, worker_contend);
      }
      contend();
      ++phase;
      break;
    case 5:
      if (!all_done(kWorkers)) {
        return;
      }
      if (overlaps.load() || counter != (kWorkers + 1) * kIterations) {
        finish(false, "SkMutex allowed overlapping critical sections");
        return;
      }
      emscripten_wasm_worker_post_function_v(workers[0], release_main);
      ++phase;
      break;
    case 6:
      if (!main_wait_ready.load()) {
        return;
      }
      main_gate.wait();
      ++phase;
      finish(true, "semaphore ordering, wakeups and SkMutex contention passed");
      break;
  }
}
}  // namespace

int main() {
  for (auto& worker : workers) {
    worker = emscripten_malloc_wasm_worker(65536);
  }
  phase_start = emscripten_get_now();
  emscripten_wasm_worker_post_function_v(workers[0], waiter);
  emscripten_set_main_loop(tick, 0, false);
  return 0;
}
