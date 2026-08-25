# Phase 7 final review

Phase 7 turns the progressive raw motion and similarity data from Phase 6 into
a responsive timeline visualization. The implementation keeps analysis,
aggregation, and painting as separate layers so zooming or switching display
modes never invokes FFmpeg or mutates the authoritative samples.

## Delivered architecture

- `analysis::AnalysisPyramid` maintains a thread-safe temporal hierarchy with a
  configurable default grouping factor of four.
- Level zero is capped at 262,144 buckets independently of the two-million raw
  sample cap. Long videos therefore coarsen the base temporal resolution instead
  of allocating one aggregate object per frame.
- Each aggregate retains sample counts plus independent min, max, and average
  motion and similarity statistics. Raw values in `AnalysisStore` are not
  replaced or blended.
- Cache reload performs one linear hierarchy rebuild. Progressive decode batches
  replace only the touched base buckets and recompute their ancestors, avoiding
  whole-video rebuilding during analysis.
- `AnalysisManager::lodView` selects the first level that fits a caller-provided
  bucket budget and returns only populated buckets in the visible time range.
- `timeline::TimelineHeatmapRenderer` is a stateless painter. It converts a
  bounded LOD view into average-height color bars with peak indicators and does
  no aggregation, cache access, or decoding.
- `TimelineWidget` asks for no more than its device-pixel width, capped by the
  existing 4,096 timeline primitive limit.

## Modes and score semantics

- Motion mode renders average motion and uses maximum motion as the bucket peak.
- Similarity mode renders average similarity and uses maximum similarity as the
  peak.
- Combined mode blends motion with similarity difference (`1 - similarity`).
  Weights are supplied through `CombinedHeatmapWeights`, and the blend is
  normalized over only the values present in each bucket.
- The default weights retain the prompt's independent motion `0.50` and
  similarity-difference `0.30` inputs. Scene weight remains deferred with scene
  analysis rather than being fabricated in Phase 7.
- Missing analysis coverage is not painted. The timeline progressively fills as
  valid batches arrive.

The **Analysis** menu exposes an exclusive Motion / Similarity / Combined action
group. Combined is the default, and the selected mode persists through
`QSettings`.

## Long-video behavior

For a source whose estimated frame count exceeds the base-bucket cap:

```text
raw samples: up to 2,000,000
LOD level 0: up to 262,144 temporal buckets
LOD level 1: up to 65,536 buckets
LOD level 2: up to 16,384 buckets
...
```

At an overview width of 1,280 device pixels, the query walks directly to a level
with at most roughly that many candidate buckets and returns only analyzed
coverage. Paint cost is bounded by pixels, not by raw frame count.

## Acceptance coverage

`tests/unit/AnalysisTests.cpp` covers:

- four-to-one LOD construction;
- min/max/average preservation;
- replacement of a touched progressive range and ancestor refresh;
- bounded base storage for a 24-hour, ten-million-frame estimate;
- one-pixel and desktop-width primitive budgets.

`tests/integration/Phase7HeatmapTests.cpp` covers:

- real FFmpeg analysis flowing into bounded LOD views;
- aggregate sample accounting and score availability;
- a live `TimelineWidget` changing its rendered image when attached to the
  analysis source;
- Motion, Similarity, and externally weighted Combined score semantics;
- rendered output for all three modes;
- exclusive MainWindow actions and synchronous mode switching.

The final completion gate built every Debug and Release target and passed all
16 CTest entries in both configurations. The Phase 7 label covers the unit
suite, real-media heatmap integration test, and both application smoke tests.

## Explicitly deferred after Phase 7

- Phase 8: scene-score fields, scene heatmap mode, scene markers/navigation,
  duplicate sections, and freeze detection
- Phase 9: complete Frame Inspector, pixel magnifier, A/B comparison, SSIM, and
  PSNR
- Phase 10+: export, measured optimization work, audio/A-V sync, and later
  professional features
