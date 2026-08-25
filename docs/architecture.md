# VidScope Phase 0-10 architecture

VidScope is a C++20, Qt 6.11.2, direct-FFmpeg video inspection application.
Phase 10 adds full-resolution frame/range export and bounded contact-sheet
generation above the playback, analysis, detection, and inspection pipeline
without
weakening the frame-accurate playback, timestamp, ownership, preview,
cancellation, or thread boundaries established earlier.

## Dependency direction

```text
MainWindow / TimelineWidget / VideoViewport / HoverPreviewPopup / FilmstripWidget
        |                |                    |                    |
        |                |                    +-> HoverPreviewController
        |                |                                         |
        |                +-> TimelineModel <- FilmstripModel        |
        |                |   (GUI-owned state)        |              |
        |                +-> TimelineHeatmapRenderer  |              |
        |                                      FilmstripController  |
        |                                               |            |
        +--------------------------------------- ThumbnailManager <--+
                                                        |
                                                ThumbnailScheduler
PlaybackController (Qt adapter, GUI-thread API)          |
        |                                       reusable worker pool
PlaybackSession (single playback-worker confinement)     |
        |                                       PlaybackSession(s)
MediaSource -> Demuxer -> VideoDecoder                    |
        |                                               |
      FFmpeg <-------------------------------------------+

AnalysisManager (GUI API / thread-safe raw and LOD queries)
        |
        +-> AnalysisStore <---- HoverPreviewController / FilmstripController
        +-> AnalysisPyramid ---> TimelineWidget
        +-> DetectionEngine ---> AnalysisResultsPanel
                              +-> automatic TimelineModel scene markers
        |
        +-> one cancellable analysis worker
                -> PlaybackSession -> LumaExtractor -> VideoAnalyzer
                -> versioned AnalysisCache

FrameInspectorPanel <---- current DecodedFrame + current display QImage
        |                           |
        +-> VideoViewport ----------+-> O(1) paused pixel lookup
        |
        +-> FrameComparisonManager -> one coalescing worker
                                      -> FrameComparison
                                      -> SSIM / PSNR / difference image

MainWindow -> ExportManager -> one cancellable export worker
                |               -> PlaybackSession (software decode)
                |               -> FrameConverter (source resolution)
                |               -> QSaveFile + QImageWriter
                |
                +-> ExportPlanner <- Timeline / Detection / Analysis targets
                                    -> bounded contact-sheet QImage + QPainter
```

`TimelineWidget` never calls FFmpeg. It owns a `TimelineModel`, turns pointer
and wheel input into model mutations and seek signals, and paints from bounded
model views. `MainWindow` owns the remappable command actions and connects
timeline seek requests to `PlaybackController`; the controller retains the
generation-based, coalescing worker boundary.

`TimelineModel` belongs to the engine layer and contains no QWidget behavior.
It is explicitly GUI-thread-owned, so it needs no internal locks. All
pixel/time conversion, viewport state, observed frame boundaries, markers, and
selection semantics live there instead of being duplicated in event handlers.

`ThumbnailManager` is a separate application service. Timeline hover signals
contain only the mapped timestamp, nearest established presentation-index hint,
and global cursor position. The manager owns no widget; it snapshots the media
epoch, checks memory cache on the GUI thread, and sends misses to a bounded
priority scheduler. `HoverPreviewController` owns debounce and popup state, and
only touches QWidget objects on the GUI thread.

`FilmstripModel` reads only centralized `TimelineModel` state and produces a
bounded target plan. `FilmstripController` maps that plan onto one tagged
thumbnail batch, rejects superseded delivery, retries a still-current loading
cell when the shared bounded scheduler reports preemption, and never decodes.
`FilmstripWidget` paints every cell itself; custom counts do not create one Qt
widget per frame.

`AnalysisManager` is a separate application service. It coalesces playhead and
visible-range requests ahead of a resumable full-video background task, owns no
widget, and exposes raw samples through a bounded thread-safe store. A separate
thread-safe `AnalysisPyramid` mirrors compact aggregates, not image data.
Playback and active scrubbing suspend analysis decode. Batched GUI notifications
let hover, filmstrip, and timeline consumers refresh without queueing one event
per analyzed frame.

Detection runs against a bounded sample snapshot, never against decoded image
surfaces. A high-priority detection task can rebuild results after thresholds
change without seeking or decoding again. Progressive decode schedules
occasional detector refreshes at an increasing sample interval and a mandatory
final refresh at completion. Result snapshots are protected independently from
the raw store and exposed by value to GUI consumers.

