# Phase 5 final review

Phase 5 adds a configurable asynchronous preview filmstrip while retaining the
Phase 4 thumbnail worker pool, cache, priority, cancellation, and media-epoch
boundaries. The filmstrip does not create a widget or decoder for each frame,
and it never performs FFmpeg work on the GUI thread.

## Delivered architecture

- `filmstrip::FilmstripModel` is a GUI-thread policy model with no QWidget or
  FFmpeg dependencies. It produces bounded plans for 8, 16, 20, 32, or custom
  1-64 targets.
- `widgets::FilmstripController` owns one logical batch generation at a time.
  It cancels the old filmstrip priority lanes, clears old delivery mappings,
  submits the new target set, and accepts a result only when both request and
  batch generations still match.
- `widgets::FilmstripWidget` is one custom-painted surface. It paints loading,
  failure, image, timestamp, established frame index, picture type, and
  keyframe state without allocating one child QWidget per cell.
- `ThumbnailManager` now exposes priority-scoped cancellation plus explicit
  cancellation completion. A filmstrip refresh can stop its own work without
  cancelling hover requests or changing the selected media epoch, and a
  still-current cell can be retried if bounded-queue preemption removes it.
- `MainWindow` owns the mode/count controls, persists them through `QSettings`,
  routes click-to-seek, and exposes a double-click integration hook for the
  full Frame Inspector planned for Phase 9.

## Mode semantics

### Entire Video

Targets are distributed over the inclusive application duration. Time remains
authoritative; no nominal-FPS frame-number conversion is used.

### Around Current Position

When the timeline has a sufficiently large, strictly time-ordered,
presentation-contiguous set from one playback session, the plan uses those
actual frame boundaries and established presentation indices. Otherwise it
uses bounded timestamp requests around the playhead, based first on actual
known frame duration/median duration and only then on a 40 ms request-spacing
fallback. The decoder still returns the real nearest presentation frame; the
fallback never asserts a synthetic frame identity.

### Visible Timeline

Targets are recomputed from the centralized timeline viewport after a short
GUI-side debounce. Timeline zoom/pan mathematics remain in `TimelineModel`.

### Selected Range

The strip stays empty with a clear selection-required state until a normalized
In/Out selection exists, then distributes targets only inside that exact time
range.

## Scheduling and bounds

```text
Filmstrip mode/count/range change
    -> new batch generation
    -> cancel VisibleThumbnail / NearPlayhead jobs
    -> testable target plan
    -> submit farthest first (scheduler runs newest first)
    -> bounded ThumbnailManager queue
    -> memory cache / disk cache / reusable FFmpeg worker
    -> queued GUI delivery
    -> request-generation lookup
    -> batch-generation validation
    -> update one filmstrip cell

If a pending or active filmstrip request is preempted:

    cancellation completion
    -> current-batch/loading-cell validation
    -> short coalesced retry
```

Around-playhead requests use `NearPlayhead`; other filmstrip modes use
`VisibleThumbnail`. Hover remains higher priority. Custom count is hard-capped
at 64, while the default manager queue is bounded at 96 to leave interactive
headroom for a full custom strip plus hover work. A deliberately tighter queue
or higher-priority preemption reports cancellation; the controller retries only
the still-current loading cell after a short delay. Superseded batches remain
invalid and are never revived. All caches, worker frame queues, and decoder
contexts remain bounded and deterministic at shutdown.

## Interaction and metadata

- Single click seeks to the decoded frame timestamp when available, otherwise
  to the requested application timestamp.
- Double click emits timestamp plus established presentation-index metadata for
  the future Frame Inspector. Phase 5 pauses/seeks and reports that the complete
  inspector UI belongs to Phase 9 rather than fabricating it early.
- Keyboard Left/Right selects a cell; Enter/Space activates it.
- Tooltips expose requested/decoded time, frame index, PTS, DTS, picture type,
  and keyframe state.
- Motion and similarity fields remain optional and are not fabricated before
  Phase 6.

## Acceptance coverage

`tests/unit/FilmstripModelTests.cpp` covers:

- count presets and custom clamping;
- full-duration inclusive distribution;
- visible-range and selected-range plans;
- selection-required state;
- exact contiguous known-frame neighborhoods;
- bounded no-index fallback around the playhead;
- no-media behavior.

`tests/integration/Phase5FilmstripTests.cpp` covers:

- a single custom-painted QWidget and click/double-click activation;
- real asynchronous FFmpeg thumbnail generation for an eight-cell full-video
  strip;
- stale 32-cell batch supersession by an eight-cell near-playhead batch;
- deterministic bounded-queue eviction followed by current-cell retry;
- selected-range activation and asynchronous population;
- image-size bounds, ordered decoded times, and zero failed cells.

`tests/unit/ThumbnailSchedulerTests.cpp` also verifies exactly-once
cancellation notification for duplicate replacement, queue eviction, and
maintenance cancellation. CMake adds the `vidscope.phase5_filmstrip` gate and
propagates the `phase5` label to unit and application smoke tests.

## Validation boundary

The authoritative release gate is still a real Qt 6.11.2 plus FFmpeg SDK build
on the target platform, followed by the full Debug and Release CTest presets.
The portable validation performed in this workspace is recorded in the final
completion report; no unavailable native Qt build is claimed.

## Explicitly deferred after Phase 5

- Phase 6-7: progressive motion/similarity analysis, persistent analysis cache,
  LOD aggregation, and heatmaps
- Phase 8: automatic scene markers, duplicate sections, and freeze detection
- Phase 9: complete Frame Inspector, pixel magnifier, A/B comparison, SSIM, and
  PSNR
- Later phases: export/contact sheets, audio rendering/A-V sync, and
  optimization/professional features
