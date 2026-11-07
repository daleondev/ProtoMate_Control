# Current motion-control review

The concurrent shoulder-jog / independent-elbow communication failure is fixed.
The captured exception was **`std::bad_alloc` in the IO urgent sender**, requesting
4,228 bytes in `Bridge::nextOutgoingFrame`. This terminated the IO bridge; the UI
subsequently reported a heartbeat timeout. It was not evidence of inadequate
STEP throughput or a reason to relax deadlines.

Each lane now reuses its startup-allocated sender buffer. The frame-selection
helper is synchronous, and the long-lived sender coroutine waits for work and
transport completion. No maximum-size return value crosses another coroutine
promise on every message. Transport still borrows a span until send completion.

See [the machine-readable verification record](review.json) for exact
firmware identities, logs and capture hashes. Maximum-rate and powered mechanical
qualification remain separate.

## Implementation and regression coverage

| Area | Current behavior and verification |
|---|---|
| Homing deadline | The overall program deadline is armed at execution start. Expiry stops the current stage and prevents the next stage from starting. A regression covers multi-axis timeout. |
| Preparation admission | A mismatched protocol version is rejected before staging or hardware preparation. Ownership and revision checks remain active. |
| Explicit job wait | Stop-token notification uses the wait mutex to avoid a lost wakeup. Interrupting `motion wait` does not cancel its motion. Shutdown cannot report an unfinished job as completed. |
| Raw motion telemetry | Position/velocity reads use generator-synchronized state without waiting on the motor command mutex. A test holds startup inside that mutex and verifies telemetry remains responsive. |
| STEP scratch storage | Growing temporary preparation storage releases the old workspace before allocating the replacement. Active DMA buffers remain separate. A register-model test verifies exact edges while growing scratch for another axis. |
| Failure reporting | Preparation logs identify readiness changes, reference inhibition and STEP binding failures; STEP preparation includes the underlying errno. Server allocation failures report Capacity. UI connection failure leaves a read-only fault screen with uncertain jobs marked Unknown. |
| Native display synchronization | The native mutex masks ThreadX's suspension signal until unlock, preventing suspension of its owner while another managed thread waits on that mutex. It does not call ThreadX while holding the native lock. |
| Native model configuration | Mechanical configuration is cached before starting live simulation transport; a request no longer triggers the expensive CAD geometry calculation. |
| Native scheduling | ISR acknowledgement is followed by a sequence-bound scheduler-idle handshake. The next device event waits for runnable controller work to suspend. This prevents periodic wakeups from starving homing under instrumentation. |
| Native display clock | VSYNC and pointer timestamps share the simulated peripheral clock; the SDL event pump remains on wall time. Dedicated regressions cover paused clocks, presentation ownership and clock restart. |
| Static analysis and formatting | Safe mechanical lint fixes and formatting were applied to changed C++ files. Configuration hashing uses canonical serialization without a potentially throwing packet allocation. Remaining lint findings are retained, not suppressed. |

No operating envelope, ownership deadline, heartbeat deadline, motor resolution
or thread priority was relaxed as a result of these checks.

## Verification of the ICC correction

- Normal GIGA Debug and Release builds pass. The Release linked-code audit finds
  zero out-of-RAM targets among 63 roots, 172 reachable functions, seven vtables
  and 18 selected ICC entry points. This remains a bounded audit.
- All **79 Linux Debug CTest suites/workflows pass**, including the real HMI,
  native motion API and simulator integration.
- ICC transport, service/action lifecycle, urgency, background traffic and
  connection tests pass. A new allocation regression observes `operator new`
  during heartbeats and requires **zero full-frame allocations after startup**.
- Controller and executor tests pass; 20 pytest capture/console checks pass.
- Two complete motor-disconnected application runs each include **16 repeated
  jog + independent-axis overlaps**, varied submission timing, controlled stops,
  restart, family-exclusion rejection, homing → stop → homing, coordinated moves,
  raw buffered/blended commands, speed changes and disable/reinitialize cycles.
  Final lease diagnostics report 804 and 808 acknowledgements, respectively,
  with **zero renewal failures**. No automatic replay or deadline changes.
