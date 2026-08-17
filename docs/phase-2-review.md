# Phase 2 final review

Review date: 2026-08-17

The Phase 0-2 foundation is implemented and the final gate is green. This
review was performed after the duplicate-timestamp, non-zero stream-origin,
configurable N-step, HDR10 metadata, and full media/UI smoke changes.

## Build gate

- CMake configure preset: passed (`windows-msvc`)
- Debug build: passed
- Release build: passed
- Debug CTest: 6/6 passed
- Release CTest: 6/6 passed
- Compiler warnings observed in final builds: none
- Optional Qt `WrapVulkanHeaders` was not installed; it is not used by the
  Phase 2 renderer and does not affect configure/build.

The six CTest gates are unit, synthetic-media generation, decoder/session
integration, controller/thread stress, offscreen media/UI delivery, and
immediate application construction/shutdown.

## Test inventory

- 22 unit checks
- 16 decoder/session integration checks
- 6 controller/thread stress checks
- 44 registered in-process checks total, plus two application smoke gates
- Final Debug integration suite repeated 20 consecutive times: passed
- Final Debug controller/thread/shutdown stress suite repeated 20 consecutive
  times: passed

Synthetic fixtures are generated at test time and cover:

- CFR without B-frames
- CFR with B-frame reordering and delayed EOF output
- VFR with multiple PTS deltas and multiple actual frame durations
- coarse 1/10 stream time base
- long GOP navigation/reconstruction
- two distinct frames at PTS zero and two more at the same later PTS
- positive five-second stream start normalized to application time zero

## Frame-accuracy review

- Next-frame consumes the immediate presentation-order successor; no FPS
  arithmetic is used.
- Previous-frame uses bounded decoded history and reconstructs from an earlier
  safe seek point after eviction.
- Reconstruction identifies the original across decode epochs by established
  index, or PTS/time/DTS plus exact visible image bytes. Row padding is ignored;
  negative strides are tested.
- Distinct equal-PTS frames, including a second frame at normalized time zero,
  remain independently navigable under a one-byte cache with indexing anchors
  disabled.
- Signed controller steps land on exact presentation indices; UI presets are
  1/2/5/10 with an editable 1-1000 value.
- Successful previous/next keyframe selection, failed next-keyframe state,
  seek-then-previous/next, long-GOP reverse reconstruction, VFR boundaries,
  coarse-time-base before/after/nearest bias, beginning, and EOF are tested.
- Decoder draining exposes all 24 frames of the B-frame fixture.

## Seeking and cancellation review

- Seek targets are converted with FFmpeg rational rescaling and normalized to
  the selected video-stream origin.
- Decoder buffers, packet state, forward queue, and drain state are reset after
  seek.
- New seek generations coalesce/cancel stale interactive work; delivery epochs
  prevent stale frames from reaching the GUI.
- A non-zero-start fixture verifies normalized duration, precise seek, reverse
  step, forward restoration, and duration-to-final-frame behavior.

## Memory and concurrency review

- `FrameCache` enforces its byte budget and rejects zero/oversized surfaces.
- `FrameQueue` enforces both frame-count and byte limits.
- Head/tail spill storage is fixed-count; the command queue is capped at 64;
  GUI delivery is a one-slot latest-result channel.
- All demux/decode/convert work is confined to the worker. QWidget access stays
  on the GUI thread.
- Shutdown cancels active FFmpeg I/O, wakes the worker, joins the `std::jthread`,
  and then destroys decoder/demux/source state. Rapid seek/play destruction and
  GUI-stall delivery coalescing are stress-tested.
- The offscreen media smoke opens a real B-frame file with the default hardware
  policy, decodes and converts a frame, delivers it through the Qt GUI signal,
  and exits through normal deterministic destruction.

## Metadata and hardware review

- Frame identity, timestamps, actual duration when supplied, picture/keyframe
  type, dimensions, pixel/color fields, bit depth, HDR10 mastering display, and
  MaxCLL/MaxFALL are exposed.
- D3D11VA and DXVA2 are attempted on Windows. Device/configuration/open failure
  falls back to the CPU decoder. Tests require successful decode, not a
  particular GPU or transferred pixel format.

## Deliberate limits after Phase 2

- Audio decode/render and A/V synchronization are not part of this video-only
  foundation.
- A hardware device lost after successful decoder startup currently surfaces a
  playback error; transparent mid-stream decoder recreation is future
  resilience work. Initial hardware failure already falls back to software.
- If FFmpeg supplies no per-frame duration, metadata remains zero rather than
  inventing a CFR value; scheduling uses neighboring timestamps and only then
  a nominal-rate fallback.
- Dynamic HDR payloads remain retained in `FrameStorage` but do not yet have
  dedicated high-level value types.
- Advanced timeline zoom/pan/markers, hover previews, filmstrips, analysis,
  scenes/duplicate detection, detailed inspection, and export are Phase 3+.

No Phase 3 subsystem was started, and the final code review found no blocking
frame-accuracy, ownership, bounded-memory, stale-delivery, race, or shutdown
defect.
