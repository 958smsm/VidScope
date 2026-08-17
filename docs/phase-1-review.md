# Phase 1 final review

Review date: 2026-08-17

Phase 1 is complete. The application foundation opens real media through
FFmpeg, selects and decodes a video stream outside the GUI thread, converts and
renders decoded frames, exposes transport controls and metadata, reports
failures, and shuts down deterministically.

This review was repeated after Phase 3 and closed two strict lifetime defects,
one stale-lifecycle failure path, one missing-timestamp scheduling hazard, and
several Phase 1-specific acceptance-coverage gaps.

## Requirement status

| Requirement | Implementation |
|---|---|
| Application skeleton | C++20 CMake targets split into engine, UI, executable, and tests |
| Qt main window | Professional dark main window with menus, actions, transport controls, status, viewport, and timeline |
| FFmpeg initialization | One process-lifetime, thread-safe RAII network initializer shared by startup and media opens |
| FFmpeg RAII wrappers | Unique ownership for format/codec contexts, packets, frames, swscale contexts, and buffer references |
| Media opening | Interruptible `avformat_open_input` and stream-info discovery with surfaced FFmpeg errors |
| Stream selection | `av_find_best_stream` by default and validated explicit video-stream selection |
| Video decoding | Direct send/receive decoding with EOF drain and optional hardware decode plus startup fallback |
| Basic renderer | Worker-side FFmpeg color conversion and aspect-preserving QWidget presentation |
| Play / pause / stop | Asynchronous controller commands; Stop seeks and republishes the first presentation frame |
| Simple seek | Nanosecond application time converted through the selected stream time base |
| Metadata display | Container/codec/geometry/rate plus frame index, time, PTS, DTS, duration, type, pixel format, bit depth, and HDR metadata |
| Clean shutdown | Cancel active work, wake the bounded command queue, join the worker, then destroy converter/session/FFmpeg state |

## Ownership and lifecycle hardening

- FFmpeg networking is initialized exactly once per process and has one
  matching deinitialization. `main` and `MediaSource` use the same idempotent
  API rather than owning independent lifetimes.
- `MediaSource::open` owns its allocated `AVFormatContext` immediately.
  Filesystem-to-UTF-8 conversion occurs while it is owned; release is limited
  to the non-throwing `avformat_open_input` call and ownership is restored
  immediately afterward.
- Playback failures retain the media-lifecycle epoch that started the
  operation. Cleanup always occurs, but an obsolete operation cannot publish
  `Error`, `mediaClosed`, or an error dialog into a newer replacement open.
- Playback scheduling never performs arithmetic on `kNoMediaTime`. Frames
  without a usable presentation timestamp use a bounded actual-duration,
  nominal-duration, or 40 ms scheduling fallback.

## Threading and UI boundary

`PlaybackController` is the asynchronous Qt adapter. A single joined worker
owns `PlaybackSession`, demuxing, decoding, seeking, prefetch, and BGRA
conversion. Frame images and immutable frame metadata cross back through
queued GUI delivery. QWidget state is touched only on the GUI thread.

The controller command queue is bounded, active FFmpeg I/O receives a
cancellation token, replacement opens and interactive seeks supersede stale
work, and frame delivery uses a one-slot latest-result channel. The
`MainWindow` explicitly destroys the controller while its GUI receivers are
still alive, ensuring the worker is joined before widget teardown.

## Phase 1 acceptance coverage

`tests/integration/Phase1FoundationTests.cpp` contains four real-media checks:

- a Matroska fixture with two differently sized video streams plus audio
  verifies deterministic default selection, explicit alternate-video
  selection, and rejection of an audio stream as the preferred video;
- missing-file and audio-only inputs verify error reporting followed by a
  successful valid-media recovery;
- a real long-GOP video verifies open, play, pause, nanosecond seek, Stop
  returning to presentation frame zero, replay, and destruction with queued
  seeks;
- an offscreen `MainWindow` verifies transport action state, media/frame
  metadata labels, duration publication, and an actual rendered viewport
  change after decoded-image delivery.

The existing application smoke gates additionally verify construction,
real-media decode/conversion/GUI delivery, and normal shutdown. Later
Phase 2 integration/stress suites continue to cover EOF drain, cancellation,
bounded queues/cache, rapid seeks, and worker joining.

Final verification on this workstation:

- MSVC Debug build: passed
- MSVC Release build: passed
- Debug CTest: 12/12 gates passed
- Release CTest: 12/12 gates passed
- focused Phase 1 gate: 5/5 executions passed, including fixture setup
- Phase 1 foundation suite: 4/4 checks passed
- repeated Debug Phase 1 gate: all five targets passed 20 consecutive runs
  each (100/100 executions)

## Phase boundary and known limits

Phase 1 is a video foundation. Audio decode/render and A/V synchronization are
later work. The current renderer is a correct CPU-converted image path;
zero-copy GPU presentation and HDR display output are optimization/later-phase
work. Hardware setup failure falls back to software, while a hardware-surface
transfer failure after successful startup currently reports a playback error
instead of rebuilding the decoder mid-stream.

For media where FFmpeg cannot establish any presentation timestamp, playback
remains safe and scheduled, but exact timestamp navigation cannot be claimed
until an authoritative timestamp or surrounding presentation identity is
available. Phase 2's stronger frame-accuracy guarantees apply where that
identity has been established.
