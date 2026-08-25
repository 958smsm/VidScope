# VidScope

VidScope is a C++20/Qt 6 desktop application foundation for frame-accurate
video inspection. It integrates FFmpeg directly, keeping presentation
timestamps, B-frame reordering, VFR navigation, seek behavior, decoded-frame
ownership, timeline coordinates, thumbnail generation, and cache policy under
application control.

The implemented milestone is Phase 0-10. It includes media opening, direct
demux/decode, optional hardware decoding with CPU fallback, bounded frame
history and forward queues, cancellable keyframe-based seek, exact
next/previous and signed N-frame navigation, keyframe navigation,
HDR10/color/frame metadata, deterministic shutdown, a custom zoomable timeline,
asynchronous decoded hover previews, and a configurable preview filmstrip. Hover
and filmstrip work run through one bounded priority scheduler and reusable FFmpeg
worker pool separate from playback. The latest interactive request supersedes
stale work; filmstrip batches reject superseded deliveries, use bounded memory
and disk caches, retry still-current cells after bounded-scheduler preemption,
and never create one QWidget or decoder per frame. A separate progressive
analysis worker decodes presentation-order frames into bounded 160x90 luma
planes, computes raw motion, similarity, scene-change, duplicate, and stable
fingerprint data, persists a versioned cache, and yields to playback and active
scrubbing. A bounded 4x temporal LOD pyramid incrementally aggregates those raw
scores, and the custom timeline renders Motion, Similarity, Scene Change, or
configurable Combined heatmaps without painting one primitive per frame at
overview zoom. Phase 8 derives bounded scene, exact/near duplicate,
repeated-section, and freeze results from the compact samples without another
decode pass. Phase 9 adds a dockable Frame Inspector with exact decoded-frame,
color, HDR, and analysis metadata; paused-frame pixel inspection and nearest-
neighbor magnification; image zoom; and captured A/B frames. A cancellable
coalescing worker computes SSIM, PSNR, absolute/amplified differences, and SSIM
maps while the viewport provides side-by-side, overlay, wipe, and blink modes.
Phase 10 adds a cancellable full-resolution export worker for current,
previous, next, selected/every-N range, keyframe, scene, and high-motion frame
output. It atomically writes PNG, JPEG, WebP, BMP, and TIFF images and creates
bounded contact sheets from entire, visible, selected, or detected-scene
sources without accumulating decoded frame sequences in memory.

## Prerequisites

- CMake 3.25+
- C++20 compiler (MSVC 2022-compatible, Clang, or GCC)
- **Qt 6.11.2 exactly** (`Core`, `Gui`, `Widgets`, and `Test` when tests are enabled)
- shared FFmpeg development SDK with libavformat, libavcodec, libavutil,
  libswscale, and libswresample
- FFmpeg CLI for generated integration fixtures

## Build on the configured Windows workstation

The enhanced wrapper is useful with no arguments: it selects the Release preset,
auto-loads MSVC when needed, configures, builds, repairs Qt/FFmpeg deployment,
runs the startup probe, and then runs CTest.

```powershell
.\tools\build_windows.ps1
```

Debug remains explicit:

```powershell
.\tools\build_windows.ps1 -c Debug
```

The checked-in Windows preset expects Qt 6.11.2 and the FFmpeg SDK under
`D:\dev`. Run it from an MSVC developer shell. On another machine, configure
directly with the local roots:

```powershell
cmake -S . -B build -DCMAKE_PREFIX_PATH=C:\Qt\6.11.2\msvc2022_64 `
  -DFFMPEG_ROOT=C:\ffmpeg-shared
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

CMake uses an exact Qt package requirement, so configuration fails rather than
silently building against an older or newer Qt patch release.

## Controls

Playback and navigation:

- `Ctrl+O`: open media
- `Space`: play/pause; `K`: pause; `S`: stop
- `Left` / `,` / `J`: exact previous presentation frame
- `Right` / `.` / `L`: exact next presentation frame
- frame-step selector: presets 1, 2, 5, 10, or custom 1-1000
- `Shift+Left` / `Shift+Right`: exact backward/forward selected-N jump
- `Ctrl+Left` / `Ctrl+Right`: previous/next decoded keyframe
- `Alt+Left` / `Alt+Right`: previous/next available scene marker

Timeline:

- left-click or left-drag: seek/scrub; repeated identical drag positions are
  coalesced
- middle-drag: pan the visible time window without seeking
- wheel: zoom around the cursor timestamp
- `Shift+wheel`: horizontal pan
- `Shift+left-drag`: create a normalized range; drag near an existing endpoint
  to resize it
- click a marker: activate it and seek to its exact timestamp
- hover: show an asynchronously decoded preview at the nearest exact
  presentation frame; the popup follows the cursor and remains inside the
  application/screen geometry
- `I` / `O`: set In/Out at the playhead; `M`: toggle a bookmark
- `Ctrl+Shift+X`: clear selection; `Ctrl+0`: show the entire video
- configured Zoom In/Out shortcuts: zoom around the visible playhead, or the
  viewport center when the playhead is outside it
