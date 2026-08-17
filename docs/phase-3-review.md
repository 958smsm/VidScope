# Phase 3 final review

Phase 3 delivers the custom timeline foundation on top of the Phase 2
frame-accurate engine. The timeline is a custom-painted widget backed by a
separate, independently testable coordinate/state model; it is not a styled
`QSlider` and does not create a widget per frame.

## Delivered architecture

- `TimelineModel` owns duration, playhead, viewport, coordinate transforms,
  exact observed frame boundaries, editable markers, and selection state.
- `TimelineWidget` owns pointer interaction and bounded painting. It emits
  seek/scrub/hover/viewport/selection/marker signals and contains no decode
  path.
- `MainWindow` connects timeline seeks to the existing cancellable
  `PlaybackController`, publishes each delivered frame's lightweight timeline
  metadata, and owns Timeline and shortcut-editor actions.
- `vidscope_engine` contains the model; `vidscope_ui` contains the widget,
  viewport, main window, and shortcut editor. Pure model tests do not construct
  a QWidget.

The model is GUI-thread-owned. Decoder work remains confined to the playback
worker, and Qt widgets are updated only on the GUI thread.

## Interaction contract

| Input | Result |
|---|---|
| Left click or drag | Clamped precise seek/scrub; identical drag targets coalesce |
| Middle drag | Pan the visible window without seeking |
| Wheel | Zoom around the cursor timestamp |
| `Shift+wheel` | Horizontal pan |
| `Shift+left-drag` | Create a normalized selection |
| `Shift+left-drag` near a handle | Resize that selection endpoint |
| Marker click | Emit one exact seek and a separate activation signal |
| Hover | Emit timestamp and nearest known presentation index |
| `I` / `O` | Set In/Out at the playhead |
| `M` | Toggle a bookmark at the playhead |
| `Ctrl+Shift+X` | Clear selection |
| Configured Zoom In/Out actions | Anchor-aware zoom |
| `Ctrl+0` | Show the entire video |

Playback, frame-step, N-frame, keyframe, and available-scene-marker actions
remain synchronized with the timeline. Application actions are remappable in
the shortcut editor; the timeline widget does not retain hidden hard-coded
duplicates of those command shortcuts.

Gestures accept only the intended held mouse button and use one cancellation
path. Focus loss cancels an active scrub, pan, or selection gesture. A
user-initiated media open immediately resets and disables the old timeline,
ignores stale old-media frame deliveries, and re-enables controls only after
the new open succeeds or reports an error.

## Coordinate semantics

- `media::MediaTime` nanoseconds are authoritative.
- Duration, playhead, viewport, markers, selection, and pixel-derived time are
  clamped to `[0, duration]`.
- Reversed viewport and selection endpoints are normalized.
- Viewports preserve at least `1,000 ns` unless total media is shorter.
- `timeToPixel` subtracts integer viewport-relative nanoseconds before scaling,
  preserving single-nanosecond distinctions at large absolute offsets.
- Anchor scaling uses integer rescaling, including timestamps beyond the exact
  integer range of a double.
- `pixelToTime` clamps pixels to the visible interval and rounds once back to
  integral nanoseconds.
- Zoom factors greater than one zoom in and preserve the anchor's relative
  pixel coordinate unless an edge clamp makes that impossible.
- Pan preserves viewport duration and cannot leave media extent.
- Major tick intervals are 1/2/5 multiples of powers of ten, aligned to
  application time zero, pixel-density-aware, and bounded by a maximum count.
- Visible frame and marker queries are closed ranges: objects exactly at both
  viewport endpoints are included.

## Exact VFR and frame rules

- `observeFrame` copies the actual decoded `FrameId`, presentation time,
  duration, and keyframe flag. It does not synthesize a nominal duration.
- An established nonnegative presentation index is the logical frame identity
  across seek/redecode serials. Re-observation updates the existing boundary
  instead of duplicating it.
- Observations are ordered by presentation time and then stable frame identity,
  so reverse insertion and equal-time distinct frames remain deterministic.
- Equal-time collision groups remain available to the paint query; a zero gap
  does not blank the entire visible frame layer.
- Frame ticks come only from observed boundaries. No `time * FPS`,
  `duration * FPS`, DTS order, or nominal constant-frame-duration assumption
  is used.
- Frame ticks are emitted only when distinct adjacent timestamps meet the
  visible pixel-spacing requirement and the primitive ceiling.
- Selection details identify the first and last observed frames in the closed
  range. `knownFrameCount` always describes actual observations.
  `frameCount` is present only when presentation indices are contiguous and
  both selection endpoints exactly match the first and last observed
  timestamps.

## Marker and selection rules

- Marker storage accepts keyframe, scene, chapter, and bookmark kinds. Current
  commands directly edit bookmarks.
- Decoded-frame keyframe hits/ticks are an independent derived layer and do not
  consume the stored-marker capacity.
