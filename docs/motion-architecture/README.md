# Practical motion foundation — current qualification status

**The first-pulse defect is resolved. The complete motion foundation is not
qualified or fully implemented.** Worker-based execution passed short waveform
checks, but both allowed rate configurations missed the capacity targets.
Qualification stopped before endurance. Normal Debug firmware is restored;
USB reports both controllers Ready and motor drivers disabled.

## Targets and measured envelope

| Requirement | Target | Current evidence |
|---|---|---|
| Control acknowledgement | ≤50 ms | Full production motion transaction not measured |
| Jog/hold/override onset | ≤100 ms | Streaming interface not implemented |
| Local electrical fault inhibition | ≤1 ms | Not qualified; initial buffer preparation still masks interrupts |
| Numerical approximation | ≤0.1 mm | Separated from step quantization; full cubic compilation/certification incomplete |
| Refill margin | ≥2× | 12.5 kHz screen fails; 6.25 kHz screen passes |
| M4 sustained utilization, nominal 20% reservation | Target ≤90% | Both screens exceed target |
| Worker stack headroom | ≥25% | Benchmark registered-thread minimum 28.6%; complete production active workload unmeasured |
| Communication-loss stop | Preinstalled validated continuation | Host-tested local per-axis braking; Cartesian corridor and inter-core failure qualification pending |

The two Release worker screens use 64-pulse halves, a 20 ms renewal interval,
40 ms frontier and 40 ms producer lease. GUI, normal ICC state/background traffic,
and nominal 20% synthetic M4 work remain active. ENABLE is HIGH/inactive.

| Measurement | 12.5 kHz candidate | 6.25 kHz fallback |
|---|---:|---:|
| Actual peak rates, M1/M2/M3 | 12,500 / 10,000 / 6,250 Hz | 6,250 / 6,250 / 6,250 Hz |
| Moves / recorded pulses per axis | 3 / 15,000; 12,000; 7,500 | 10 / 25,000 each |
| Firmware wall interval | 1.977 s | 6.290 s |
| Minimum refill transaction margin | 1.863× | 3.237× |
| M4 CPU, including reservation/traffic | 95.08% | 93.00% |
| Registered-thread minimum headroom | 28.6% | 28.6% |
| Counts, high times, common origin, inactive enable | Pass | Pass |
| Capacity screen | Fail | Fail: utilization target |

The fallback also has a separate three-move screen with 3.64× margin and 93.41%
CPU. The longer short screen confirms that this is not only startup accounting.
CPU uses 10 ms runtime ticks divided by the whole command wall interval, rather
than the shorter waveform interval. This is aggregate utilization, **not a measured
attribution of all remaining cost to interpolation**. The result is insufficient
capacity margin under this workload, not proof that this hardware cannot control
the robot. No further rate reduction or architectural rewrite was attempted.

The fallback's synthetic worker executed 99.72% of requested 1 kHz periods. This
is a nominal instruction-work reservation; it does not establish an EtherCAT SPI
or stack deadline. Its maximum observed refill transaction includes notification
wait, worker calculation and publication: 804,504 cycles at 240 MHz (3.352 ms)
against 108,498 timer ticks at 10 MHz (10.850 ms). Maxima are measured observations,
not worst-case proofs. Start/preparation timings are local benchmark calls and
must not be reported as end-to-end command admission.

## Resolved first-pulse mechanism

STM32 DMA direct mode prefetches the first memory word when the stream is enabled.
The queued first falling edge was changed in RAM during schedule renewal, while
the old word remained cached. The timer's new first rising compare therefore
paired with the old falling compare. A captured M3 case records old rise/fall
12,781,059 / 12,781,109 and new rise/fall 12,781,030 / 12,781,080: the stale word
would give **79 ticks (7.9 µs)** instead of **50 ticks (5 µs)**.

The register model now reproduces that prefetch behavior. The fix detects a changed
cached word and reloads DMA only in a protected LOW interval with sufficient lead
time. It rebases accounting without losing emitted pulses; unsafe publication is
rejected before mutation. See the DMA direct-mode description in the local
[STM32H747 reference manual](../arduino_giga_r1_wifi/rm0399-stm32h745755-and-stm32h747757-advanced-armbased-32bit-mcus-stmicroelectronics.pdf),
§16.3.3 and §16.3.14.

One hypothesis-driven startup firmware variant was sufficient. M3 alone and all
three axes pass immutable and renewable starts, three moves per case. The empty
stopping-schedule → first-pulse boundary is also covered in the real-register
model. A ten-move scaled combined-load screen records 50,000/40,000/25,000 pulses
without a high-time violation. This startup evidence uses its identified image;
it is not a qualification of the later worker architecture.

