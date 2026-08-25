# Phase 6 final review

Phase 6 adds a production-oriented progressive video-analysis subsystem while
preserving the playback, timeline, thumbnail, filmstrip, and GUI-thread
boundaries established in Phases 0-5.

## Delivered architecture

- `analysis::LumaExtractor` uses a reusable `SwsContext` to reduce decoded
  software frames to a configurable, bounded 160x90 GRAY8 plane.
- `analysis::VideoAnalyzer` computes normalized mean absolute luma difference
  for motion and a separately testable pixel-difference/histogram aggregate for
  similarity. Both results are clamped to `[0, 1]`.
- `analysis::AnalysisStore` keeps raw timestamp-authoritative samples sorted,
  supports exact presentation-index hints and nearest-time lookup, and enforces
  a configurable hard sample cap. Phase 7 can build LOD aggregates above it
  without discarding raw values.
- `analysis::AnalysisManager` owns one `std::jthread` and one reusable,
  thread-confined `PlaybackSession`. Playhead and visible-range tasks preempt a
  resumable full-video background task. Playback and active scrubbing suspend
  analysis decode.
- `analysis::AnalysisCache` persists compact raw samples through `QSaveFile`.
  Identity includes canonical source path, size, modification time, selected
  stream, schema version, and algorithm version. Document/sample/disk budgets
  are enforced and old documents are pruned by modification time.
- `HoverPreviewController` and `FilmstripController` query the thread-safe store
  when thumbnails arrive and when batched analysis notifications cover existing
  cells. No one-event-per-frame GUI flood is introduced.

## Scheduling and cancellation

```text
Media open
    -> increment media epoch
    -> cancel stale decode
    -> load compatible cache on analysis worker
    -> publish cached coverage in one batch
    -> queue full-video background range

Playhead / visible-range request
    -> coalesce same-priority pending work
    -> preempt lower-priority background work
    -> seek with the frame-accurate PlaybackSession
    -> decode a bounded preroll for prior-frame context
    -> extract luma and publish raw samples in batches
    -> resume background range

Playback or active scrub
    -> cancel current range
    -> retain a resumable background timestamp
    -> wait without busy looping
    -> continue after interactive work ends
```

The worker checks cancellation before decode, after luma extraction, and before
publishing a sample. Media-epoch validation rejects stale queued GUI delivery.
Shutdown cancels active FFmpeg work, wakes the condition variable, and joins the
worker before the manager is destroyed.

## Score semantics

- Motion is `mean(abs(currentY - previousY)) / 255`.
- Similarity combines normalized per-pixel luma agreement (80%) with 32-bin
  histogram intersection (20%).
- `0` motion means no measured luma change; `1` is maximal change.
- `0` similarity means highly different; `1` means nearly identical.
- The first frame without decoded prior context has absent motion/similarity
  values. The UI reports it as not analyzed rather than inventing a score.

## Bounds and responsiveness

- Default raw-sample cap: 2,000,000.
- Default analysis frame cache: at most 32 MiB.
- Default analysis forward queue: at most 16 MiB and four frames.
- Luma working data: 14,400 bytes per current/previous plane at 160x90.
- Default cache disk budget: 512 MiB; per-document cap: 256 MiB.
- Cache and decode I/O never run on the GUI thread.
- GUI notifications are batched every 32 analyzed frames by default.

## Acceptance coverage

`tests/unit/AnalysisTests.cpp` covers:

- identical, maximally different, and partially changed luma scores;
- score normalization;
- sorted store insertion, replacement, range lookup, and hard capacity;
- versioned analysis-cache round trip and source-change invalidation.

`tests/integration/Phase6AnalysisTests.cpp` covers:

- real FFmpeg decoding through the progressive analysis manager;
- presentation-time ordering and bounded score values;
- absent first-frame prior-context scores;
- completed cache persistence and reload;
- playback-priority pause and clean resume;
- deterministic manager destruction after background work.

The Phase 6 CTest label also includes unit tests and application/media smoke
tests. The final completion run built every Debug and Release target and passed
all 15 tests in both configurations.

During the full regression pass, six Phase 5 test inputs/assertions were fixed
to convert `seconds`/`milliseconds` to nanoseconds explicitly. Production
filmstrip timestamps were already correct; the tests had compared raw duration
`count()` values as though they were nanoseconds.

## Explicitly deferred after Phase 6

- Phase 7: timeline heatmap rendering, mode controls, combined weights, and LOD
  aggregation for overview painting
- Phase 8: scene markers/navigation, duplicate sections, and freeze detection
- Phase 9+: complete Frame Inspector, A/B comparison, export, audio/A-V sync,
  and later optimization/professional features

