# VidScope

VidScope is a C++20/Qt 6 desktop foundation for frame-accurate video
inspection. It integrates FFmpeg directly, keeping presentation timestamps,
B-frame reordering, VFR navigation, seek behavior, decoded-frame ownership,
and cache policy under application control.

The implemented milestone is Phase 0-2: media opening, direct demux/decode,
optional hardware decoding with CPU fallback, bounded frame history and forward
queues, cancellable keyframe-based seek, exact next/previous and signed N-frame
navigation, keyframe navigation, HDR10/color/frame metadata, a non-blocking Qt
controller, video viewport, logging, deterministic shutdown, and synthetic
frame-accuracy regressions.

## Prerequisites

- CMake 3.25+
- C++20 compiler (MSVC 2022-compatible, Clang, or GCC)
- Qt 6.6+ (`Core`, `Gui`, and `Widgets`)
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

- `Ctrl+O`: open media
- `Space`: play/pause
- `K`: pause
- `Left` / `,`: exact previous presentation frame
- `Right` / `.`: exact next presentation frame
- frame-step selector: presets 1, 2, 5, 10, or custom 1-1000
- `Shift+Left` / `Shift+Right`: exact backward/forward selected-N jump
- `Ctrl+Left` / `Ctrl+Right`: previous/next decoded keyframe
- click or drag the seek bar: cancellable timestamp seek

## Documentation and scope

- [Phase 0 assessment](docs/phase-0-assessment.md)
- [Architecture](docs/architecture.md)
- [Phase 2 final review](docs/phase-2-review.md)

Advanced timeline zoom, hover previews, thumbnails, analysis heatmaps, scenes,
duplicate/freeze analysis, audio rendering/A-V sync, detailed inspection, and
export deliberately remain after the Phase 2 frame-accuracy gate. They are not
represented by fake data or placeholders.

## FFmpeg licensing

VidScope itself has not been assigned a distribution license in this
workspace. The detected FFmpeg binary is GPLv3-enabled. Before distributing
binaries, select a project license and ensure the chosen FFmpeg configuration
and linked codec libraries are license-compatible.
