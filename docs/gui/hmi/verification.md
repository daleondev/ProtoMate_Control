# HMI verification

The normal firmware opens the robot Home screen on Linux and GIGA M7. This
qualification covers the HMI, its existing motion API integration, the native
simulated plant and panel-free rendering. It does **not** certify powered robot
motion. The separate ICC allocation fix and motor-disconnected overlap checks
are recorded in the [motion review](../../motion-current-state/review.md).

## Software and visual checks

- The rendering update passes all ten targeted GUI suites in Linux Debug and
  Release, including damage-region coverage, pending-frame replacement, pacing,
  ownership, presentation timeout, golden screenshots and native X11 input.
  The real `simulation.hmi` workflow also passes. Its automation waits for the
  GUI's enabled controls before clicking, rather than assuming that an IO-ready
  reply means the next telemetry update has already reached the screen.
- The real SDL-input workflow exercises initialization without auto-enable,
  enabling, homing → stop → homing, coordinated joint displacements, axis moves,
  held-jog cancellation on focus loss, raw CLI/GUI family exclusion, keypad
  cancellation, empty pages, resized letterboxing and orderly window shutdown.
  Motion goes through the application client, ICC and the native plant.
- Observed plant joints and TCP are compared with the GUI's commanded values.
  Displayed feedback is compared separately with client feedback, and its TCP
  independently recalculated. M2/M3 INDEX quantization is explicitly retained:
  one electrical cycle is four full motor steps (7.2 shaft degrees), giving
  1.44 elbow degrees and 0.025 mm Z resolution in this simulation profile. It
  is not tested or presented as precision encoder feedback.
- Presenter tests cover command mapping, units/factors, the numeric keypad,
  replacement confirmation, preview-only selection, Stop in modal/empty pages,
  Cartesian step directions and shared CAD link transforms. Application tests
  cover reserved disable/stop capacity, queue saturation, queued Enable
  cancellation, input timeout and release before delayed acceptance.
- Nineteen reviewed golden images cover the pages and ready, disabled, busy,
  unconfigured, stale and fault states. State variants are **presentation
  fixtures**, not evidence of physical faults. Async 3D pixels and changing
  graphics counters are excluded; controls and overlays remain compared.
- Native SDL/X11 interaction also passes. The 13-event software interaction
  sample gives approximately **35 ms input-to-visible-feedback p95** on this
  host. This is not a motion-admission or hardware-touch measurement.
- GIGA M4/M7 Debug and Release and the profiling image build successfully.
  Sixteen Python profiling tests pass. No clang-tidy or sanitizer run was
  performed for this rendering update.

Real workflow captures, including the referenced pose, are in [screens](screens).
For example: [Home](screens/home-referenced.png), [Axis controls](screens/axis.png),
[Robot controls](screens/robot-motion.png), [keypad](screens/keypad.png),
[I/O](screens/io.png) and [Programs](screens/programs-empty.png).

## GIGA image and memory

Verified Release image-pair identifier:

```
0cdb2fee6d5cebdf8d9055d22c560ef607a42afdcdca2ee0ca055fba15788195
```

M4 image: 1,149,828 bytes. M7 image: 2,059,732 bytes. Both were programmed and
verified. The board is left running this normal firmware on Home; both
controllers report Ready, ICC Connected and graphics verified. This image was
tested after a coordinated programming reset; a fresh USB power-cycle test was
not repeated for this update.

CPU-only engine/widget storage uses 22,912 bytes of upper ITCM. The linker
separates it from executable ITCM and bootstrap zeroes it; MPU marks it
execute-never. This leaves the ordinary M7 heap with **238,400 bytes capacity**
and approximately **39.6 KiB available** under the measured workload. No worker
stack or ICC deadline was reduced. Allocation failures report their exception
through `state` and the fault screen instead of only an internal-error number.

M7 DTCM static allocation is 110,184 / 112,640 bytes, so that region remains
tight. New stacks/state require an explicit memory budget. The hardware results
below establish the current budget, not immunity to future allocation growth.
Executable renderer hot code already occupies 16,088 bytes of ITCM. The two
colour buffers and depth buffer retain their 1,536,000-byte SDRAM allocation.
No new framebuffer, mesh storage, worker stack or heap budget was added.

## Panel-free rendering observation

