# VidScope Phase 0-2 architecture

VidScope is a C++20, Qt 6, direct-FFmpeg video inspection application. This
milestone deliberately stops at the frame-accurate playback foundation;
advanced timeline, thumbnail, analysis, audio, and export systems remain
downstream consumers of these interfaces.

## Dependency direction

```text
widgets / render
        |
playback::PlaybackController (Qt adapter, GUI-thread API)
        |
playback::PlaybackSession (single decode-worker confinement)
        |
media::MediaSource -> media::Demuxer -> media::VideoDecoder
        |
                FFmpeg
```

`PlaybackSession` is synchronous and thread-confined. `PlaybackController`
owns the one `std::jthread` that calls it, accepts GUI commands through a
bounded and coalescing command queue, and posts immutable frame/image payloads
through a single coalesced GUI-delivery slot. No widget calls FFmpeg.

## Ownership and lifetime

- `MediaSource` exclusively owns `AVFormatContext` and its interrupt state.
- `Demuxer` borrows `MediaSource` and cannot outlive it.
- `VideoDecoder` exclusively owns `AVCodecContext` and the optional hardware
  device context.
- `FrameStorage` owns an `av_frame_clone` reference. `DecodedFrame` is immutable
  after publication; cache, queue, and GUI share the retained FFmpeg buffers.
- `PlaybackController` cancels active work, requests stop, wakes its condition
  variable, joins its worker, and only then destroys converter/session/FFmpeg
  state.

## Timestamp and identity invariants

- Stream timestamps remain integers in their stream time base until converted
  with `av_rescale_q` or `av_rescale_q_rnd`.
- Application time and duration are normalized to the selected video-stream
  origin. They are never computed as `frame / nominal_fps`.
- FFmpeg receive order is presentation order. DTS is metadata, not the display
  ordering key.
- A presentation index is assigned while continuity from stream start is
  established. A bounded `(presentationTime, PTS)` anchor map can reconnect an
  arbitrary seek to known indexing; repeated keys are marked ambiguous.
- Within one decode epoch, `(presentationTime, sessionSerial)` is a strict total
  order for queues and caches. Across a seek/redecode epoch, identity uses a
  known presentation index, or matching PTS/time and non-conflicting DTS plus
  exact visible-frame bytes. Padding is excluded and negative strides are
  handled explicitly.
- Equal-time, visually distinct frames remain separate presentation frames,
  including duplicates at normalized timestamp zero.

## Seeking and stepping

An arbitrary seek plans an absolute stream timestamp, seeks to or before a
safe keyframe, flushes codec state, decodes forward in presentation order,
applies the requested before/after/nearest bias, drains delayed frames at EOF,
and retains neighboring frames. New request generations cancel stale work.

Next-frame consumes the immediate buffered/cache successor. Previous-frame
uses bounded history first; on a miss it seeks to an earlier safe point,
decodes forward until the exact current-frame identity, rebuilds the local
neighborhood, and returns the true predecessor. Keyframe navigation operates
on decoded keyframe flags. Signed N-frame commands (1/2/5/10 or custom up to
1000) use the same exact presentation-order primitive.

## Memory and responsiveness

`FrameCache` enforces a hard byte budget with LRU eviction and a current-frame
pin preference. `FrameQueue` has independent item and byte bounds; head/tail
spill state is fixed-count. The controller queue is capped and merges repeated
navigation while newest seeks supersede stale seeks. Decode and BGRA conversion
run on the worker. The GUI paints an implicitly shared `QImage` and never waits
on FFmpeg directly.

## Metadata and hardware decoding

`DecodedFrame` exposes presentation identity, PTS/DTS/best-effort timestamp,
actual FFmpeg duration when present, keyframe/picture type, geometry, pixel
format, color range/space/primaries/transfer, chroma location, bit depth,
HDR10 mastering-display metadata, and MaxCLL/MaxFALL content-light metadata.
The retained `AVFrame` keeps other side data for later inspection work.

The decoder tries D3D11VA then DXVA2 on Windows and transfers hardware surfaces
to system memory for the Phase 2 renderer. Device/configuration/open failures
retry the CPU decoder, so correctness does not depend on GPU availability. A
future renderer can replace the transfer with a native-texture path without
changing frame identity.

## Phase boundary

Phase 2 includes video-only playback, precise seek, exact single/N-frame and
keyframe navigation, metadata, bounded caches, cancellation, logging,
offscreen media/UI smoke coverage, and deterministic shutdown. Audio rendering
and A/V sync, the zoomable analysis timeline, previews, analysis, inspection,
and export intentionally remain Phase 3 or later work; no placeholder data is
used for those systems.
