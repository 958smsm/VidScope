# Phase 4 final review

Phase 4 adds asynchronous decoded hover previews without weakening the Phase 2
frame-accuracy engine or putting decode work on the GUI thread. The timeline
continues to own only coordinates and lightweight observed-frame metadata; a
separate thumbnail subsystem owns preview scheduling, worker decoders, image
conversion, caching, cancellation, and stale-result filtering.

## Delivered architecture

- `ThumbnailManager` is the GUI-thread application service. It assigns request
  generations, snapshots the current media identity/epoch, performs memory
  cache hits without blocking on disk or decode, and accepts worker delivery
  only while the generation and media lifecycle remain current.
- `ThumbnailScheduler` is a bounded priority queue with active cancellation.
  Hover requests supersede stale pending and active interactive work; pending
  duplicate cache keys are coalesced, and lower-priority duplicates cannot
  displace a higher-priority job.
- A small `std::jthread` worker pool owns reusable, thread-confined
  `PlaybackSession` and `FrameConverter` instances. Workers are separate from
  the playback decoder and close/reopen deterministically on media lifecycle
  changes.
- `ThumbnailCache` provides a byte-bounded in-memory LRU plus a bounded,
  versioned disk cache. Cache identity includes canonical path, file size,
  modification time, video stream index, and schema version.
- `HoverPreviewController` debounces cursor motion, requests only the latest
  target, cancels on leave/reset, and rejects mismatched result generations.
- `HoverPreviewPopup` is non-interactive, follows the global cursor, and clamps
  itself to the visible application/screen intersection.

## Request and delivery contract

```text
Timeline hover
    -> debounce
    -> generation + exact application timestamp
    -> memory cache
    -> bounded priority scheduler
    -> disk cache
    -> reusable worker PlaybackSession seek (nearest bias)
    -> direct FFmpeg scale to thumbnail size
    -> bounded memory-cache insert
    -> queued GUI delivery
    -> worker-side versioned disk persistence
    -> media epoch + latest generation validation
    -> popup
```

No UI event handler opens FFmpeg, seeks, decodes, scales, or accesses disk. A
new hover generation cancels stale work and stale queued results are filtered a
second time at GUI delivery. The memory and disk cache paths have independent
locks, and decoded output is posted before worker-side disk serialization, so a
memory hit or visible preview cannot queue behind image encoding or file I/O. A
media close/open increments the media epoch, cancels all jobs, wakes workers,
and forces each reusable session to release its old FFmpeg state.

## Frame and timestamp semantics

- Requests use `media::MediaTime` nanoseconds from the central timeline model.
- Worker selection uses the existing exact `PlaybackSession::seek` path with
  `SeekBias::Nearest`; no `milliseconds * FPS` or nominal frame index mapping is
  introduced.
- UI generations remain delivery/cancellation identities. They are not reused as
  per-session seek sequence numbers because priority dispatch can intentionally
  process queued jobs out of numerical generation order.
- The popup reports the decoded frame's actual presentation time, established
  presentation index when available, PTS/DTS metadata retained by the cache,
  picture type, and keyframe flag.
- Nearby cursor timestamps may resolve to the same decoded presentation frame,
  but the requested timestamp remains part of the cache key and the decoded
  frame metadata remains authoritative.
- Motion and similarity fields are optional. Phase 4 displays “not analyzed”
  until Phase 6 provides real scores.

## Bounds, priority, and shutdown

- Default preview pool: two workers, each with bounded frame-cache and forward
  queue budgets.
- Default shared preview memory cache: 96 MiB.
- Default disk cache: 1 GiB under the Qt application cache location.
- Pending jobs are capped; cache entries and worker queues cannot grow without
  configured bounds.
- Hover has the highest thumbnail priority, followed by explicit user targets,
  visible thumbnails, near-playhead work, and background precache.
- `std::stop_callback`, scheduler close, cancellation sources, condition
  notification, and `std::jthread` joins provide deterministic shutdown.
- Software decoding is the default preview correctness path. The abstraction
  retains the existing decoder option for later controlled hardware use.

## Acceptance coverage

`tests/unit/ThumbnailCacheTests.cpp` covers LRU memory eviction, complete disk
metadata/image round trips, and source-file/stream cache identity changes.

`tests/unit/ThumbnailSchedulerTests.cpp` covers latest-hover replacement,
active stale-job cancellation, priority-safe pending duplicate coalescing, and
media maintenance wake/cancellation.

`tests/integration/Phase4HoverPreviewTests.cpp` uses generated CFR/B-frame and
long-GOP media to cover:

- rapid A-to-B-to-C hover requests with only C accepted;
- direct decoded output and exact presentation metadata;
- immediate memory-cache reuse;
- disk-cache reuse after destroying and recreating the worker pool;
- offscreen timeline hover, popup imagery, cursor following, and application /
  screen geometry clamping;
- leave cancellation and popup dismissal;
- bounded shutdown while decode work is active.

The CMake test labels add `phase4`, `thumbnail`, `media`, and `ui` gates as
appropriate. The application and tests require Qt 6.11.2 exactly.

## Validation in this workspace

The available container does not include a Qt development SDK or FFmpeg header
and import-library packages, so it cannot perform the authoritative Qt 6.11.2
compile and CTest run. The following checks were completed here instead:

- exact-version CMake configure/generate structure was exercised with a local
  metadata-only Qt 6.11.2/FFmpeg package shim, and a 6.11.3 shim was rejected;
- all configured source references were verified to exist and all 13 CTest
  registrations, including `vidscope.phase4_hover_preview`, were enumerated;
- the scheduler implementation and its six unit tests compiled and ran 6/6
  against minimal API stubs, including Clang ASan/UBSan and GCC TSan runs;
- `ThumbnailManager.cpp`, `HoverPreviewPopup.cpp`, and
  `HoverPreviewController.cpp` passed focused Clang C++20 syntax/warning checks
  against API stubs that preserve their ownership, cancellation, queued-delivery,
  and geometry interfaces;
- the complete synthetic-media fixture generator ran successfully with the
  installed FFmpeg CLI;
- JSON parsing, `git diff --check`, placeholder scans, and archive-integrity
  checks are part of the final packaging gate.

The release gate remains the real commands in `README.md`: configure against
Qt 6.11.2 exactly and the FFmpeg development SDK, then build and run the full
Debug and Release CTest presets. No claim is made that those unavailable binary
build/tests ran in this container.

## Explicitly deferred after Phase 4

- Phase 5: configurable 8/16/20/32/custom filmstrip UI and full-video,
  near-playhead, visible-range, and selected-range population policies
- Phase 6-7: progressive motion/similarity/scene analysis, persistent analysis
  cache, LOD aggregation, and timeline heatmaps
- Phase 8: automatic scene markers, duplicate sections, and freeze detection
- Later phases: frame inspector, comparison, audio rendering/A-V sync, and
  export/contact sheets
