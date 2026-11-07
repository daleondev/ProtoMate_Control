# HMI modernization verification

The normal Linux/GIGA UI uses the revised Home screen, **Axis 1/2/3** naming,
centered controls, a six-button sidebar including Alarms, focused movement tabs, I/O cards and typed diagnostic tables. Home always shows estimated feedback. Axis and Cartesian Hold are the defaults. Cartesian exposes Jog (Step/Timed/Hold), Move To and Settings, with persistent estimated feedback X/Y/Z. Timed/Hold repeat bounded finite increments through the existing planner; Step size does not affect these modes.
The same 640×400 scene, 7,914-triangle interactive model and 36,376-triangle settled
model run on both platforms. Idle poses do not rerender.

![Reviewed layout overview — presentation fixtures](screens/modern-overview.png)

## Software and visual verification

- All **13 GUI suites pass in Linux Debug and Release**: presentation, native
  X11, wheel/letterboxing/focus, pages, screenshots, gestures, graphics, detail
  switching, image ownership, robot rendering and source-asset checks.
- The separate real **simulation.hmi** workflow passes in both builds. It uses
  the actual motion client, ICC and simulated plant: initialize without enable,
  homing → stop → homing, coordinated and individual moves, held-jog cancellation, Cartesian increment repetition, release and CLI-stop without replay, selected-distance Step, Timed expiry in controller time and Cartesian Move To,
  CLI/GUI exclusion, numeric cancellation, observed joint/TCP agreement and shutdown.
- HAL architecture and simulated display-clock checks pass. The timer rollover
  test uses native monotonic time alongside the native simulated counter;
  production timing uses the unchanged runtime clock. Mixing native counter
  time with delayed hosted ThreadX ticks is not a valid rollover reference.
- **56 reviewed screenshots** cover the pages/tabs and ready, disabled, busy,
  unconfigured, stale and fault states. Seven additional pixel tests check text
  centering in tabs, large controls, multiline fields, keypad keys and sidebar icon/caption groups.
- Pinch tests cover entry/exit, reordering, near-zero separation, jitter,
  exclusions and cancellation. Bounded wheel tests cover coordinates, fractions,
  age, overflow and exactly-once delivery. Native SDL tests cover ordinary and
  flipped direction, focus loss followed by window exit, and letterboxing.
- Joint badges carry the completed frame's projected anchors and values. Tests
  retain metadata across replaced requests and immutable displayed frames;
  collision tests check bounds/non-overlap. Asynchronous scene pixels and changing
  graphics counter values are excluded narrowly from the static goldens.
- The detailed/interactive asset review retains 36 matched views with minimum
  silhouette IoU **98.358%**. Three static joint illustrations use the detailed
  registered CAD and projected positive-direction guides. Each contains only the two connected links, with original stationary-link materials and a highlighted moving link; hashes/sizes match.
- Linux native input-to-visible-feedback samples are retained in `verification.json`. The sampler observes control background pixels, avoiding unchanged text/icon ink. X11 tests require local display-socket access outside the restricted sandbox. These small samples do not establish a physical touch or motion-latency bound.
- Linux and both GIGA cores build in Debug/Release. No clang-tidy or sanitizer
  campaign was run. Targeted formatting was applied to changed C++ files.

## Final image and resource budget

Release pair programmed and verified:

```
ec10a0449dcdefa4b943a252893fd4cdde621899bec2d7ffd296c743ec3a0e8e
```

M4 image: **1,149,828 bytes**. M7 image: **2,659,116 bytes**.

| Resource | Allocation |
|---|---:|
| New compiled axis illustrations | 346,560 B QSPI |
| New navigation icons | 16,224 B QSPI |
| Renderer hot code | 16,088 B ITCM |
| Engine/widget storage | 24,000 / 24,576 B allowed |
| M7 Release DTCM static data | 110,056 / 112,640 B |
| Two 640×400 images plus depth | 1,536,000 B SDRAM |
| Ordinary M7 heap capacity | 238,400 B |

Fonts/icons/illustrations remain below the 1 MiB additional-asset budget.
No framebuffer or worker stack was added. The existing GUI/touch/render stacks
remain 16/8/16 KiB. Only **576 bytes** remain in the GUI bookkeeping allowance;
further widgets need an explicit budget, rather than unbounded page trees.

## Panel-free hardware verification

The final image was programmed, verified and started through the normal coordinated debugger reset. Both controllers reported Ready, and ICC stayed connected during the 13.46-second interactive-camera sample. A physical USB cold start was not repeated for this UI refinement.

| Measurement | Final Release result |
|---|---:|
| Newly presented 3D images/s | 3.865 |
| Mean render wall time | 195.29 ms |
| LTDC underruns / DMA errors | 0 / 0 |
| Renderer/input/command errors | 0 |
| Heap use, minimum–maximum | 199,712–199,720 B |
| Final minus initial heap use | 0 B |
| GUI / renderer / touch free stack | 70.50% / 61.07% / 92.33% |

This short sample is a health check, not a guaranteed frame rate, memory-leak proof or endurance qualification. The live 3D view still has visibly stepped animation. Physical display/touch operation remains unverified.

A 24 MHz, three-second analyzer capture records 72 million samples: all three STEP outputs LOW, enable HIGH/inactive, and zero transitions. This verifies idle outputs during graphics work, not executing-motion timing.

Both SDRAM framebuffer slots passed the diagnostic-pattern and Home readback checks. The saved [GIGA image](screens/giga-home.png) is a framebuffer readback, not a photograph of a physical display.

Exact-image manifest, counters and stacks: `build/hmi-cartesian/hardware`.
Analyzer samples/hash: `build/hmi-cartesian/idle`.
Framebuffers: `build/hmi-cartesian/framebuffers`.

## Reproduction and remaining qualification

```sh
cmake --build --preset debug-linux
ctest --test-dir build/debug-linux -R 'gui\.|simulation.hmi' --output-on-failure
cmake --build --preset giga-release
python3 scripts/giga_program.py flash --build build/giga-release
# After confirming both cores Ready, with motor power disconnected:
python3 scripts/hmi_hardware_check.py --rates 10 --seconds 30 --observe \
  --interactive --output build/hmi-check
python3 scripts/gui_hardware_inspect.py --capture-only --output build/hmi-framebuffers
python3 scripts/hmi_output_capture.py --seconds 5 --output build/hmi-idle-outputs
```

`--observe` measures throughput and checks health; the configured FPS ceiling is
not treated as promised performance. These checks do not establish motion pulse
timing, powered mechanical accuracy or physical panel behavior. The motion engine,
ICC deadlines, timer settings and interrupt priorities were not changed.

Physical RGB appearance, touch/pinch on ST1633i, supply sequencing and powered
calibrated motion remain separate first-panel/commissioning checks. Synthetic
mouse/contact tests are not evidence of physical touch operation. The existing
[motion review](../../motion-current-state/review.md) retains execution qualification.
