# Phase 12 final review

Phase 12 turns the stable inspection core into a more complete professional
workflow without weakening frame accuracy or adding unbounded state. The
milestone implements the features that fit the current video-analysis
architecture and explicitly records which candidates require a future media
pipeline or a justified extension contract.

## Implemented professional features

- FFmpeg container chapters are converted to application-relative nanoseconds,
  retain their titles and boundaries, appear as chapter markers, and are
  navigable with previous/next chapter actions.
- Timeline markers now retain a stable type, category, label, and free-form
  note. The marker editor can add or update those fields while the Professional
  Tools dock lists, seeks, edits, and deletes the same authoritative objects.
- A bounded 128-entry browser-style history records recently inspected frame
  identity and exact timestamp. Back/forward navigation preserves a cursor and
  discards stale forward history after a new branch.
- The existing thumbnail-backed Scenes results tab is the scene browser.
  Visual Search adds a separate seekable result list ranked by 64-bit
  perceptual-hash Hamming distance, then temporal proximity. It searches compact
  analysis samples and caps output at 48 entries.
- Playback diagnostics expose decoder FPS, last seek latency, decoded-frame and
  command queue depths, cache entries/bytes/hit rate/evictions, GUI deliveries,
  coalesced dropped deliveries, and active hardware decode device. Unsupported
  GPU utilization remains explicitly unavailable.

All new collections are bounded. Chapter markers share the 10,000-marker
ceiling, history stores 128 compact entries, visual search scans at most the
requested bounded analysis snapshot and retains only its capped result set, and
diagnostics are value snapshots attached to the already coalesced frame
delivery.

## Correctness and UX

Chapter and marker navigation use the timeline's sorted marker model, so the
visible line and navigation destination cannot diverge. Inspection history
stores exact decoded identities but navigates through the normal timestamp seek
path. Visual search omits the query frame and never fabricates hashes for
unanalyzed frames.

The Professional Tools dock is tabified with Analysis Results and Frame
Inspector. It can be shown from View, and all Phase 12 actions participate in
the existing context-aware enablement and remappable shortcut system.

The application version continues to come from CMake and is visible in
`Help > About VidScope`; this milestone is `0.12.0`.

## Acceptance coverage

- Unit tests cover bounded branching frame history, deterministic visual-search
  ranking, and marker category/note persistence through updates.
- The generated media suite includes a two-chapter Matroska fixture and verifies
  title and exact boundary extraction.
- Phase 12 UI integration coverage verifies the Professional Tools histories,
  markers, diagnostics, actions, and automatic chapter marker installation.
- The full Debug and Release test suites remain the completion gate.

The completion gate builds every target with MSVC `/W4`. Both configurations
pass all 21 CTest entries, and the unit executable passes 74/74 cases.

## Considered and intentionally deferred

- Audio waveform needs timestamped audio demux, decode, resampling, multichannel
  reduction, caching, and zoom-level aggregation. VidScope currently has no
  audio playback pipeline, so a waveform-only shortcut would create a second
  incomplete timing architecture.
- Subtitle tracks and subtitle search need packet decode for text/bitmap/ASS
  variants, encoding and style normalization, track selection, bounded indexing,
  and timestamp-correct rendering.
- A public plugin architecture needs a demonstrated extension point before its
  ABI, version negotiation, ownership, cancellation, thread affinity, sandbox,
  and compatibility guarantees can be designed responsibly.

Those items are future dedicated phases rather than partially implemented
checkboxes in Phase 12.