`TimelineHeatmapRenderer` is stateless and receives only a bounded LOD view,
mode, and external combination weights. Motion and similarity remain separate
raw values. Combined mode blends motion with similarity difference and
normalizes only the components available in a bucket; rendering does not own or
hard-code the analysis algorithms.

`FrameInspectorPanel` retains only the current display frame and the two
explicitly captured A/B frames. It presents immutable decoded metadata plus the
current analysis sample on the GUI thread. `VideoViewport` owns display-only
zoom, pointer-to-image mapping, and the inexpensive side-by-side/overlay/wipe/
blink compositions. `FrameComparisonManager` coalesces requests on one worker,
cancels stale computation, and queues only the newest SSIM/PSNR or derived-image
result back to the GUI thread.

`ExportManager` accepts at most one pending or active request. Its worker
owns a thread-confined software `PlaybackSession` and `FrameConverter`,
decodes presentation-order ranges or exact timestamp targets, and publishes
only progress and a bounded summary to the GUI thread. Timeline selections,
visible ranges, detections, and high-motion analysis samples are copied as
lightweight request metadata before work starts.

## Ownership and lifetime

- One process-lifetime RAII guard owns FFmpeg network initialization. Application
  startup and every `MediaSource` open share the same thread-safe initializer.
- `MediaSource` exclusively owns `AVFormatContext` and its interrupt state.
  Ownership begins immediately after allocation and is released only across the
  non-throwing `avformat_open_input` call.
- `Demuxer` borrows `MediaSource` and cannot outlive it.
- `VideoDecoder` exclusively owns `AVCodecContext` and the optional hardware
  device context.
- `FrameStorage` owns an `av_frame_clone` reference. `DecodedFrame` is immutable
  after publication; cache, queue, and GUI share the retained FFmpeg buffers.
- `TimelineModel` stores only lightweight `FrameBoundary` identity/time/duration
  metadata. It does not retain decoded image surfaces.
- `PlaybackController` cancels active work, requests stop, wakes its condition
  variable, joins its worker, and only then destroys converter/session/FFmpeg
  state.
- Each thumbnail worker owns one reusable, thread-confined `PlaybackSession` and
  `FrameConverter`. A media epoch change cancels active jobs, wakes the pool,
  closes each old session, and prevents old queued delivery from being accepted.
- `ThumbnailCache` owns its memory-LRU entries and versioned disk representation.
  Cached `QImage` values are implicitly shared across queued delivery. Memory-LRU
  mutation and disk serialization use separate locks, so a GUI-thread memory hit
  cannot wait behind image encoding, file I/O, or disk pruning.
- `FilmstripController` owns no worker thread and retains only media metadata,
  generation-to-cell mappings, and GUI timers. Its destruction cancels only the
  filmstrip priority lanes before `ThumbnailManager` joins its workers.
- `HoverPreviewController` explicitly deletes its top-level tooltip popup before
  its anchor window is destroyed. `MainWindow` destroys hover/filmstrip
  coordination first, then joins the analysis and thumbnail workers before the
  playback controller.
- The analysis worker owns one thread-confined `PlaybackSession` and
  `LumaExtractor`. A media epoch cancels active decode, rejects queued delivery,
  replaces the sample store, and prevents old-media cache results from becoming
  visible. `std::jthread` shutdown cancels, wakes, and joins before destruction.
- `FrameComparisonManager` owns one `std::jthread` and implicitly shared
  A/B `QImage` snapshots. A new comparison cancels active work and replaces any
  pending request; destruction requests cancellation, wakes, and joins before
  the captured images are released.
- `ExportManager` owns one `std::jthread`, one thread-confined export
  session/converter, and at most one request. A media epoch cancels active work;
  generation checks suppress stale progress, and shutdown wakes and joins
  before UI receivers or media metadata are released.

## Timestamp and identity invariants

- Stream timestamps remain integers in their stream time base until converted
  with `av_rescale_q` or `av_rescale_q_rnd`.
- Application time and duration are normalized to the selected video-stream
  origin. They are never computed as `frame / nominal_fps`.
- FFmpeg receive order is presentation order. DTS is metadata, not the display
  ordering key.
- A presentation index is assigned only where continuity is established. A
  bounded `(presentationTime, PTS)` anchor map can reconnect a seek to known
  indexing; repeated keys are marked ambiguous.
- The timeline treats an established nonnegative presentation index as the
  logical identity across seek/redecode serials. Re-observation upserts that
  boundary instead of creating a duplicate. Unindexed observations retain full
  `FrameId` identity.
- Equal-time, visually distinct frames remain separate presentation frames.
- Timeline observations preserve actual `presentationTime` and duration and are
  sorted by presentation time, then stable frame identity; neither DTS nor
  nominal rate participates.

