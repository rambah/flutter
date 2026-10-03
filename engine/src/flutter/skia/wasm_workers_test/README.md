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

[Source build 37146362928](https://github.com/rambah/flutter/actions/runs/37146362928)
succeeded for commit `fa76ee7dcf55d037f3f8d95c3aaba92c7faff734`. The log confirms
compilation of `skia.SkSemaphore_wasm_workers.o` and linking of `skwasm.js`.
This covers the lightweight renderer. The matched-SDK upstream reproduction
tests below also exercise this source-built artifact.

A separate local diagnostic applied equivalent semaphore logic to a copy of
an existing renderer and ran a text-layout/raster stress fixture. That is
causal diagnosis, not a source-build integration test. The source-built
renderer has since passed the matched-SDK reproduction tests below; full
application integration and UX validation remain outstanding.

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

## Matched-SDK upstream reproductions (2026-10-03)

These tests use the actual source-built Skwasm renderer, not the earlier binary
patch. Framework base: `53d381d9067f16b1d4132f5b8f059a9cc5ddb9ec`; matching official
engine content hash: `41de58d04229db1294f191f7965e6009fb1bd7a2`; Dart:
`3.14.0-294.0.dev`. Both original and patched renderers use this same runtime
source baseline. The GitHub compare API confirms that this framework baseline
contains both #190048 and #191014, addressing the request to retest beyond the
older strike-cache fixes. Exact hashes identify the build; a shallow fork's
computed version label is not reliable provenance.

Environment: Chrome 154.0.8037.93, macOS arm64, fresh browser process/profile per
case, 1440 x 960 viewport. Apps were built with `--release --wasm
--no-web-resources-cdn`. Successful cases verify the Dart WASM download,
renderer WASM download, exported thread mode, isolation state and advancing
raster/layout counters. A host-side watchdog also detects an unresponsive page
or stalled rendering; a responsive JavaScript event loop alone is not a pass.

For #193439, the issue's Dart reproduction was used unchanged, with true
variable Montserrat and static Roboto font assets. This differs from the Linux
commenter's test, which placed static Roboto under both font filenames.

| #193439 case | Original renderer | Source-built fix |
| --- | --- | --- |
| Default, three fresh starts, 25 s each | 3/3 hung or trapped | 3/3 passed (643, 647, 647 raster frames) |
| Default, additional 120 s run | Not repeated for this duration | Passed: 3,167 raster frames, 254,787 layouts |
| `font=roboto`, 25 s | Hung | Passed |
| `layout=0`, 25 s | Passed | Passed |
| `zoom=0`, 25 s | Trapped during startup | Passed |
| `zoom=0&start=6`, 25 s | Passed | Passed |
| `wght=0`, 25 s | Main thread ran, but zero raster frames | Passed |
| DIP `isolate-and-credentialless`, 25 s | WASM runtime errors | Passed |
| COOP + COEP `credentialless`, 25 s | Memory-access trap | Passed |
| No isolation, single-threaded, 25 s | Passed | Passed |

The remaining isolated cases above use COOP `same-origin` and COEP
`require-corp`. Original failures included memory access out of bounds, a null
function and a function-signature mismatch. Some failures happened before the
first telemetry sample; their modes are configured by the same server and
bootstrap as the successful controls, rather than verified after the hang.
No patched run in this set emitted a page error or console error.

For #193695, the comparison app was checked out at the exact linked commit,
`kevmoo/flutter-wasm-compare@5c4e80e9f6e5ca461614d5eb1756545d69b5e571`.
Its bootstrap reads `mode`, not `renderer`. A separate run with the issue's
literal `renderer=wimp` query verified that it actually loaded Skwasm. The real
Wimp tests used `mode=wimp`, and verified both `wimp.wasm` and the runtime's
`skwasm_isWimp()` result, in addition to the thread-mode check.

[Source build 37147785222](https://github.com/rambah/flutter/actions/runs/37147785222)
succeeded for `e8f503e55090eee94e781465a9b809036e8315e3`, compiling the replacement
semaphore and linking both Skwasm and Wimp. This run supplied the patched Wimp
artifact; run 37146362928 supplied patched Skwasm. Both builds have the same
runtime source changes. The later artifact archive has SHA-256
`451b23f27eecf10a8d4c3077fcaaf3902409d7c9bf0fcf2a3c9671673ab403f4`.

| #193695 grid case, 25 s each | Original renderer | Source-built fix |
| --- | --- | --- |
| Skwasm, 100 and 1,000 nodes, headed Chrome | Both passed | Both passed |
| Skwasm, 100 and 1,000 nodes, headless SwiftShader | Both passed | Both passed |
| Wimp, 100 and 1,000 nodes, headed Chrome | Both passed | Both passed |
| Skwasm / Wimp, 1,000 nodes, no isolation | Both passed | Not separately repeated |

The grid-specific failure was **not reproduced** in these windows. Those runs
provide compatibility coverage, not an independent red/green demonstration.
SwiftShader was verified from the WebGL renderer string; this is still macOS
Chrome 154, not the commenter's Linux Chrome 153 environment. The literal-query
check was a separate 12-second run with the patched Skwasm renderer.

Across both issue fixtures, there were 39 fresh-process runs: all 19 patched
runs passed without page or console errors. Of 20 original-renderer runs,
eight reproduced a hang, trap or absent raster progress in the #193439
fixture; the remaining controls and grid cases passed. These counts exclude
the earlier primitive regression and binary-patch diagnostics.

No production release is approved by these tests. Main-thread spinning can
increase input latency under contention, even when it fixes corruption.
Before rollout, measure input latency, scrolling, long tasks, CPU use and frame
timing against single-threaded rendering; test repeated cold starts, font
loading, context loss, worker failure and the actual application integrations.
Do not hold these locks across operations requiring the main event loop.
Firefox/Safari single-threaded WASM policy and mobile builds are outside this
experiment's rollout scope.
