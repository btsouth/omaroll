# Gallery loading, 3 October 2026

Gallery requests now wait for valid thumbnail dimensions. A recycled card hides
pixels from its previous file or version while the replacement loads. Visible
row calculations account for a shifted grid origin after files are inserted
or removed above the viewport. Existing four-row buffering is preserved.

## Scheduling comparison

A candidate policy admitted visible requests first, then one warmup row above
and below after an 80 ms delay and visible-request completion. It canceled
ineligible requests. Two continuous-input comparisons found a regression, so
this policy was discarded.

The environment and generated 10,000-item corpus match the
[bounded-work comparison](../2026-10-03-bounded-media/README.md). Each process
used fresh application caches; filesystem caches were uncontrolled. Metadata
indexing remained active, with source QML and llvmpipe OpenGL. Builds and tests
ran separately from measurements.

The probe sent 120 phased touchpad updates, requesting 40 pixels every 16 ms.
It verified 4,720 pixels of actual travel against 4,800 requested and waited for
the final viewport to settle. Actual timer delivery took about 10.2 seconds,
so this was not a fixed-duration input trace.

| Observation | Previous implementation | Discarded policy | Final loading fixes |
| --- | ---: | ---: | ---: |
| Ready viewport frames / submitted frames during input | 258/258; 265/265 | 21/248; 21/255 | 261/261; 263/263 |
| Input delivery duration | 10.200 to 10.208 s | 10.199 to 10.216 s | 10.174 to 10.318 s |
| Cached gallery returns, within-run median | 24.3 to 24.7 ms | 266.0 to 279.1 ms | 24.1 to 24.7 ms |
| Fresh-cache gallery jumps, within-run median | 326.0 to 326.9 ms | 299.2 to 299.9 ms | 323.7 to 328.2 ms |

Each column contains two runs, all completing 90 operations. The modest jump
reduction did not justify slower cached returns and fewer fully faded, ready
viewports during input. The final change keeps the buffering behavior and fixes
the concrete loading and recycling problems. It preserves the previous scrolling
readiness and cached-return timings; no gallery speed gain is claimed.

The compiled executable was also probed twice per revision using the same
library and fixture. Populated-gallery submitted frames took 701.7 to 705.5 ms
from main entry in the saved previous executable and 697.5 to 733.0 ms in the
final executable. The ranges overlap; this does not establish a startup gain.
Initialization checks the completed component immediately and defers later
size changes until Image's setter finishes, avoiding an unnecessary initial
callback delay while preserving valid request dimensions.

These are GUI readiness snapshots latched into submitted frames, not pixel
coverage or physical Wayland presentation measurements. Readiness includes the
200 ms thumbnail fade. Timer delivery depends on GUI and rendering work.
Generated hardlinks do not represent independent camera files or cold storage.
The earlier image-original memory reduction remains separate from this work.
