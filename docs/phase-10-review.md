# Phase 10 final review

Phase 10 completes VidScope's still-frame, filtered-range, and contact-sheet
export workflow. Export uses a dedicated full-resolution decoder/converter and
does not reuse timeline or filmstrip thumbnails.

## Export modes

- Save Current, Previous, and Next Frame use the paused playback timestamp and
  exact presentation-order navigation.
- Export Selected Frames decodes every actual frame in the normalized timeline
  range. Export Every N Frames counts decoded presentation frames, so VFR media
  never falls back to duration multiplied by nominal FPS.
- Export Keyframes filters decoded keyframe metadata.
- Export Scene Frames uses current bounded scene detections.
- Export High-Motion Frames uses current raw analysis samples and a configurable
  normalized threshold.
- Formats are PNG, JPEG, WebP, BMP, and TIFF where supported by the deployed Qt
  plugins. Every still image is converted at source dimensions.

## Contact sheets

- Sources are Entire Video, Visible Timeline, Selected Range, and Detected
  Scenes.
- Presets provide 8, 16, 20, and 25 frames; custom rows and columns are bounded
  to 1,024 cells.
- Timestamp and exact frame-index-when-known labels can be enabled independently.
- PNG, JPEG, and WebP output is available.
- Contact-sheet allocation is rejected above 64 megapixels. Frames are decoded,
  painted, and released one at a time rather than retained as a batch.

## Threading, cancellation, and output safety

`ExportManager` owns one `std::jthread`, one software
`PlaybackSession`, and one `FrameConverter`. It permits one pending or
active job, rejects parallel requests, uses generation checks when media
changes, and joins before destruction.

Frame sequences are capped at 100,000 outputs and stream each converted image
directly to disk. `QSaveFile` plus `QImageWriter` provides atomic
commit behavior; existing files are rejected unless the save-file workflow has
already confirmed replacement. Progress and cancellation remain non-modal.

## Acceptance coverage

`tests/unit/ExportTests.cpp` covers normalized ranges, hard limits, exact
integer sampling, preset/custom contact-sheet geometry, filename sanitization,
and all Phase 10 formats.

`tests/integration/Phase10ExportTests.cpp` covers:

- atomic full-resolution current, previous, and next frame export;
- overwrite rejection without partial files;
- decoded-range stride, keyframe, and timestamp-target exports;
- PNG, JPEG, WebP, and BMP encoding at source dimensions;
- contact-sheet output geometry and bounded cell decoding;
- parallel-request rejection and cancellation;
- context-aware MainWindow export actions with real FFmpeg media.

The application version comes from the CMake project and is displayed by
`Help > About VidScope`. This milestone is `0.10.0`.

The completion gate built every target with MSVC `/W4` and passed all 19
CTest entries in both Debug and Release. The unit executable contains 70 cases;
the dedicated Phase 10 executable adds five planner, worker, cancellation,
contact-sheet, and real-media/UI scenarios.

## Explicitly deferred after Phase 10

- Phase 11 measured optimization work after profiling;
- audio rendering and A/V synchronization;
- later professional features beyond the supplied phased plan.
