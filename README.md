# VidScope

VidScope is a C++20/Qt 6 desktop application foundation for frame-accurate
video inspection. It integrates FFmpeg directly, keeping presentation
timestamps, B-frame reordering, VFR navigation, seek behavior, decoded-frame
ownership, timeline coordinates, thumbnail generation, and cache policy under
application control.

The implemented milestone is Phase 0-4. It includes media opening, direct
demux/decode, optional hardware decoding with CPU fallback, bounded frame
history and forward queues, cancellable keyframe-based seek, exact
next/previous and signed N-frame navigation, keyframe navigation,
HDR10/color/frame metadata, deterministic shutdown, a custom zoomable timeline,
and asynchronous decoded hover previews. Hover work runs through a bounded
priority scheduler and reusable FFmpeg worker pool separate from playback. The
latest cursor request supersedes stale work; results use bounded memory and disk
caches and are delivered only when their media epoch and request generation are
still current.

## Prerequisites

- CMake 3.25+
- C++20 compiler (MSVC 2022-compatible, Clang, or GCC)
- **Qt 6.11.2 exactly** (`Core`, `Gui`, `Widgets`, and `Test` when tests are enabled)
- shared FFmpeg development SDK with libavformat, libavcodec, libavutil,
  libswscale, and libswresample
- FFmpeg CLI for generated integration fixtures

## Build on the configured Windows workstation

```powershell
& 'E:\Program Files\Microsoft Visual Studio\18\Enterprise\Common7\Tools\Launch-VsDevShell.ps1' `
  -Arch amd64 -SkipAutomaticLocation
cmake --preset windows-msvc
cmake --build --preset windows-debug
ctest --preset windows-debug
cmake --build --preset windows-release
ctest --preset windows-release
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

Application shortcuts can be remapped through **Settings > Keyboard
Shortcuts**.

The Phase 4 popup exposes exact decoded timestamp, established frame index when
known, frame type, keyframe state, and cache source. Motion and similarity are
shown as **not analyzed** until the Phase 6 analysis pipeline supplies real
scores; no placeholder values are fabricated.

## Documentation and scope

- [Phase 0 assessment](docs/phase-0-assessment.md)
- [Architecture](docs/architecture.md)
- [Phase 1 final review](docs/phase-1-review.md)
- [Phase 2 final review](docs/phase-2-review.md)
- [Phase 3 final review](docs/phase-3-review.md)
- [Phase 4 final review](docs/phase-4-review.md)

Phase 4 is the completed hover-preview foundation. The preview manager is also
an extension point for Phase 5 filmstrips: it already supports priority lanes,
request generations, reusable workers, pending duplicate coalescing, bounded
memory/disk caching, and exact target metadata. Configurable filmstrip modes,
progressive motion/similarity analysis and heatmaps, automatic scene and
duplicate/freeze detection, audio rendering/A-V sync, detailed inspection, and
export remain later phases.

Known timeline frame boundaries and keyframe ticks still grow only from exact
frames published by the playback engine; opening a video does not force a
hidden full-file indexing or thumbnail pass.

## FFmpeg licensing

VidScope itself has not been assigned a distribution license in this
workspace. The detected FFmpeg binary is GPLv3-enabled. Before distributing
binaries, select a project license and ensure the chosen FFmpeg configuration
and linked codec libraries are license-compatible.
