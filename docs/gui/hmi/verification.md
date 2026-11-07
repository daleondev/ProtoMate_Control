# HMI verification

The normal Linux and GIGA UI opens the robot Home screen. Camera/joint changes
use the **7,914-triangle interactive model**; after 500 ms without changes the
view requests one **36,376-triangle full-detail frame**, then idles. Both platforms
use the same models, 640×400 rendering surface, transforms and switching policy.
The interval starts from the last visual change, not a telemetry timestamp.
The final refinement still needs rendering time before it appears.

## Software and visual checks

- All **11 targeted GUI suites pass in Linux Debug and Release**, including
  geometry, rendering, damage coverage, scheduling, presentation timeout,
  screenshots, native X11 input and the new detail-switching test.
- The real `simulation.hmi` workflow passes through the application client, ICC
  and native plant. It exercises initialization without auto-enable, enabling,
  homing → stop → homing, coordinated joint and axis moves, held-jog cancellation,
  raw CLI/GUI family exclusion, keypad cancellation, letterboxing and shutdown.
- The input-driven detail test drags the real TouchGFX viewport using SDL events,
  observes interactive images, releases the pointer, verifies full-detail
  refinement, then requires a stable render counter while idle.
- Policy tests cover repeated pose changes, the 500 ms boundary, retrying a
  rejected submission, reopening a hidden view and restarting movement after
  refinement. Existing immutable-frame ownership and shutdown tests still pass.
- **14 Python asset tests** verify source hashes, finite/valid geometry, unit
  normals, local-coordinate bounds, omitted detail and the recorded review.
- The 36 matched TGX renders cover three articulated poses and 12 camera angles,
  including below/above views. Minimum silhouette intersection-over-union is
  **98.358%**. All views were inspected; geometry guards remain intact. This is
  sampled visual evidence, not an all-view geometric error guarantee.
- Nineteen golden HMI screenshots still match. Fixtures for readiness/fault
  presentation do not establish physical faults. Asynchronous 3D pixels remain
  excluded from these UI goldens; the matched model renders check them separately.
- The 13-event Linux Release sample gives about **38 ms** input-to-visible-feedback
  p95. This is a small software-input sample, not a hardware touch or motion
  admission measurement.
- Linux Debug/Release and both GIGA cores in Debug/Release build. No clang-tidy
  or sanitizers were run for this change.

See the [matched model comparison](../../robot/visuals/interactive/comparison.png)
and [all reviewed views and hashes](../../robot/visuals/interactive).
The [asset workflow](../../../src/ui/gui/assets/robot/README.md#interactive-detail)
records how the separate visual shell is generated. CAD, collision geometry,
kinematics and the detailed model remain unchanged.

M1 encoder feedback remains distinct from M2/M3 INDEX estimates. The existing
simulation pose checks independently compare commanded joints/TCP and feedback;
INDEX resolution is not presented as precision mechanical feedback.

## GIGA image and memory

Programmed and verified normal Release image pair:

```
7d6babb2c52284228096e168180e91245eb5455751b67b631a7f7829f9565c70
```

M4 image: **1,149,828 bytes**. M7 image: **2,257,724 bytes**.
The board remains on normal Home with both cores Ready and ICC connected.
A physical USB power cycle was not repeated in this pass.

| Resource | Allocation |
|---|---:|
| Additional compiled interactive mesh assets, QSPI | 196,280 bytes |
| Renderer hot code, ITCM | 16,088 bytes |
| Engine/widgets, execute-never upper ITCM | 22,944 / 32,768 bytes |
| M7 DTCM static data | 110,184 / 112,640 bytes |
| Two 640×400 colour images plus depth, SDRAM | 1,536,000 bytes |
| Ordinary M7 heap capacity | 238,400 bytes |

Both detail levels share the existing render buffers and workers. No extra
framebuffer, worker stack or heap reservation was added. Scene definitions are
constant flash data. DTCM remains tight; further features need an explicit budget.

## Panel-free hardware comparison

The two runs use the **same firmware**, synthetic rotating camera and normal
GUI/ICC/USB diagnostics, with motors disconnected and no panel attached. The
full-detail ceiling is 5 fps; the interactive ceiling is 10 fps. Both use adaptive
pacing with a rest of one quarter of render wall time (minimum 1 ms). This rest
policy is not a CPU-load measurement.

| Measurement | Full detail | Interactive detail |
|---|---:|---:|
| Triangles | 36,376 | 7,914 |
| Observation window | 30.75 s | 30.70 s |
| Newly presented 3D images/s | 2.049 | 3.453 |
| Mean render wall time | 381.67 ms | 220.75 ms |
| LTDC underruns / DMA errors | 0 / 0 | 0 / 0 |
| Renderer/input/command errors | 0 | 0 |
| ICC failures | 0 | 0 |
| Heap use, minimum–maximum | 197,816–197,888 B | 197,864–197,904 B |
| Final minus initial heap use | −32 B | −24 B |
| Last sampled scene age | 587 ms | 384 ms |

The lighter model delivers approximately **1.69× the frame rate**, with **42%
less render wall time**. It improves moving-view responsiveness, but **3.45 fps
is still visibly stepped animation**. Neither 10 fps delivery nor a 100 ms 3D
pose latency is established. The cadence caps are not promised throughput.
These are bounded observations, not endurance qualification. Two-dimensional
controls remain a separate rendering path.

Measured free stack headroom: **GUI 80.46%, renderer 62.05%, touch 92.82%**.
A subsequent read-only check confirms Home uses the detailed model and its 3D
render counter stays unchanged while idle. The full-detail image remains
immutable until replaced. Hardware touch cannot be tested without the panel.

Raw counters, console logs, stack data and exact image manifests:
`build/robot-interactive/hardware/{full,interactive}`; idle confirmation:
`build/robot-interactive/hardware/idle.json`.

Reproduce the comparison without motor commands:

```sh
python3 scripts/hmi_hardware_check.py --rates 5 --seconds 30 --observe \
  --output build/robot-interactive/hardware/full
python3 scripts/hmi_hardware_check.py --rates 10 --seconds 30 --observe \
  --interactive --output build/robot-interactive/hardware/interactive
```

`--observe` checks rendering/connection health while reporting actual throughput;
it does not pass a fixed-FPS requirement. Each script restores normal Home.

## Remaining qualification

Physical display power/reset timing, visible RGB/phase/orientation, touch input,
supply sequencing and powered calibrated motion remain separate checks. No
motor commands, timing changes, interrupt-priority changes or motion logic edits
were made for this work. Hardware interrupt masking and analyzer tests were not
repeated; host tests retain missing-completion and shutdown coverage.

The separate ICC allocation fix and motor-disconnected motion overlap results
remain in the [motion review](../../motion-current-state/review.md). GUI detail
switching does not replace that execution qualification.
