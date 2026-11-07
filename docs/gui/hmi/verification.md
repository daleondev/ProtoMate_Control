# HMI verification

The normal firmware opens the robot Home screen on Linux and GIGA M7. This
qualification covers the HMI, its existing motion API integration, the native
simulated plant and panel-free rendering. It does **not** certify powered robot
motion. The separate ICC allocation fix and motor-disconnected overlap checks
are recorded in the [motion review](../../motion-current-state/review.md).

## Software and visual checks

- Full Linux Debug and Release CTest runs: **79/79 suites passed in each build**.
  The strengthened real GUI pose workflow also passes after those full runs.
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
- Bootstrap image/MPU tests: **17 passed**, including the GUI region's zero table
  and execute-never attributes. GIGA M4/M7 Debug and Release build successfully.
  No clang-tidy or sanitizer run was performed for this integration.

Real workflow captures, including the referenced pose, are in [screens](screens).
For example: [Home](screens/home-referenced.png), [Axis controls](screens/axis.png),
[Robot controls](screens/robot-motion.png), [keypad](screens/keypad.png),
[I/O](screens/io.png) and [Programs](screens/programs-empty.png).

## GIGA image and memory

Verified Release image-pair identifier:

```
36eb011791d9645eeee5c7dc87d5af40d8d280c5bcd5750941e55b5c8205f7eb
```

M4 image: 1,128,688 bytes. M7 image: 2,036,100 bytes. Both were programmed and
verified; the final rebuild reproduces the same pair. The board is left running
this normal firmware on Home. After the user's USB power cycle, both controllers
reported Ready, ICC Connected and graphics verified, with no presentation errors.

CPU-only engine/widget storage uses 22,528 bytes of upper ITCM. The linker
separates it from executable ITCM and bootstrap zeroes it; MPU marks it
execute-never. This leaves the ordinary M7 heap with **238,400 bytes capacity**
and approximately **31–32 KiB available** under the measured workload. No worker
stack or ICC deadline was reduced. Allocation failures now report their exception
through `state` and the fault screen instead of only an internal-error number.

M7 DTCM static allocation is 110,000 / 112,640 bytes, so that region remains
tight. New stacks/state require an explicit memory budget. The hardware results
below establish the current budget, not immunity to future allocation growth.

## Ten-minute panel-free run

Conditions: full **36,376-triangle** robot, **640×400** render surface, requested
2 new 3D frames/s, rotating diagnostic camera, normal GUI/ICC, repeated USB CLI
and IO-memory queries, no display panel, motors disconnected. Camera rotation
is injected diagnostic input; it is not evidence of physical touch operation.

| Measurement | Result |
|---|---:|
| Sample window | 600.20 seconds |
| New 3D images presented | 1.908 / second |
| Average 3D render wall time | 367.50 ms |
| LTDC underruns / DMA errors | 0 / 0 |
| GUI render failures / input or command overflows | 0 |
| ICC failures | 0 |
| M7 heap used, minimum–maximum | 206,288–206,928 bytes |
| Final minus initial heap use | +104 bytes |
| GUI stack headroom | 80.76% |
| Render-worker stack headroom | 62.25% |
| Touch-worker stack headroom | 92.77% |

The bounded cadence screen selected 2 fps; 5 fps was not sustainable with the
full model. There is no reduced-model or reduced-resolution fallback. Normal
idle pages render only when dirty. GUI `late` and scene maximum-latency counters
include startup and synchronization; they are distinct from LTDC underruns.
The deliberate low 3D cadence also adds scene latency. Final scene age was
approximately 0.5 seconds, and the recorded startup maximum was 1.439 seconds.
No claim of 100 ms **3D pose** latency is made. Two-dimensional control feedback
is a separate path; physical GIGA input latency is still unmeasured without the
panel.

Artifacts: `build/hmi-validation/qualified-release/{images,screening,stack}.json`
and `console.log`. Raw counters and all sampled values are retained, not filtered.

## Framebuffers, failure handling and electrical outputs

- Both SDRAM slots passed exact RGB565 color-bar checks above the persistent
  Stop strip. Home readbacks contain the complete shaded robot, text and shell;
  exported PNGs were visually inspected. Live SWD reads are not atomic captures.
- Masking LTDC interrupts causes a bounded GUI failure, backlight duty zero and
  disabled scanout. USB CLI and ICC remain responsive. A coordinated reset
  restores normal Home; the subsequent confirmed cold power cycle also passes.
- A separate five-second **24 MHz** analyzer recording during rendering contains
  120 million samples. All three STEP signals remain LOW and D15 remains HIGH
  (drivers disabled), with **zero transitions**. This checks unintended outputs;
  it does not qualify active-motion pulse timing.

Artifacts: `build/hmi-validation/inspection`, `qualified-output/result.json`,
its raw compressed capture, and `cold-start.log`. The uncompressed analyzer SHA256
is `140e1c7a8337b15c80e5d2a649936025abb651874d833db815616b853c0f183a`.

## Remaining physical qualification

The Smartwin panel/adapter, ST1633i touch, visible color/phase/orientation,
supply sequencing and powered calibrated motion remain first-hardware checks.
Missing driver/calibration inputs on the disconnected GIGA are reported, not
replaced with simulation fixtures. The concurrent-motion ICC allocation defect is fixed and checked separately
in the motion review; no deadlines were weakened. Interactive motion workflows and GUI/CLI ownership were qualified
in Linux, rather than claimed as powered robot acceptance.
