# Phase 9 final review

Phase 9 completes VidScope's detailed frame, pixel, and A/B comparison
workflow. Inspection operates on the exact frame published by playback and its
already converted display image; it does not introduce a second decoder,
nominal-frame-rate navigation, or synchronous full-frame mouse readback.

## Frame Inspector

- The dockable panel shows frame index, normalized timestamp, PTS, DTS,
  duration, picture type, keyframe state, pixel format, resolution, bit depth,
  color range, matrix, primaries, transfer, HDR mastering/content-light
  metadata, motion, similarity, and scene score.
- Previous/Next buttons use `PlaybackController` exact presentation-order
  stepping. Double-clicking a filmstrip cell pauses, seeks, and raises the
  inspector.
- Viewport zoom supports Fit, 100%, 200%, and 400%, centered without changing
  timeline or playback coordinates.
- Missing timing, color, HDR, or analysis values remain explicit; the inspector
  does not fabricate metadata.

## Pixel inspection

- Pixel inspection is enabled only for a stable paused display frame and is
  disabled while a complete A/B comparison is active.
- Pointer motion is mapped through the viewport's displayed-image rectangle and
  reads one pixel from the current `QImage`. No full-frame conversion or GPU
  readback occurs per mouse move.
- The readout exposes X/Y, RGB, and display-derived YUV using the frame's
  selected matrix coefficients.
- The magnifier offers 2x, 4x, 8x, and 16x nearest-neighbor views. At 8x and
  16x it adds a pixel grid and highlights the selected sample.

## A/B comparison

- Set Frame A/B captures stable immutable frame identities and implicitly
  shared display images. Menu actions use `Ctrl+Shift+A` and
  `Ctrl+Shift+B`.
- The viewport supports Side by side, Overlay, Wipe, Blink, Absolute
  difference, Amplified difference (4x), and SSIM map modes.
- `FrameComparison` reports RGB MSE/PSNR and an 8x8 luminance SSIM score.
  Difference and SSIM-map surfaces are generated only when their modes need
  them. Mixed dimensions are rejected explicitly rather than silently scaled.
- `FrameComparisonManager` owns one coalescing `std::jthread`. A new
  request cancels the active generation, replaces pending work, and prevents
  stale GUI delivery.

## Acceptance coverage

`tests/unit/FrameComparisonTests.cpp` covers identical and maximally
different metrics, bounded SSIM-map output, mixed-dimension rejection, and
pre-requested cancellation.

`tests/integration/Phase9InspectionTests.cpp` covers:

- newest-generation-only asynchronous comparison delivery;
- all 18 metadata rows, analysis scores, paused pixel mapping, magnification
  choices, and previous/next signals;
- A/B capture, SSIM/PSNR publication, derived-difference mode switching, and
  comparison clearing;
- real FFmpeg frames flowing through MainWindow actions, viewport comparison,
  image zoom, dock visibility, and paused pixel availability.

The application version comes from the CMake project and is displayed by
`Help > About VidScope`. This milestone is `0.9.0`.

The completion gate built every target with MSVC `/W4` and passed all 18
CTest entries in both Debug and Release. The unit executable contains 66 cases;
the dedicated Phase 9 executable adds three asynchronous, widget, and real-media
integration scenarios.

## Explicitly deferred after Phase 9

- export workflows and formats;
- measured performance optimization after profiling;
- audio rendering and A/V synchronization;
- later professional features beyond the supplied phased plan.