- **Analysis > Motion / Similarity / Scene Change / Combined**: switch the
  timeline heatmap;
  the selected mode persists across sessions
- the heatmap selects a temporal LOD level from the current pixel density,
  preserves unknown analysis gaps, and refines progressively as batches arrive
- the dockable **Analysis Results** panel lists detected scenes, duplicate and
  repeated ranges, and freezes; clicking any result seeks to its timestamp
- scene list entries receive background-priority thumbnails through the shared
  bounded thumbnail service
- scene, near-duplicate, freeze-similarity, and minimum-freeze controls can be
  changed and applied with **Reanalyze Detections** without decoding the video
  again

Filmstrip:

- count presets: 8, 16, 20, and 32, plus custom 1-64
- modes: Entire Video, Around Current Position, Visible Timeline, and Selected Range
- filmstrip work is asynchronous and does not share the playback decoder
- bounded-scheduler preemption is reported and retried only for the current batch
- click a thumbnail to seek to its decoded presentation timestamp
- double-click pauses, seeks to the selected frame, and raises the Frame Inspector
- the selected mode and count persist through `QSettings`

Inspection:

- the dockable **Frame Inspector** displays frame index, timestamp, PTS, DTS,
  duration, frame type, keyframe state, pixel format, resolution, bit depth,
  color range/matrix/primaries/transfer, HDR metadata, motion, similarity, and
  scene score
- **Previous Frame** and **Next Frame** use the exact presentation-order
  navigation path; image zoom provides Fit, 100%, 200%, and 400%
- paused-frame pixel inspection reports X/Y, RGB, and display-derived YUV at
  2x, 4x, 8x, or 16x; the magnifier uses nearest-neighbor pixels and shows a
  grid at 8x and 16x
- **Set Frame A** and **Set Frame B**, also available as
  `Ctrl+Shift+A` / `Ctrl+Shift+B`, capture stable display images
- A/B display modes are Side by side, Overlay, Wipe, Blink, Absolute
  difference, Amplified difference, and SSIM map; SSIM, PSNR, and MSE are
  reported for matching frame dimensions

Export:

- **Export** provides full-resolution current, previous, and next frame saves;
  selected-range, every-N, keyframe, detected-scene, and high-motion sequences;
  contact sheets; and active-export cancellation
- frame sequences use decoded presentation order, so VFR ranges and every-N
  selection never use `duration * nominal FPS` estimates
- frame formats are PNG, JPEG, WebP, BMP, and TIFF where the deployed Qt image
  plugin supports them; contact sheets use PNG, JPEG, or WebP
- contact sheets support Entire Video, Visible Timeline, Selected Range, and
  Detected Scenes; presets are 8, 16, 20, and 25 frames plus custom rows and
  columns, with optional timestamp and exact-index-when-known labels
- exports run on a separate cancellable software-decoding worker, stream each
  full-resolution frame directly to an atomic output file, and show non-modal
  progress; `Ctrl+Shift+S` saves the current frame and
  `Ctrl+Alt+C` opens contact-sheet creation

Application shortcuts can be remapped through **Settings > Keyboard
Shortcuts**.

The hover popup exposes exact decoded timestamp, established frame index when
known, frame type, keyframe state, cache source, and real Phase 6 motion and
similarity scores as soon as the corresponding frame has been analyzed. The
filmstrip uses the same raw samples and updates already-visible cells as
analysis batches arrive; no placeholder values are fabricated.

## Documentation and scope

- [Phase 0 assessment](docs/phase-0-assessment.md)
- [Architecture](docs/architecture.md)
- [Phase 1 final review](docs/phase-1-review.md)
- [Phase 2 final review](docs/phase-2-review.md)
- [Phase 3 final review](docs/phase-3-review.md)
- [Phase 4 final review](docs/phase-4-review.md)
- [Phase 5 final review](docs/phase-5-review.md)
- [Phase 6 final review](docs/phase-6-review.md)
- [Phase 7 final review](docs/phase-7-review.md)
- [Phase 8 final review](docs/phase-8-review.md)
- [Phase 9 final review](docs/phase-9-review.md)
- [Phase 10 final review](docs/phase-10-review.md)

Phase 10 is the completed export milestone. `ExportManager` owns a bounded,
thread-confined decoder/converter, `ExportPlanner` normalizes ranges and hard
limits, and MainWindow supplies timeline, detection, and analysis targets.
Measured optimization, audio rendering/A-V sync, and later professional
features remain later phases.

The application version is set from the CMake project version and is visible in
`Help > About VidScope`. Phase 10 reports version `0.10.0`.

Known timeline frame boundaries and keyframe ticks still grow only from exact
frames published by the playback engine; opening a video does not force a
hidden full-file indexing or thumbnail pass.

## FFmpeg licensing

VidScope itself has not been assigned a distribution license in this
workspace. The detected FFmpeg binary is GPLv3-enabled. Before distributing
binaries, select a project license and ensure the chosen FFmpeg configuration
and linked codec libraries are license-compatible.
