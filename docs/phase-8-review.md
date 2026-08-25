# Phase 8 final review

Phase 8 turns the compact progressive analysis stream into actionable scene,
duplicate, repeated-section, and freeze results. Detection remains separate from
decoding, storage, aggregation, and painting, so changing a threshold reuses the
current samples and never starts a second full-video pass.

## Delivered analysis data

- `VideoAnalyzer::compare` produces independent normalized motion,
  similarity, scene-change, and duplicate scores from consecutive 160x90 luma
  planes.
- Each analyzed frame retains a stable content hash and a 64-bit perceptual
  fingerprint. Exact duplicate results mean identical content at the analysis
  resolution; perceptual hashes support near and non-adjacent matching.
- `AnalysisSample`, cache schema 2, and the cache algorithm identity carry the
  new scores and fingerprints. Old Phase 6/7 documents are rejected rather than
  interpreted with missing fields.
- `AnalysisPyramid` preserves scene counts plus minimum, maximum, and average
  scene score. Scene Change and Combined timeline modes therefore keep the same
  bounded pixel-density query behavior as the earlier heatmaps.
- Default Combined weights are supplied outside rendering: motion `0.50`,
  similarity difference `0.30`, and scene change `0.20`. Raw values
  remain independently queryable.

## Bounded detection

`analysis::DetectionEngine` consumes a sample span and returns one unified
result shape with timestamp range, exact presentation indices when known, frame
count, score, and an optional matching range:

- scene cuts are thresholded local maxima with a minimum temporal separation;
- adjacent exact and near-duplicate frames are coalesced into ranges;
- frozen sequences additionally require configurable similarity, duration, and
  frame-count minima;
- non-adjacent repeated sections use bounded perceptual-fingerprint candidate
  history and forward extension;
- candidate history and results per kind have hard limits.

`AnalysisManager` publishes progressive detection snapshots at increasing
sample intervals and always rebuilds after final analysis or cache load.
Changing controls queues a high-priority detector task over the existing
bounded store. It does not invoke FFmpeg.

## User interface

- The Analysis menu now exposes Motion, Similarity, Scene Change, and Combined
  modes in one exclusive, persisted action group.
- Every detected scene is mirrored into the bounded timeline marker store.
  `Alt+Left` and `Alt+Right` navigate the same markers that are painted.
- The dockable Analysis Results panel has Scenes, Duplicates, and Freezes tabs.
  Each row shows exact frame identities/ranges when available and seeks on click.
- Scene rows request at most 48 thumbnails through the existing shared scheduler
  at background-precache priority. A new result generation cancels stale work.
- Scene, near-duplicate, freeze-similarity, and minimum-freeze controls feed
  **Reanalyze Detections**.
- `Help > About VidScope` displays the CMake-supplied application version;
  this milestone is `0.8.0`.

## Acceptance coverage

`tests/unit/AnalysisTests.cpp` covers metric/fingerprint stability, scene
peaks, exact and near-duplicate ranges, freezes, repeated-section matching,
result bounds, scene LOD statistics, and schema-2 cache round trips.

`tests/integration/Phase8DetectionTests.cpp` covers:

- real FFmpeg analysis producing hashes, normalized scene/duplicate scores,
  scene LOD data, and bounded detection results;
- threshold-only manager reanalysis and result publication;
- populated scene/duplicate/freeze tabs, result-to-seek behavior, and submitted
  threshold configuration;
- MainWindow Scene Change mode, automatic scene markers, scene-result rows, and
  enabled previous/next navigation.

The completion gate built every target with MSVC `/W4` and passed all 17
CTest entries in both Debug and Release. The unit executable contains 61 cases;
the dedicated Phase 8 executable adds four real/UI integration scenarios,
including the About-dialog version source.

## Explicitly deferred after Phase 8

- Phase 9: complete Frame Inspector, pixel magnifier, A/B comparison, SSIM, and
  PSNR
- Phase 10+: export, measured optimization work, audio/A-V sync, and later
  professional features
