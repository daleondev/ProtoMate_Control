# Current motion-control review

The short electrical cases and normal Linux workflow pass. **The complete GIGA
motion API is not qualified:** concurrent continuous jog and independent-axis
motion can expire the ICC heartbeat. The two native sanitizer workflows also
remain unresolved. Passing unit tests and isolated pulse captures do not remove
these blockers.

## Implementation and regression coverage

| Area | Current behavior and verification |
|---|---|
| Homing deadline | The overall program deadline is armed at execution start. Expiry stops the current stage and prevents the next stage from starting. A regression covers multi-axis timeout. |
| Preparation admission | A mismatched protocol version is rejected before staging or hardware preparation. Ownership and revision checks remain active. |
| Explicit job wait | Stop-token notification uses the wait mutex to avoid a lost wakeup. Interrupting `motion wait` does not cancel its motion. Shutdown cannot report an unfinished job as completed. |
| Raw motion telemetry | Position/velocity reads use generator-synchronized state without waiting on the motor command mutex. A test holds startup inside that mutex and verifies telemetry remains responsive. |
| STEP scratch storage | Growing temporary preparation storage releases the old workspace before allocating the replacement. Active DMA buffers remain separate. A register-model test verifies exact edges while growing scratch for another axis. |
| Failure reporting | STEP prepare/start errors retain their stage; server allocation failures report Capacity. UI connection failures print the reason and available exception text before shutdown. |
| Native display synchronization | The native mutex masks ThreadX's suspension signal until unlock, preventing suspension of its owner while another managed thread waits on that mutex. It does not call ThreadX while holding the native lock. |
| Native model configuration | Mechanical configuration is cached before starting live simulation transport; a request no longer triggers the expensive CAD geometry calculation. |
| Static analysis and formatting | Safe mechanical lint fixes and formatting were applied to changed C++ files. Configuration hashing uses canonical serialization without a potentially throwing packet allocation. Remaining lint findings are retained, not suppressed. |

No operating envelope, ownership deadline, heartbeat deadline, motor resolution
or thread priority was relaxed as a result of these checks.

## Verification

The machine-readable [review record](review.json) contains final outcomes,
firmware identities, evidence paths and hashes. Raw captures and full logs are
retained under `build/motion-review` and the paths recorded there.

- **Normal Linux:** all 73 CTest suites/workflows pass, including the complete
  native motion API and simulator integration.
- **GIGA:** normal Debug and Release build successfully; the Release link audit
  finds zero out-of-RAM targets among 63 roots, 172 reachable functions, seven
  vtables and 20 selected ICC entry points. This is a bounded audit, not proof of
  every possible indirect call or execution-time bound.
- **Sanitizers:** 71 of 73 suites pass. `simulation.motion_api` fails a simulator
  status request; `simulation.integration` fails to complete raw homing within
  its workflow deadline. Neither emits a sanitizer memory-error report. Their
  underlying cause is not fully established; instrumentation overhead alone
  has not been proven to explain them.
- **Lint:** a full project scan was followed by a scan of all changed production
  translation units and their platform contexts: **70/70 parsed, zero blocked,
  1,899 unique findings remaining**. These include 632 initialization-style,
  421 magic-number, 230 missing-brace and 60 narrowing-conversion findings.
  Findings in included project headers are counted too. Parsing coverage does
  not mean a zero-warning result; the lint command exits unsuccessfully.
- **Formatting:** changed C++ sources pass `clang-format --dry-run --Werror`;
  `git diff --check` passes. Vendor submodules were not changed.

### Disconnected-motor logic-analyzer results

The dedicated application fixture inhibits physical enable and uses synthetic
reference inputs. The 24 MHz analyzer observes STEP1/2/3 and ENABLE. All three
captures start and end with STEP low and have zero active-enable samples.

| Test | Emitted pulses M1 / M2 / M3 | HIGH samples | Maximum inter-axis rising-edge spread |
|---|---:|---:|---:|
| Independent raw commands | 18 / 18 / 9 | 120–121 | Not a synchronized command |
| Coordinated completion | 400 / 400 / 400 | 120–121 | 1 sample |
| Early controlled stop | 266 / 266 / 266 | 120–121 | 1 sample |