- A separate fresh-start analyzer run captures the overlap itself. The final
  source-matched fixture is `15131d746fe890d1`; the first screening fixture
  `4216523f17883099` additionally logged STEP binding diagnostics.
- Lint and sanitizers were not rerun for this targeted correction.

### Motor-disconnected overlap capture

| Output | Rising / falling edges | HIGH samples at 24 MHz |
|---|---:|---:|
| Shoulder / M1 | 9 / 9 | 120 |
| Elbow / M2 | 44 / 44 | 120–121 |
| Z / M3 | 0 / 0 | — |

Terminal pulse accounting agrees. Every STEP output starts and ends LOW;
ENABLE stays inactive in all 51,867,648 samples. HIGH time is 5.000–5.042 µs,
including analyzer sampling uncertainty. These independent motions are not
expected to have coincident edges. The capture does not verify DIR wiring,
maximum-rate throughput, physical switches or powered motion.

The fixture uses real timer/DMA, runtime, ICC, CLI and HMI, with synthetic driver
and reference inputs and physical enable held inactive. Its M4 cold code runs
in SDRAM for debugging; normal Release uses QSPI. Timing-critical code remains
in internal RAM in both images.

### Reproducing the checks

With the fixture flashed, motors disconnected, and a fresh reset:

```sh
python3 examples/giga_motion_api_test/verify.py --overlap-cycles 16 --output build/overlap-check
# Reset before this separate capture so synthetic homing starts near its contacts.
python3 examples/giga_motion_api_test/capture.py --overlap --output build/overlap-capture
```

Analyzer: D0 → A7, D1 → D3, D2 → D2, D3 → D15, and common GND.

## Resource limits and remaining qualification

Normal Release pair **`b87bf16b221f8486`** is restored and verified: UI/IO Ready,
ICC connected, motion disabled and no owning jobs. The Home renderer reports zero
LTDC underruns, DMA errors and render failures. Idle heap snapshots show 45,680
bytes available on IO and 44,880 on UI. These are not workload minima. M4 fast code uses
192,824 / 196,608 bytes and static D2 data 80,864 / 88,064 bytes. No runtime,
STEP interrupt priority, timer, communication deadline or motor limit changed.

In the final repeated-motion run, the IO heap had 33,328 bytes available at the
terminal diagnostic sample. This is **not a minimum-free-watermark measurement**.
The ICC worker peaked at 12,104 / 20,476 stack bytes; the motor command worker
peaked at 17,792 / 20,476 bytes. The latter has only about 13% measured headroom,
so additional command stack demand requires budgeting. No stack overflow was
observed. The configured 6,250-pulse/s ceiling, worst-case latency, sustained
maximum-load headroom and powered calibrated mechanics are not established by
these short regression runs.

## Figures and sources

- [Communication, memory, function placement and current problems (vector PDF)](current-motion-architecture.pdf)
- [Class and motion sequence diagrams (vector PDF)](uml/motion-uml.pdf)
- [Figure generator](generate.py), [class diagram](uml/05-cross-core-classes.puml),
  [finite admission](uml/06-finite-motion-admission.puml),
  [execution/stop/lease](uml/07-execution-stop-and-lease.puml),
  [raw motor path](uml/08-raw-motor-motion.puml),
  [homing lifecycle](uml/09-homing-lifecycle.puml),
  [concurrent motion and ICC buffers](uml/10-concurrent-motion-blocker.puml),
  [native simulation time handoff](uml/11-native-simulation-time.puml)
  ([rendered diagram](uml/11-native-simulation-time.svg)).

The figures describe current source behavior and the normal Release memory map.
Their SVG metadata records supporting source hashes. PlantUML sources are retained
alongside SVG and PNG outputs. Physical calibration, contacts, torque and collision
safety remain separate commissioning work.

Reproduce the software checks from the project root:

```sh
cmake --build --preset debug-linux
ctest --test-dir build/debug-linux --output-on-failure
cmake --build --preset giga-debug
cmake --build --preset giga-release
python3 scripts/giga_ram_audit.py build/giga-io-release/io-controller.elf
```

Native X11 tests require local display/socket permissions. Electrical captures use
`examples/giga_motion_api_test/capture.py` with the dedicated disabled-enable
fixture; normal firmware must not be substituted for that fixture.