Conditions: full **36,376-triangle** robot, **640×400** render surface, requested
5 fps ceiling, rotating diagnostic camera, normal GUI/ICC, repeated USB CLI
and IO-memory queries, no display panel, motors disconnected. Camera rotation
is injected diagnostic input; it is not evidence of physical touch operation.

| Measurement | Result |
|---|---:|
| Sample window | 61.10 seconds |
| New 3D images presented | 2.029 / second |
| Average 3D render wall time | 382.22 ms |
| LTDC underruns / DMA errors | 0 / 0 |
| GUI render failures / input or command overflows | 0 |
| ICC failures | 0 |
| M7 heap used, minimum–maximum | 197,776–197,848 bytes |
| Final minus initial heap use | +56 bytes |
| GUI stack headroom | 80.61% |
| Render-worker stack headroom | 62.10% |
| Touch-worker stack headroom | 92.82% |

This is an observation of adaptive pacing, **not a passed 5 fps qualification**
or a ten-minute endurance run. The full-detail hardware live view remains about
2 fps; this update does not establish smooth animation. Linux and GIGA use the
same rendering, damage and pacing policy. Faster host computation still means
different achieved throughput. The worker leaves a rest interval of one quarter
of the preceding render wall time, with a 1 ms minimum; this is not a CPU-load
measurement. There is no reduced-model or reduced-resolution fallback.

Normal Home submits only changed poses/cameras, or a fresh image when reopened.
An identical pose with a newer telemetry timestamp does not rerender. Hidden
views do not render, and an idle worker waits for new work. Benchmark modes
deliberately animate. A live idle check on this image kept the 3D render count
at 145 across two queries separated by three seconds. GUI `late` and scene maximum-latency counters
include startup and synchronization; they are distinct from LTDC underruns.
Full-detail rendering adds scene latency. Final scene age was
589 ms, and the recorded startup maximum was 1.134 seconds.
No claim of 100 ms **3D pose** latency is made. Two-dimensional control feedback
is a separate path; physical GIGA input latency is still unmeasured without the
panel.

Artifacts: `build/gui-optimization/final/{images,screening,stack,idle}.json`
and `console.log`. Raw counters and all sampled values are retained, not filtered.

## Rendering costs and scope of optimization

Telemetry updates reconcile retained widgets; unchanged text/layout does not
trigger rewriting every control. Icon strokes batch identical pixels into
runs. Sixteen bounded damage regions merge overlaps, contained rectangles and
adjacent rectangles when that adds no area. Overflow merges the least additional
area rather than copying the entire screen. Buffer ownership and copying of
unchanged regions remain intact.

The profiling build records clear, grid, each robot link, widget update, DMA
completion and framebuffer synchronization separately. A short capture plus
1,000 live DWT PC samples identifies TGX mesh/shader execution as the dominant
work (466 samples), already in ITCM. Approximate scope averages were: clear
21 ms, grid 11 ms, four links 58/40/65/12 ms, buffer synchronization 23 ms and
widget reconciliation 1.9 ms. Scope durations include preemption. This separate
diagnostic image had different render times from the ordinary image; its short
window and camera positions do not substitute for the Release results above.

Profiling artifacts, exact diagnostic ELF and trace metadata are under
`build/gui-optimization/profile/capture`, with PC samples in `profile/pc.log`.
The full-model rasterization cost remains the principal limitation. No further
detail reduction, hardware-only rendering path or timing experiment is included.

## Framebuffer inspection and verification scope

- Current Home readbacks from both SDRAM slots contain the complete shaded
  robot, text and shell; PNGs were visually inspected. The uncalibrated,
  disconnected board correctly labels this a nominal CAD pose. Live SWD reads
  are not atomic captures. Files: `build/gui-optimization/final/framebuffers`.
- Hardware startup graphics self-tests report success. Host tests cover
  missing presentation completion and shutdown. Hardware interrupt masking,
  physical cold-start and analyzer captures were not repeated for this update.
- No motor command was issued during this rendering pass. Active-motion
  qualification remains separate from these display observations.

## Remaining physical qualification

The Smartwin panel/adapter, ST1633i touch, visible color/phase/orientation,
supply sequencing and powered calibrated motion remain first-hardware checks.
Missing driver/calibration inputs on the disconnected GIGA are reported, not
replaced with simulation fixtures. The concurrent-motion ICC allocation defect is fixed and checked separately
in the motion review; no deadlines were weakened. Interactive motion workflows and GUI/CLI ownership were qualified
in Linux, rather than claimed as powered robot acceptance.
