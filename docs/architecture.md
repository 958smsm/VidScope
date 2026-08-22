# VidScope Phase 0-5 architecture

VidScope is a C++20, Qt 6.11.2, direct-FFmpeg video inspection application.
Phase 5 adds configurable asynchronous preview filmstrips on top of the Phase 4
thumbnail subsystem without weakening the frame-accurate playback, timestamp,
ownership, timeline, cancellation, or thread boundaries established earlier.

## Dependency direction

```text
MainWindow / TimelineWidget / VideoViewport / HoverPreviewPopup / FilmstripWidget
        |                |                    |                    |
        |                |                    +-> HoverPreviewController
        |                |                                         |
        |                +-> TimelineModel <- FilmstripModel        |
        |                    (GUI-owned state)        |              |
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
  coordination and thumbnail workers before the playback controller.

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
externally supplied markers, while automatic detection remains Phase 8.

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

The decoder tries D3D11VA then DXVA2 on Windows and transfers hardware surfaces
to system memory for the current renderer. Device/configuration/open failures
retry the CPU decoder, so correctness does not depend on GPU availability.

## Phase boundary

Phase 5 includes the custom timeline, asynchronous decoded hover previews, and
a configurable filmstrip with 8/16/20/32/custom counts and Entire Video, Around
Current Position, Visible Timeline, and Selected Range policies. Filmstrip
planning is independently testable, timestamp-authoritative, and bounded to 64
cells. One controller batch generation plus per-request generations prevents a
stale mode/count/range result from mutating the current strip.

The hover popup and filmstrip share `ThumbnailManager`, but not the playback
decoder. Hover remains higher priority; visible-strip and near-playhead lanes can
be cancelled independently. The default pending queue is bounded at 96, leaving
headroom above the 64-cell filmstrip cap. Tighter configurations and priority
preemption emit cancellation completion so only still-current loading cells are
retried. Memory/disk caches and reusable worker sessions are shared, so Phase 5
adds no decoder-per-thumbnail path.

The filmstrip exposes real decoded timestamp, presentation index when
established, PTS/DTS, picture type, and keyframe state. It leaves motion and
similarity absent until Phase 6. A double-click integration signal is present,
but the complete Frame Inspector remains Phase 9.

Phase 6+ owns progressive analysis, motion/similarity/scene heatmaps and LOD,
automatic scene/duplicate/freeze detection, detailed inspection, audio
rendering/A-V sync, and export.
