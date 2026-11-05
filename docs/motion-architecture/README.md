# Motion architecture qualification — GIGA

The initial coordinated-motion target is **25 kHz per axis**. Admission remains
**5 ms** and an accepted jog/override/hold must begin affecting execution within
**20 ms**. Exact pulse counts, trajectory constraints, local faults and a validated
stopping continuation after stream loss remain mandatory.

**The finite-profile electrical/refill gates pass; complete coordinated
architecture qualification remains open.** The experiments remain outside normal firmware. They do
not change production motor limits, enable robot trajectories or authorize powered
motion. All hardware tests used disconnected motors and inactive enable.

## Current evidence

- [Reproduction, commands, wiring and measurement definitions](../../examples/giga_motion_bench/README.md)
- [25 kHz measurements and image-pair identifiers](reduced-rate.json)
- [Raw capture / firmware artifact paths and hashes](reduced-rate-artifacts.json)
- [Underlying transport and reference-solver characterization](measurements.json)

Raw captures and content-addressed firmware are retained under
`build/motion-options/`; the original transport characterization is under
`build/motion-qualification/`. Keep these directories when preserving the hardware
measurement record. The report's hashes identify the exact images; benchmark code
placement has changed between experiments, so results are not interchangeable.

![Current component gates](reduced-rate-gates.png)

## What the lower target establishes

The benchmark runs immutable jerk-limited cubic profiles through the existing
TIM2/DMA STEP backend, including acceleration from rest, cruise and deceleration
to rest. Two fixtures cover equal 25 kHz peaks on all three axes and different
25/20/12.5 kHz peaks with a common time origin. They use synthetic, uncalibrated
positions, not robot mechanical limits.

Every rising edge is compared with an independent analytic trajectory in Python.
The 24 MHz analyzer also checks falling-edge counts, high times, common origin,
inactive enable and final LOW levels. Clock offset is fitted on cruise and reported
separately; the comparison retains the 100 ns STEP grid and analyzer uncertainty.

Three fresh coordinated boots pass both short fixtures. The current long capture,
using the replacement analyzer ENABLE lead and a short common ground, executes
**1,000 profiles** over **619.5 seconds wall time / 600 seconds of trajectory
time**, with **10,000,000 rising and falling edges per axis**. Every STEP edge passes
independent trajectory comparison, and **ENABLE remains HIGH/inactive throughout
all 14,919,873,024 samples**. No samples are filtered or discarded.

The current run's minimum observed refill margin is **2.805×**.
Maximum STEP residual is **2.42 analyzer samples
(101 ns)** after separating clock offset;
the allowed envelope is 4.5 samples. Relative MCU/analyzer clock offset is
approximately **+0.0139%**. Neither instrument's absolute
error is established.

Firmware reports zero STEP/DMA errors, and GUI counters report zero underruns or
errors. Registered-thread headroom in this run is at least
63.3% on M4 and 59.9% on M7.
The separate kernel-stack measurements remain below. Final heap usage is
105,224 bytes on M4 and 151,280 bytes on M7; these are benchmark
snapshots, not a production steady-state memory bound.

The strict electrical gate now passes for this fixture. Captures containing brief
ENABLE/marker disturbances, including the interrupted reseated-lead run, remain
in the artifact record and are still marked rejected. The clean result after
replacing the cable is consistent with a connection/acquisition problem; it does
not establish the exact physical cause or replace powered-driver qualification.

The deadline margin is measured for each interrupt against the time remaining
until its already armed hardware guard, before refill extends that deadline. This
includes the service/refill/callback work and accounts for time already lost before
service. Comparing the slowest interrupt in a low-speed phase to the shortest
high-speed deadline gives a more pessimistic bound, but is not the applicable
per-refill deadline. Counter storage follows the measured ISR interval. These are
observed margins, not a mathematical worst-case proof.