## Timeline coordinate and viewport model

`media::MediaTime` is nanoseconds and remains the authoritative timeline
coordinate. The model owns `timeToPixel`, `pixelToTime`, and `frameToPixel`.
It subtracts integer viewport-relative nanoseconds before floating-point pixel
scaling, avoiding loss from converting large absolute timestamps first. Anchor
zoom uses integer rescaling so timestamps beyond exact double-integer range
remain stable.

All temporal input is clamped to `[0, duration]`. A viewport is normalized,
clamped to media extent, and kept at least 1 microsecond wide unless the media
itself is shorter. Zoom factors greater than one zoom in. Zoom preserves the
anchor's relative pixel position except where a media boundary necessarily
clamps the window. Pan preserves visible duration.

Major ruler intervals use the next 1/2/5 multiple of a power of ten that meets
minimum pixel spacing. Ticks are aligned to application time zero and capped.
Frame ticks are exposed only when exact visible boundaries have sufficient
pixel separation and the primitive cap is respected. Equal-time collision
groups remain deterministic and visible; density is evaluated using positive
spacing between distinct timestamps. Frame and marker visible queries include
both viewport endpoints.

## Markers and selection

Stored markers are sorted by `(time, id)`. A surviving marker keeps its
monotonic ID through sorting and updates, and removed or reset IDs are never
reused. Keyframe, scene, chapter, and bookmark kinds share the bounded marker
store. Observed decoded keyframes are an independent derived frame layer, so
they do not consume stored-marker capacity. Scene-marker navigation is ready for
detected scene markers are replaced from the latest detection snapshot.
Previous/next scene actions query the same sorted marker store, so navigation
and the visible scene lines cannot disagree.

Selections normalize forward and reverse gestures into `start <= end` and are
clamped to media extent. Selection details return exact first/last established
frame identities and the number of known boundaries. A total frame count is
reported only when observed presentation indices are contiguous and the
selection endpoints exactly match the first and last observed timestamps.
Nominal FPS is never used to infer missing coverage.

## Seeking and stepping

An arbitrary seek plans an absolute stream timestamp, seeks to or before a safe
keyframe, flushes codec state, decodes forward in presentation order, applies
the requested before/after/nearest bias, drains delayed frames at EOF, and
retains neighboring frames. New request generations cancel stale work.

Timeline click/drag maps pixels to clamped application time and emits the same
precise seek path. Repeated identical scrub coordinates are coalesced, and an
ordinary click emits one seek rather than a second unchanged release request.
Marker activation reports the hit separately without enqueuing another seek.
The widget ignores asynchronous position callbacks during an active scrub so
stale delivery cannot pull the local playhead away from the user's current
target. Focus loss cancels any active gesture.

Next-frame consumes the immediate buffered/cache successor. Previous-frame
uses bounded history first and reconstructs from an earlier safe seek point on
a miss. Keyframe and signed N-frame commands use the same exact
presentation-order primitives.

## Memory, threading, and responsiveness

`FrameCache` enforces a byte budget; `FrameQueue` has independent item and byte
bounds. The controller queue is capped and merges repeated navigation while
newest seeks supersede stale seeks. Decode and BGRA conversion run on the
worker; QWidget access stays on the GUI thread. Playback scheduling rejects the
`kNoMediaTime` sentinel before arithmetic and falls back to a bounded actual,
nominal, or 40 ms frame duration when presentation time is unavailable.

Thumbnail work is independent from playback work. The shared scheduler has a
hard pending-job bound and explicit priority lanes; hover supersedes stale
interactive and background work. The default two-worker pool has independent
bounded frame caches and forward queues. The shared thumbnail memory cache and
disk cache are byte-bounded. Disk lookup, decode, scaling, and cache writes run
outside the GUI thread; only a final generation/epoch validation and signal
emission run on it. `std::stop_callback`, condition notification, cancellation
sources, and `std::jthread` joins provide deterministic pool shutdown.

Analysis uses its own software-decoding `PlaybackSession` with a 32 MiB frame
cache, a 16 MiB forward queue, and at most four queued frames. Each decoded
surface is immediately reduced to a fixed 160x90 GRAY8 plane; only compact raw
scores and 64-bit fingerprints remain in `AnalysisStore`, which has a
configurable hard sample cap.
Interactive ranges preempt background analysis, while playback and scrubbing
pause it. Disk checkpoints use `QSaveFile`, a schema/algorithm version, source
path/size/modification time, stream index, per-document bounds, and global LRU
pruning.

