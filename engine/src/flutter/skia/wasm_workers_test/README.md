# Experimental Wasm Worker semaphore fix

This is an unapproved fix candidate for Flutter issues
[#193695](https://github.com/flutter/flutter/issues/193695) and
[#193439](https://github.com/flutter/flutter/issues/193439).

`-sWASM_WORKERS=1` without `-pthread` links POSIX semaphore stubs.
SkSemaphore's atomic fast path still works, but its contended slow path does
not actually wait. SkMutex therefore fails to exclude concurrent FreeType
access from layout and raster threads.

The Flutter build selects `SkSemaphore_wasm_workers.cpp` only for Wasm Worker
targets. It retains Skia's header and fast path, replacing the OS semaphore
with Emscripten's Wasm Worker semaphore API. Workers sleep on an atomic wait;
the main browser thread retries nonblocking acquisition. Signal-before-wait
and batched wakeups use the semaphore's permit count, so notifications are
not lost. Native targets and CanvasKit's non-worker configuration continue
using upstream Skia's implementation.

This is a temporary Flutter integration point. The underlying implementation
would preferably live in Skia. Keep the replacement aligned with the pinned
SkSemaphore header when rolling Skia.

## Run the targeted browser regression

Use an activated Emscripten 3.1.70 SDK (the version pinned by Flutter's
`activate_emsdk.py`), the Skia revision in the root DEPS file, Node.js,
Playwright, and an installed Google Chrome.

From the Flutter repository root:

```sh
python3 engine/src/flutter/skia/wasm_workers_test/build.py \
  --emxx /path/to/emsdk/upstream/emscripten/em++ \
  --skia engine/src/flutter/third_party/skia \
  --output /tmp/wasm-semaphore-test
node engine/src/flutter/skia/wasm_workers_test/run.mjs /tmp/wasm-semaphore-test
```

Set `PLAYWRIGHT_MODULE` to an absolute path to Playwright's `index.mjs` if it
is not installed in Node's module search path. The runner uses its own Chrome
instance and serves only on loopback. It records results in the build folder.
The upstream control must fail specifically because an unsignaled waiter
proceeds; the patched implementation must pass five fresh browser-page runs.

Coverage includes unsignaled waiting, data publication, signal-before-wait,
batched wakeups with three waiting workers, permit conservation, 200,000
SkMutex-protected increments shared between the main thread and three workers,
and waking the spinning main thread directly from a worker. A host watchdog
bounds a hung test and cleanup.

## Validation and remaining work

On 2026-10-03, the targeted test was built with Emscripten 3.1.70 and Skia
`94c06062b123294806360ebcf299be7785aec5fe` and run in Chrome 154.0.8037.93 on
macOS. The original implementation failed at the first unsignaled wait.
All five patched runs passed with 200,000 increments and zero overlapping
critical sections each. This validates the primitive, not the whole engine.

The fork's `Experimental Skwasm semaphore build` workflow additionally attempts
a source build of the complete lightweight Skwasm renderer. Its artifacts are
experimental and must not be substituted into a different engine revision.

A separate local diagnostic applied equivalent semaphore logic to a copy of
an existing renderer and ran a text-layout/raster stress fixture. That is
causal diagnosis, not a source-build integration test. The source-built
renderer still needs matched-SDK stress and application integration tests.

In that diagnostic, the unchanged roundtrip control trapped in FreeType after
approximately six seconds. The semaphore-corrected copy completed 300 seconds
and 5,409 rendered frames without a reported runtime error. Both exercised
multi-threaded Skwasm in the same Chrome version. This supports the suspected
cause, but does not establish production stability or unchanged input latency.
Three additional cold browser launches each completed a 30-second diagnostic
stress run (511, 528 and 528 rendered frames, with no reported runtime errors).
A mocked application home view rendered and correctly filtered/restored search
results in both single-threaded and multi-threaded mode with the diagnostic
copy. This does not cover live authentication, payments or embedded services.

No production release is approved by these tests. Main-thread spinning can
increase input latency under contention, even when it fixes corruption.
Before rollout, measure input latency, scrolling, long tasks, CPU use and frame
timing against single-threaded rendering; test repeated cold starts, font
loading, context loss, worker failure and the actual application integrations.
Do not hold these locks across operations requiring the main event loop.
Firefox/Safari single-threaded WASM policy and mobile builds are outside this
experiment's rollout scope.
