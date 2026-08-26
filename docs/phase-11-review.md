# Phase 11 final review

Phase 11 is a measured optimization pass. It adds a reproducible Release
profiler first, records the unchanged implementation, and changes only the
analysis and timeline paths where the measurements or code-level work count
showed avoidable repeated work.

## Profiling protocol

`vidscope_phase11_benchmarks` is built with the test suite and runs against the
generated `long_gop.mp4` fixture. It reports process-to-Qt startup, media open,
software decode FPS, seek median/P95, scaled thumbnail conversion, real-media
analysis FPS, synthetic analysis-kernel A/B throughput, luma-extraction A/B
throughput, two-hour analysis-pyramid rebuild and read throughput, 1,920-pixel
timeline rasterization/cached paint, memory-cache hits, four-reader contention,
and whether automatic hardware decode became active.

Performance values are diagnostic output, not brittle pass/fail thresholds.
The executable does fail on missing media, decode/analysis failure, changed
luma output, or failure to reuse the luma allocation. Numerical equivalence of
the precomputed-hash analysis path is also covered by the unit suite.

The workstation profile used MSVC Release, Qt 6.11.2, the configured shared
FFmpeg build, and the offscreen Qt platform. The table uses medians from the
final three warmed runs; A/B ratios compare both paths inside the same process.

| Measurement | Median result |
| --- | ---: |
| Process/module load to Qt application | 9.15 ms |
| Media open | 3.65 ms |
| Software decode | 11,667 FPS |
| Seek median / P95 | 1.80 / 4.65 ms |
| 320x180 thumbnail conversion median | 0.165 ms |
| Real-media analysis | 2,732 FPS |
| Two-hour, 180,000-sample pyramid rebuild | 11.41 ms |
| LOD views, one reader / four readers | 31,193 / 65,450 per second |
| Memory-cache lookup, one / four readers | 8.80M / 4.88M per second |
| Memory-cache hit rate for the resident working set | 100% |
| 704-bucket heatmap rasterization | 18.06 ms |
| Cached heatmap paint | 0.091 ms |
| Automatic hardware decode | active |

The first post-link cold run was retained as a separate observation: Qt startup
and first media open were 2.16 s and 2.89 s. Immediate reruns dropped to the
single-digit-millisecond range, so no application startup rewrite was inferred
from an environment-level cold-load event.

## Implemented optimizations

### Analysis allocation and hash reuse

`LumaExtractor` now has a destination-buffer overload. `AnalysisManager` swaps
two worker-confined `LumaPlane` instances, so steady-state analysis resizes
already-capacious vectors instead of allocating a new 160x90 buffer per frame.
The original value-returning API remains available for callers that need it.

The manager also carries the previous perceptual hash and passes both known
hashes into `VideoAnalyzer::compare`. This removes the two redundant
perceptual-hash calls formerly made for every adjacent pair while leaving the
motion, similarity, scene-change, and duplicate formulas unchanged.

Across the final paired samples, the median analysis-kernel gain was 42.8% and
the median reused-luma extraction gain was 10.6%. Unit coverage requires every
floating-point result from the precomputed path to equal the original path;
the real-frame profiler requires reusable extraction pixels to be identical and
the vector storage address to remain stable.

### Timeline paint cache

The corrected long-video profile showed the 704-bucket direct heatmap paint at
18.06 ms, already beyond a 16.67 ms 60 Hz frame budget before ticks, markers,
selection, and playhead were drawn. `TimelineWidget` now rasterizes only the
analysis layer into a device-pixel-ratio-aware premultiplied `QImage`.

Analysis batches/state changes, viewport changes, heatmap mode/weight changes,
resizing, and DPI changes invalidate the layer. Playhead, hover, frame/scene
markers, selection, and ruler overlays do not, so normal interaction uses a
0.091 ms median image blit. That is a 99.5% reduction for the heatmap portion of
steady-state paints while progressive analysis remains visible batch by batch.

## Reviewed paths intentionally retained

- Hardware decoding already tries D3D11VA then DXVA2 and safely transfers the
  selected surface for the QWidget renderer; the profile confirmed the hardware
  path was active. A zero-copy GPU renderer would be an architectural backend
  replacement, not a justified local Phase 11 change.
- Seek median/P95 and thumbnail conversion were already low. Thumbnail decode,
  scaling, disk I/O, and persistence remain off the GUI thread, and decoded
  previews are still delivered before disk serialization.
- `QImage` delivery remains implicitly shared. Reusing a published display
  buffer would violate immutable cross-thread ownership, so copy reduction was
  confined to worker-local luma memory.
- Analysis-pyramid reads and rebuilds remained bounded. Its shared mutex and
  copied pixel-budget LOD view were retained instead of introducing speculative
  lock-free state.
- Thumbnail memory LRU stayed at a 100% hit rate for the resident profile set,
  with millions of lookups per second under both single- and four-reader load.
  The separate memory/disk locks and byte budgets remain the cache policy.

## Acceptance coverage

- The existing analysis tests now cover exact equivalence of original and
  precomputed-hash metrics.
- Phase 7 integration coverage renders a cached repeat and verifies that a mode
  change invalidates the heatmap image.
- The Phase 11 executable validates real decode, seek, conversion, analysis,
  reusable luma storage, LOD, timeline, cache, contention, and hardware-policy
  paths while printing reproducible diagnostic values.
- The application version comes from CMake and remains visible in
  `Help > About VidScope`; this milestone is `0.11.0`.

The completion gate builds all targets with MSVC `/W4` and runs the full Debug
and Release suites. Phase 11 has 20 CTest entries and 71 unit cases.

## Explicitly deferred after Phase 11

- a zero-copy GPU presentation backend, pending backend-specific profiling;
- audio rendering and A/V synchronization;
- later professional features in Phase 12 and beyond.