Paused pixel inspection maps the mouse through the current viewport destination
rectangle and reads one pixel from the already converted display `QImage`. It
does not copy or read back the full frame per mouse move. SSIM and PSNR operate
on normalized RGB images off the GUI thread, checking cancellation per row and
8x8 SSIM block. Only derived difference/SSIM-map modes allocate another full-
frame surface; simple compositions reuse the captured implicitly shared images.

The detector caps output per kind, bounds fingerprint candidate history, and
uses only compact sample metadata. Exact duplicate classification compares the
downscaled-luma content hash; near duplicates and repeated sections use
normalized similarity and bounded perceptual-hash distance. Freeze results also
require configurable frame-count and duration minima. Scene candidates are
thresholded local maxima with a minimum temporal separation.

The heatmap pyramid caps level zero at 262,144 temporal buckets by default.
Higher levels aggregate four children, so total storage is geometrically
bounded. Each progressive delivery rebuilds only touched base buckets and their
ancestors. A paint query chooses the first level whose visible bucket count fits
the device-pixel budget and copies only populated buckets, with the existing
4,096 timeline primitive ceiling as a final guard. Multi-hour overview painting
therefore remains proportional to viewport pixels rather than frame count.

Timeline known-frame metadata defaults to a hard 100,000-entry cap and uses
deque storage plus a presentation-index location map for efficient append,
lookup, and endpoint eviction. At the frame cap, the model evicts the temporal
endpoint farthest from the current playhead while maintaining sorted order.
Stored marker metadata is capped at 10,000 entries. Painting uses one custom
widget, bounded views, pixel-density gates, and a 4,096 frame-primitive ceiling;
it never creates one QWidget or unconditional paint primitive per video frame.

## Metadata and hardware decoding

`DecodedFrame` exposes presentation identity, PTS/DTS/best-effort timestamp,
actual FFmpeg duration when present, keyframe/picture type, geometry, pixel
format, color range/space/primaries/transfer, chroma location, bit depth,
HDR10 mastering-display metadata, and MaxCLL/MaxFALL content-light metadata.
The Frame Inspector exposes these values directly together with the matching
motion, similarity, and scene score when analysis is available; unknown values
remain explicit rather than being inferred.

The decoder tries D3D11VA then DXVA2 on Windows and transfers hardware surfaces
to system memory for the current renderer. Device/configuration/open failures
retry the CPU decoder, so correctness does not depend on GPU availability.

## Phase boundary

Phase 6 includes progressive presentation-order analysis, bounded luma
extraction, normalized motion scoring, independently computed similarity,
priority/preemption, playback/scrub suspension, batched delivery, bounded raw
sample storage, and persistent versioned cache reload. The first frame in a
decoded run has no fabricated prior-frame score. Hover previews and filmstrip
cells show real scores when present and retain “not analyzed” semantics for
unknown samples.

Phase 7 adds progressive Motion, Similarity, and Combined timeline
visualizations, configurable blend weights, min/max/average LOD statistics,
bounded four-to-one hierarchy construction, pixel-density level selection,
sparse-coverage rendering, persisted mode selection, and long-video primitive
bounds. Unknown analysis regions remain visually empty.

Phase 8 adds raw scene/duplicate scores and content/perceptual fingerprints,
scene-score LOD aggregates, Scene Change heatmaps, the prompt's default
Combined weights (motion `0.50`, similarity difference `0.30`, scene change
`0.20`), bounded scene/duplicate/repeated/freeze detection, automatic scene
markers and navigation, threshold-only reanalysis, seekable result lists, and
background-priority scene thumbnails. Detailed inspection, audio rendering/A-V
sync, and export were outside the Phase 8 boundary.

Phase 9 adds the dockable Frame Inspector, exact previous/next navigation,
Fit/100%/200%/400% image zoom, paused-frame X/Y RGB and display-derived YUV
inspection, 2x/4x/8x/16x nearest-neighbor magnification with high-zoom grid,
stable A/B capture, seven viewport comparison modes, 8x8 luminance SSIM, RGB
PSNR/MSE, cancellable difference rendering, and newest-generation delivery.

Phase 10 adds full-resolution current/previous/next images, decoded selected
and every-N ranges, keyframe filtering, detected-scene and high-motion targets,
PNG/JPEG/WebP/BMP/TIFF atomic writes, four contact-sheet sources, preset/custom
grids, optional labels, non-modal progress, cancellation, and media-generation
rejection. Frame sequences stream one converted image at a time, contact sheets
are capped at 1,024 cells and 64 megapixels, and batch output has a 100,000-file
safety limit. Audio rendering/A-V sync, measured optimization, and later
professional features remain later phases.