Waveform conformance remains distinct from driver minima: the requested waveform
is 5 µs HIGH, and the capture still enforces it. No unexplained longer pulse was
accepted merely because a driver might count it. No analyzer samples were filtered.
The common-origin check includes every axis's first pulse, rather than allowing
independent phase alignment to conceal inter-axis displacement.

## Implemented boundaries

- M7 retains robot geometry, FK/IK, general planning and preview. Preview reports
  numerical approximation and step quantization separately.
- M4 retains the single timer/DMA backend and bounded profile evaluator. Routine
  refill computation is in the priority-1 worker; interrupts account and notify.
- The finite local Controller API prepares, starts, renews and stops fixed-direction,
  rest-to-rest profiles. Producer loss leaves the M4 worker executing its installed
  per-axis stopping branch. M4 worker failure invokes the guard instead.
- The fallback is centralized: 6,250 pulses/s, M1/M2 1/16 and M3 1/8. Driver settings,
  step angle, feedback scaling and configuration fingerprint stay consistent.
  Referencing is required after configuration changes.
- No new per-pulse allocations or UI-heap motion buffers were added. The shared
  waveform backend remains in use by simulation and independent motor commands.

Nominal 5:1 rotary reductions and 1.25 mm Z lead imply **140.625°/s rotary** and
**4.8828 mm/s Z** at these settings. These calculations are feasibility only;
transmissions, reference coordinates and mechanical limits remain unverified.

## Outstanding work at the stop gate

Do not present this as the requested first usable coordinated release. Remaining:

1. M7 bounded cubic compilation of general planner output, with nonzero-state joins,
   approximation/dynamics certification and explicit overflow rejection.
2. Shared trajectory prepare/start/append/replace/hold/stop/resume lifecycle with
   credits, revisions and installed path-preserving stopping corridors. Robot/ICC
   still advertises `coordinated_execution=false`; there is no new coordinated CLI.
3. Continuous execution without artificial stops, direction reversals, fractional
   carry across arbitrary spans, jog and homing integrated with that path.
4. Full production-composition timing, allocation/stack budgets and fault latency;
   initial start preparation still needs to leave interrupt-disabled sections.
5. Ordinary ICC screening against 25 ms transport / 50 ms acknowledgement budgets.
   The existing native-path experiment has not been promoted by this work.
6. Current-worker hardware stop/fault/stall/reset tests, DIR setup/hold capture,
   three fresh boots and the ten-minute combined-load run. These were deliberately
   not started after the short capacity screen failed.
7. Powered mechanical qualification with calibrated limits.

The ≤90% utilization figure is an agreed target, not a physical failure threshold.
Proceeding with less headroom requires a new explicit decision; an isolated passing
waveform must not silently authorize that change.

## Verification, resources and reproducibility

[The local API/storage contract](../../src/io/motion/EXECUTOR.md) documents ownership
and failure modes. [Benchmark instructions](../../examples/giga_motion_bench/README.md)
cover startup masks, immutable comparison and the bounded capture workflow.
[Machine-readable results](executor-results.json) record image hashes, input evidence,
resource snapshots, failed gates and tests.

The Debug host suite's 72 CTest entries pass across the full run and targeted
reruns. Native X11/OpenGL tests need normal host socket access; the initial sandbox
failures were rerun successfully. The simulated M3 count expectation now reflects
1/8 microstepping. New regressions cover DMA prefetch, worker notification/guard
behavior, rate admission and configuration propagation. Both normal GIGA Debug and
Release build, and their explicit M4 RAM-call audits find no out-of-region calls.
Clang-tidy and sanitizers were not run.

Normal Release has **1,440 bytes free in M4 FAST** and **7,904 bytes free in M4 static
RAM**. Restored Debug's idle heap snapshots are **44,832 bytes available on IO** and
**7,800 bytes on UI**. Its motion worker is present but idle; those snapshots do not
establish active-motion memory headroom. GUI/ICC remain operational with drivers
disabled and no LTDC/DMA errors in the read-only restoration check.

All new raw captures, sample summaries and content-addressed firmware are retained
under `build/motion-foundation/`. Failed attempts are retained too. `screening.json`
records CPU, refill and stack gates separately from waveform evidence; a preexisting
`result.json` is not full qualification. Keep these build artifacts when preserving
the measurement record. The ledger includes their SHA-256 hashes. Older raw-transfer,
solver and immutable-profile characterization remains in `measurements.json`,
`reduced-rate.json` and their artifact records; it does not qualify the current executor.
