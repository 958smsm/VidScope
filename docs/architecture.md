# VidScope Phase 0-3 architecture

VidScope is a C++20, Qt 6, direct-FFmpeg video inspection application. Phase 3
adds the custom timeline and its interaction model without weakening the
frame-accurate playback, timestamp, ownership, or thread boundaries established
in Phase 2.

## Dependency direction

```text
MainWindow / TimelineWidget / VideoViewport
        |                |
        |                +-> TimelineModel (GUI-owned state and transforms)
        |
PlaybackController (Qt adapter, GUI-thread API)
        |
PlaybackSession (single decode-worker confinement)
        |
MediaSource -> Demuxer -> VideoDecoder
        |
      FFmpeg
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

Phase 3 includes the custom painted timeline, timestamp mapping, viewport,
anchor zoom, horizontal pan, playhead, seek/scrub interaction, hover
coordinates, timestamp and exact published-frame ticks, bounded marker layers,
In/Out selection infrastructure, remappable commands, and tests. Frame and
keyframe coverage grows from exact frames delivered to the GUI; Phase 3 does
not perform a full-file pre-index. Its timeline also currently requires a
positive declared media duration rather than growing an initially unknown
extent.

Phase 4+ owns decoded hover-preview scheduling and popup imagery, thumbnail
workers and caches, filmstrips, progressive analysis, motion/similarity/scene
heatmaps, automatic scene and duplicate/freeze detection, selection consumers
such as loop/analyze/export/contact sheet, inspection, and export. No fake
thumbnail, preview, scene, or analysis values are generated by Phase 3.