At 24 MHz, one sample is 41.67 ns; observed HIGH time is 5.000–5.042 µs. This
includes analyzer sampling uncertainty. No independent timebase calibration was
performed in this review. The coordinated tests peak near 711 pulses/s, not the
configured 6,250-pulse/s ceiling. DIR wiring/setup/hold was not captured in this
pin arrangement. No maximum-rate, endurance, powered-motion or physical-switch
qualification is claimed.

The fixture uses SDRAM for M4 cold code. Normal Release uses QSPI for that code;
both retain timing-critical paths in internal RAM. The capture fixture and normal
firmware have different image identities, recorded in the review JSON.

### Unresolved application failure

After homing, a continuous shoulder jog at -0.2 degrees/s together with an
independent elbow move of -1 degree can produce `Peer heartbeat expired` on UI.
Both commands are legal members of the Axis family. The UI follows its existing
shutdown policy; M4 remains alive and handles ownership/link loss locally. No
HardFault was observed. Loss of communication is not itself proof of an exact
physical stopped position.

A debugger sample caught the M4 command worker serializing a reply while ICC
was ready. That sample does **not** isolate the cause. The missing evidence is
which execution/locking interval prevents heartbeat service for the required
window. No further endurance run is useful until that short failure is resolved.
The [failure sequence diagram](uml/10-concurrent-motion-blocker.puml) explicitly
separates observation from inference.

## Current hardware and memory state

Normal Release pair **`3be44ebcaf678647`** is restored and verified. Read-only
post-flash commands report combined/UI/IO Ready, `enabled=0`, no active family and
no owning jobs. Motion `ready=0` is expected without initialized physical drivers;
controller readiness is not motor readiness. The checks did not initialize or
energize drivers.

The idle heap snapshots report 32,920 bytes available on IO and 30,840 bytes on UI.
They are snapshots after startup/diagnostics, not minimum headroom under motion.
Normal Release uses 193,656 of 196,608 bytes in M4's fast-code region and 80,832
of 88,064 bytes in its D2 data region. The fast-code budget remains tight.
Full-workload CPU utilization, stack headroom, stop-onset latency and replenishment
margin remain unqualified while the concurrent-motion blocker is open.

## Figures and sources

- [Communication, memory, function placement and current problems (vector PDF)](current-motion-architecture.pdf)
- [Class and motion sequence diagrams (vector PDF)](uml/motion-uml.pdf)
- [Figure generator](generate.py), [class diagram](uml/05-cross-core-classes.puml),
  [finite admission](uml/06-finite-motion-admission.puml),
  [execution/stop/lease](uml/07-execution-stop-and-lease.puml),
  [raw motor path](uml/08-raw-motor-motion.puml),
  [homing lifecycle](uml/09-homing-lifecycle.puml),
  [concurrent-motion blocker](uml/10-concurrent-motion-blocker.puml).

The figures describe current source behavior and the normal Release memory map.
Their SVG metadata records supporting source hashes. PlantUML sources are retained
alongside SVG and PNG outputs. Physical calibration, contacts, torque and collision
safety remain separate commissioning work.

Reproduce the software checks from the project root:

```sh
cmake --build --preset debug-linux
ctest --test-dir build/debug-linux --output-on-failure
cmake --build --preset sanitize-linux
ctest --preset sanitize-linux
cmake --build --preset giga-debug
cmake --build --preset giga-release
python3 scripts/giga_ram_audit.py build/giga-io-release/io-controller.elf
python3 scripts/clang_tidy.py --build-dir build/debug-linux
```

The sanitizer preset supplies the hosted ThreadX signal-stack setting. Native
X11 tests require local display/socket permissions. The current failures above
are not excluded by these commands. Electrical captures use
`examples/giga_motion_api_test/capture.py` with the dedicated disabled-enable
fixture; normal firmware must not be substituted for that fixture.