The workload includes the actual GUI and normal/background ICC publications plus
a calibrated fixed instruction workload requested every millisecond on M4. The
load is configured for nominal 20% M4 capacity; recorded iteration counts expose
scheduling delay. It does not qualify a real 1 kHz EtherCAT task. STEP servicing
still masks interrupts, with some measured intervals exceeding 1 ms; a production
interrupt-latency analysis remains necessary.

## Computation and communication choices

Keep geometry, FK/IK, Cartesian subdivision, lookahead and planning on M7. Keep
physical IO, final admission, fault handling, execution and stop selection on M4.
The measured fixed-point evaluator makes **compact cubic trajectories evaluated
on M4 (candidate A)** the next integration candidate at 25 kHz.

`FixedCrossing` evaluates Q32 cubic coefficients. Ambiguous comparisons use the
original double predicate. Newton supplies an estimate only; bracketed crossing
checks determine the emitted tick. Host tests compare every generated pulse in
varied acceleration and braking cases with the double reference. No trajectory
constraint or accepted timing tolerance was weakened. This is a benchmark cubic
evaluator, not a general Cartesian/quintic production replacement.

The complete M4 sequence/fallback call path and its vtable reside in instruction
RAM, with coefficients in internal D2 SRAM. The linked-image audit follows known
indirect roots as well as direct calls: 46 roots, 124 reachable functions, no
out-of-RAM functions in the inspected paths. New callbacks need their own audit.

A three-axis full stopping timestamp calculation at the reduced rate takes about
**17.6 ms on M4 / 5.62 ms on M7** in the current kernel fixture. Its M4 20 KiB stack
retains 6,124 bytes (29.9%); M7 retains 10,304 of 24,576 bytes (41.9%). Those figures
measure a whole precomputed stopping tail, not the cost of selecting or executing
a validated continuation during motion.

The general coroutine bridge still does not meet the 5 ms admission target under
the tested load. Batching and priority experiments did not remove the bottleneck.
A dedicated allocation-free native command path uses the existing SPSC/HSEM
mechanism and readiness notifications, independently of ordinary ICC dispatch.
It completed **one million 32-byte request/reply transactions** with ordinary ICC
flooding and GUI active, without corruption: maximum intended-submission-to-M4
handler latency **2.20 ms**, maximum intended round trip **2.283 ms**.

A separate three-boot native-path test with constant 3 × 100 kHz STEP, GUI, ICC
flood and nominal 20% M4 work observed at most **2.458 ms** intended admission and
exactly 600,000 pulses per axis per capture. This demonstrates the small command
path's scheduling capacity. It does not implement or measure full motion
validation, credits, action cancellation or streaming transactions. The ordinary
ICC bridge remains the application lifecycle/diagnostics/logging path.

Compact quadratic pulse runs with correction bitmaps were also screened. They
reduce data volume but their construction cost did not provide a better complete
solution than the reduced-rate cubic evaluator. They remain isolated benchmarks.

## Boundaries that still require implementation

| Requirement | Current status |
|---|---|
| 3 × 25 kHz finite jerk-limited execution | Electrical waveform/refill gate passed for the measured fixture |
| 5 ms command admission | Small native packet handler passed; full motion admission pending |
| 20 ms adjustment onset | Not qualified; current fixed-pulse buffers are too long at low speed |
| Full replenishment transaction with 2× margin | Pending codec, credits, validation, stopping data and commitment integration |
| Validated local stop after stream loss | Pending continuation selection and failure-injection tests |
| Cancellation, independent restart, stale-session rejection | Existing ICC tests pass; new coordinated executor needs its own tests |
| Production CPU/memory reserve | Pending; benchmark composition omits production motion and CLI services |
| Real EtherCAT, mechanical accuracy and calibrated limits | Outside this campaign |