- Stored marker time is clamped and storage is ordered by `(time, id)`. A
  surviving marker keeps its ID across movement and re-sorting; deleted,
  cleared, and reset IDs are never reassigned to another marker.
- Coincident editable markers are hit-tested in reverse paint order before a
  derived keyframe, so the topmost visible editable marker wins.
- Bookmark add/remove is available at the playhead.
- In/Out commands and forward/reverse drags produce the same normalized range.
- A selection wholly outside the viewport paints no false edge sliver.
- Scene marker display and navigation are implemented for supplied marker data;
  automatic scene detection is not part of Phase 3.

## Bounds and threading review

- Known frame boundaries have a hard configurable cap (default 100,000).
- The model uses deque storage and a presentation-index location map. Monotonic
  append, indexed upsert/lookup, and endpoint eviction avoid repeated
  front-vector shifts.
- Stored markers have a hard configurable cap (default 10,000); add fails
  cleanly at capacity rather than silently discarding a user marker.
- Frame eviction removes the temporal endpoint farthest from the playhead and
  retains sorted presentation order.
- Visible frame painting is capped at 4,096 primitives and gated by pixel
  spacing. Ruler ticks have their own 2,048 default cap.
- The timeline stores metadata only, not `AVFrame` or `QImage` surfaces.
- No decode, seek, thumbnail, or analysis work runs in paint or mouse handlers.
- Existing request generations, bounded controller queue, newest-seek
  coalescing, cancellation, GUI-delivery epoch, and deterministic worker join
  remain the seek/thread-safety boundary.

## Acceptance coverage

`tests/unit/TimelineModelTests.cpp` covers coordinate/viewport normalization,
anchor zoom, 1/2/5 ruler intervals, 100-day and greater-than-`2^53`
timestamp math, cross-serial logical-frame upsert, conflicting identity/time
ordering, equal-time collision groups, exact capacity eviction, closed
endpoints, marker identity/capacity, and strict selection coverage/cache
invalidation.

`tests/integration/TimelineWidgetTests.cpp` covers click/drag scrub signals,
one-seek-per-click behavior, scrub-time position isolation, wheel zoom,
middle-button pan, forward/reverse selection, exact marker activation,
zero-/multi-hour painting, reset, and bounded repeated input.

`tests/integration/TimelineWidgetPrecisionTests.cpp` covers off-center 25%/75%
cursor anchors, exact bidirectional pan and edge clamps, Shift-wheel pan,
multiple/coincident/outside marker hits, and sentinel-image paint evidence.

`tests/integration/TimelineMediaIntegrationTests.cpp` exhaustively ingests the
generated VFR fixture's decoded presentation boundaries, durations, and
keyframes; verifies deterministic reverse insertion for media containing
B-frames; and checks a seven-entry model bound across 25,000 observations.
Phase 2's decoder integration remains the evidence for B-frame presentation
decode itself.

`tests/integration/TimelineEndToEndTests.cpp` performs a software reference
decode of the real VFR fixture, sends timeline clicks through `MainWindow` and
`PlaybackController`, and verifies exact at-or-after presentation index/time
delivery. Its rapid A-to-B-to-C case verifies that no stale A/B delivery
survives once the newest C seek supersedes them.

`tests/integration/Phase3TimelineStressTests.cpp` exercises real-media rapid
scrub/zoom/pan, destruction with queued work, and repeated main-window
lifecycles.

Final verification on this workstation:

- MSVC Debug build: passed
- MSVC Release build: passed
- Debug CTest: 12/12 gates passed
- Release CTest: 12/12 gates passed
- unit executable: 35/35 cases in both configurations
- timeline widget: 9/9; precision: 5/5; media: 3/3
- Phase 3 stress: 2/2; timeline end-to-end: 1/1
- Debug repeated gate: all eight selected targets passed 20 consecutive runs
  each (160/160 executions)

## Explicitly deferred after Phase 3

- Phase 4: asynchronous decoded hover-frame scheduler, cancellation/cache,
  cursor-following popup, and stale-result prevention
- Phase 5: configurable preview filmstrip and thumbnail modes
- Phase 6-7: progressive motion/similarity/scene analysis, cache, LOD, and
  timeline heatmaps
- Phase 8: automatic scene detection, scene list/thumbnails, duplicate and
  freeze detection
- Later phases: selection loop/analyze/export/contact-sheet consumers, frame
  inspector, comparison, audio rendering/A-V sync, and export

Known frame/keyframe coverage grows only from exact frames published to the GUI;
Phase 3 does not run a full-file pre-index. The current timeline also requires a
positive declared duration instead of growing an initially unknown media
extent. The hover signal, marker kinds, viewport resolution, and normalized
selection are extension points for later systems; Phase 3 does not fabricate
preview thumbnails, heatmaps, scene scores, or analysis results.
