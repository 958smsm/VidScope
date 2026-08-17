# VidScope

VidScope is a C++20/Qt 6 desktop foundation for frame-accurate video
inspection. It integrates FFmpeg directly, keeping presentation timestamps,
B-frame reordering, VFR navigation, seek behavior, decoded-frame ownership,
timeline coordinates, and cache policy under application control.

The implemented milestone is Phase 0-3. It includes media opening, direct
demux/decode, optional hardware decoding with CPU fallback, bounded frame
history and forward queues, cancellable keyframe-based seek, exact
next/previous and signed N-frame navigation, keyframe navigation,
HDR10/color/frame metadata, deterministic shutdown, and a custom zoomable
timeline. The timeline provides nanosecond-safe timestamp mapping, clamped
viewport zoom and pan, playhead and ruler ticks, exact observed VFR frame
boundaries, derived decoded-keyframe ticks, bounded
keyframe/scene/chapter/bookmark marker storage, In/Out selection, hover
coordinates, and cancellable seek/scrub
integration.

## Prerequisites

- CMake 3.25+
- C++20 compiler (MSVC 2022-compatible, Clang, or GCC)
- Qt 6.6+ (`Core`, `Gui`, `Widgets`, and `Test` when tests are enabled)
- shared FFmpeg development SDK with libavformat, libavcodec, libavutil,
  libswscale, and libswresample
- FFmpeg CLI for generated integration fixtures

## Build on this workstation

```powershell
& 'E:\Program Files\Microsoft Visual Studio\18\Enterprise\Common7\Tools\Launch-VsDevShell.ps1' `
  -Arch amd64 -SkipAutomaticLocation
cmake --preset windows-msvc
cmake --build --preset windows-debug
ctest --preset windows-debug
cmake --build --preset windows-release
ctest --preset windows-release
```

The checked-in Windows preset uses the detected SDKs under `D:\dev`. Run it
from an MSVC developer shell. On another machine, configure directly with the
local Qt and FFmpeg roots:

```powershell
cmake -S . -B build -DCMAKE_PREFIX_PATH=C:\Qt\6.10.0\msvc2022_64 `
  -DFFMPEG_ROOT=C:\ffmpeg-shared
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

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
- hover: report timestamp and nearest established presentation index; decoded
  preview imagery is deliberately deferred
- `I` / `O`: set In/Out at the playhead; `M`: toggle a bookmark
- `Ctrl+Shift+X`: clear selection; `Ctrl+0`: show the entire video
- configured Zoom In/Out shortcuts: zoom around the visible playhead, or the
  viewport center when the playhead is outside it

Application shortcuts can be remapped through **Settings > Keyboard
Shortcuts**.

## Documentation and scope

- [Phase 0 assessment](docs/phase-0-assessment.md)
- [Architecture](docs/architecture.md)
- [Phase 1 final review](docs/phase-1-review.md)
- [Phase 2 final review](docs/phase-2-review.md)
- [Phase 3 final review](docs/phase-3-review.md)

Phase 3 is the completed custom-timeline foundation. Asynchronous hover-frame
popups, thumbnail scheduling/cache, preview filmstrips, motion/similarity
analysis and heatmaps, automatic scene and duplicate/freeze detection, audio
rendering/A-V sync, detailed inspection, and export remain Phase 4 or later.
Known frame boundaries and keyframe ticks grow only from exact frames published
by the playback engine; Phase 3 does not perform a hidden full-file indexing
pass. The current timeline exposes extension points for later systems without
fake preview or analysis data.

## FFmpeg licensing

VidScope itself has not been assigned a distribution license in this
workspace. The detected FFmpeg binary is GPLv3-enabled. Before distributing
binaries, select a project license and ensure the chosen FFmpeg configuration
and linked codec libraries are license-compatible.