The live fixture uses 64-pulse DMA halves. Both buffers represent **5.12 ms at
25 kHz**, but a larger interval at low speed. In this fixture the first 128 pulses
of acceleration span approximately **67.5 ms**. Therefore fixed pulse-count
buffering cannot be presented as a general 5.12 ms commitment guarantee or as
proof of 20 ms interactive response. The next executor must distinguish future
planning coverage from a bounded irrevocable time horizon.

Completing a preinstalled finite trajectory while M7 is idle is also not a test
of selecting a stopping branch after communication loss. Stalls, missing data,
local faults and the last-resort hardware guard require distinct tests. Reconnection
must not resume or replay motion automatically.

## Resources and scheduling

These are experimental settings, not a production resource allocation:

| Worker | Core | Priority | Stack |
|---|---|---:|---:|
| Native command path | M4 / M7 | 1 / 2 | Application command workers |
| Ordinary ICC with protected admission | M4 / M7 | 5 / 4 | 32 KiB each |
| Fixed-work reservation | M4 | 3 | 4 KiB |
| Kernel measurement | M4 / M7 | 6 | 20 / 24 KiB static |
| GUI / touch / 3D | M7 | 8 / 7 / 16 | Existing application settings |
| Flood producer/subscriber | M7 / M4 | 12 | Benchmark-only |

The native endpoint uses four-slot bounded queues, 32-byte checked messages and
HSEM 6/7. Its 64 KiB storage reservation is at `0xC0500000`, in the non-cacheable
benchmark SDRAM area. Each core additionally reserves 256 KiB workspace and
256 KiB compact-run storage. No new benchmark arrays consume the small production
UI heap.

The Release benchmark uses about **141 KiB / 190 KiB** of M4 instruction RAM and
**87,072 / 90,112 bytes** of its static-data region. The remaining static-data space
is only 3,040 bytes. M7 instruction RAM uses about 57 KiB / 64 KiB. These reservations
include unused alternate experiments; they are not a viable production memory
budget to copy wholesale. Production's roughly 10 KiB free UI heap remains a
separate constraint.

General M4 planning kernels reside in QSPI; the fixed evaluator and STEP hot paths
are in SRAM. M7 kernels reside in ITCM. `Kernels.cpp` now receives `-O2` through its
owning CMake target; earlier wrapper measurements used `-Os` because the original
directory-local source property did not apply. Preserve image hashes and actual
compiler settings when comparing measurements.

## Verification and board state

Normal and benchmark M4/M7 images build in Debug and Release. The final tests pass:
20 numerical GoogleTests, 34 pytest cases, and 16 targeted CTest entries covering
HAL, the STEP register model, motion, robot planning/execution and ICC. The 20
numerical tests are included in those CTest entries. No test relaxes the ENABLE gate.

Debug hardware completes the same ten-profile 3 × 25 kHz fixture with zero reported
errors and 2.824× measured refill margin; this is a firmware correctness check,
not an additional analyzer-qualified run. Its kernel and clock checks also pass.
The Release image pair for the repeated/long captures is
`a1b43be831895b2fa67014021c216912336f2870255e680a775b9aeac428d305`.
The final Debug benchmark pair is
`0c6189efc1900384` (full identifier in the archived runner record).

**Normal Debug firmware is restored**, pair
`9acee4be5ea50d28c1acf610b7bd02b86bf4748cb9008901b17bfa43f9ff6bab`.
The USB CLI reports UI and IO Ready, with fresh IO state. Display and remote
heap/stack diagnostics respond. SWD reads show zero CFSR/HFSR on both cores, STEP
pins LOW, and ENABLE input/output latch HIGH. All serial/debug sessions are closed.
The ordinary UI heap has about 10 KiB available. Its GUI reports zero peripheral
errors but some late frames, so this is not a GUI frame-deadline qualification.

No clang-tidy or sanitizers were run. No powered motor, physical robot or EtherCAT
stack qualification is implied. Reset-state bias-resistor testing remains separate
from the LOW/HIGH levels checked while firmware runs.
